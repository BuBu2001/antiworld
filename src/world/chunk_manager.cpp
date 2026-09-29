#include "world/chunk_manager.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <system_error>

#include <entt/entt.hpp>

// Jolt — только для создания HeightFieldShape в ГЛАВНОМ потоке (см. comment
// у loadChunkToGpu). Воркеры к Jolt не прикасаются.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include "core/logger.h"
#include "ecs/components.h"
#include "world/climate.h"

namespace world {

namespace {

// Перемешивание seed'а координатами чанка: один и тот же чанк при одном
// globalSeed всегда генерируется одинаково (детерминированный мир), но разные
// чанки не коррелируют.
std::uint32_t mixSeed(std::uint32_t base, std::int64_t x, std::int64_t z) noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(base) * 0x9E3779B97F4A7C15ull +
                      static_cast<std::uint64_t>(x) * 0xC2B2AE3D27D4EB4Full +
                      static_cast<std::uint64_t>(z) * 0x165667B19E3779F9ull;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<std::uint32_t>(h);
}

// double -> bits / bits -> double (для атомарной статистики).
std::uint64_t toBits(double v) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}
double fromBits(std::uint64_t bits) noexcept {
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// append с конверсией числа в строку без iostream-потоков на каждое поле.
template <class T>
void appendNum(std::string& out, T value) {
    char buf[48];
    auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}
void appendNumFixed(std::string& out, double value, int precision) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed,
                             precision);
    out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

}  // namespace

const char* toString(ChunkState state) {
    switch (state) {
        case ChunkState::Unloaded:   return "Unloaded";
        case ChunkState::Requested:  return "Requested";
        case ChunkState::Generating: return "Generating";
        case ChunkState::Ready:      return "Ready";
        case ChunkState::Loaded:     return "Loaded";
    }
    return "?";
}

// ============================== Конструкция ==============================

// Пересчёт частот шума под мировой масштаб (22 585 км на сторону).
//
// Дефолты TerrainGenerator::Config рассчитаны на 256x256 м игрушку:
//   scale = 0.005         -> длина волны рельефа ~200 м;
//   continentScale=0.0035 -> размер «материка» 1/0.0035 = 286 м.
// На мире в 22 585 км это даёт однородный мелкий шум: материков не видно,
// рельеф выглядит как статика. Здесь частоты подобраны так, чтобы:
//   * базовая октава рельефа давала форму порядка десятков км;
//   * материки были тысячи км (как настоящие);
//   * детализация рельефа не опускалась ниже шага сетки чанка (16 м).
TerrainGenerator::Config ChunkManager::Config::worldScaleTerrain() {
    TerrainGenerator::Config cfg{};
    // Рельеф: базовая волна ~20 км, 9 октав с gain 0.45 доходят до
    // 20000/2^8 = 78 м — сопоставимо со шагом сетки чанка.
    //
    // Частота записана как ЦЕЛОЕ число ячеек на kWorldExtent, а не как 1/20000.
    // Решётка Перлина стоит в целых координатах, поэтому период по X в метрах
    // равен cells/scale и совпадёт с размером мира только при целом числе
    // ячеек. 1/20000 на 22 585 км даёт 1129.25 ячейки, и край карты не смыкался:
    // скачок высоты до 319 м, и карта показывала другой рельеф, чем тот, по
    // которому идёт игрок (разница до 3.8 км). 1129 ячеек = 4.99889e-5, отличие
    // от 1/20000 в 0.02% — на глаз неотличимо, зато период точный.
    constexpr double kReliefCells = 1128.0;
    cfg.scale = kReliefCells / world::kWorldExtent;
    cfg.octaves = 9;
    cfg.lacunarity = 2.0f;
    cfg.gain = 0.45f;
    // Полуразмах рельефа: горы до ~2.5 км, равнины ±200 м. Прежние 26 м на
    // мире в 22 585 км выглядели бы как плоская равнина с рябью.
    cfg.amplitude = 900.0f;
    cfg.baseLevel = 0.0f;

    // Материки: базовая ячейка ~3.8 млн м. Три октавы лакунарности 2 =>
    // крупнейшая форма 3.8 млн м, мелкая ~0.94 млн м.
    //
    // Как и scale, это ЦЕЛОЕ число ячеек на kWorldExtent: 1/4e6 давало
    // 5.646 ячейки, то есть край карты уезжал на 1.4 млн м (256 пикселей
    // карты) от своего же периода. Шесть ячеек вместо 5.646 — это материки
    // на 6% мельче, мир при этом остаётся тем же полем шума с точным периодом.
    constexpr double kContinentCells = 6.0;
    cfg.geography.continentScale = kContinentCells / world::kWorldExtent;
    // Глубина океана 2.5 км, максимум суши 3.5 км — планетарный разброс.
    cfg.geography.oceanDepth = 2500.0f;
    cfg.geography.maxLandHeight = 3500.0f;
    // Уровень моря — ноль, чтобы суша и океан были сопоставимы по площади.
    cfg.geography.seaLevel = 0.0f;
    return cfg;
}

ChunkManager::ChunkManager(renderer::VulkanBase& renderer,
                           physics::PhysicsWorld& physics,
                           entt::registry& registry,
                           Climate* climate,
                           Config config)
    : config_(std::move(config)),
      renderer_(renderer),
      physics_(physics),
      registry_(registry),
      climate_(climate) {
    // applyWorldScaleDefaults() ЗДЕСЬ НЕ вызывается: мировые частоты уже стоят
    // в default-инициализаторе Config::terrain. Раньше вызов был здесь, и он
    // молча перетирал любую конфигурацию, заданную вызывающим, — параметр
    // выглядел рабочим, а не работал. Вызвать пересчёт явно может только сам
    // вызывающий (Config::applyWorldScaleDefaults()).
    //
    // Порядок важен: generator_ читает config_.terrain, поэтому присваивается
    // seed и создаётся генератор уже в теле конструктора (в списке
    // инициализации вложенный тип ещё не был готов).
    config_.terrain.seed = config_.seed;
    generator_ = TerrainGenerator(config_.terrain);
    pool_ = std::make_unique<ThreadPool>(config_.workerThreads);
    poolSlots_ = pool_->threadCount();
    if (config_.maxChunksInMemory == 0) config_.maxChunksInMemory = 1;
    if (config_.uploadsPerFrame == 0) config_.uploadsPerFrame = 1;
    if (config_.resolution < 2) config_.resolution = 2;
    // Jolt HeightFieldShape требует (resolution-1) кратным 4 — от этого
    // зависит обход рёбер при построении широкофазных объёмов. Без проверки
    // неправильное значение даёт либо исключение в Jolt, либо кривые коллайдеры
    // на краях чанка. Приводим к ближайшему корректному и предупреждаем.
    if ((config_.resolution - 1u) % 4u != 0u) {
        std::uint32_t fixed = 5;
        while (fixed < config_.resolution) fixed += 4;
        core::Logger::warn("ChunkManager: resolution " +
                            std::to_string(config_.resolution) +
                            " несовместимо с Jolt ((resolution-1) кратно 4), берём " +
                            std::to_string(fixed));
        config_.resolution = fixed;
    }
    core::Logger::info("ChunkManager: старт, пул " + std::to_string(poolSlots_) +
                       " потоков, LRU-кэш " + std::to_string(config_.maxChunksInMemory) +
                       " чанков, chunkSize " + std::to_string(config_.chunkSize) + " м");
}

// Делегирующая перегрузка: конфиг по умолчанию. Вынесена отдельно, потому что
// default-аргумент с вложенным типом в этом же классе не компилируется.
ChunkManager::ChunkManager(renderer::VulkanBase& renderer,
                           physics::PhysicsWorld& physics,
                           entt::registry& registry,
                           Climate* climate)
    : ChunkManager(renderer, physics, registry, climate, Config{}) {}

ChunkManager::~ChunkManager() {
    // Порядок уничтожения критичен:
    // 1) ждём завершения ВСЕХ фоновых генераций (деструктор ThreadPool join-ит
    //    воркеров; после этого ни один future не «висит», и ссылки на
    //    generator_/config_ из задач больше не нужны);
    pool_.reset();
    // 2) забираем из Task Queue ещё не загруженные ChunkData — unique_ptr
    //    освобождают heightmap/mesh данные (утечек быть не должно);
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        readyQueue_.clear();
    }
    // 3) выгружаем каждый загруженный чанк: ECS entity -> Jolt body ->
    //    Vulkan буферы -> CPU-данные. Всё это требует живых renderer_/physics_,
    //    поэтому деструктор ChunkManager обязан отрабатывать РАНЬШЕ них
    //    (объявлять объекты в main() в порядке: renderer, physics, chunks).
    for (ChunkEntry& entry : chunksLru_) {
        if (entry.loaded) {
            destroyChunkResources(*entry.loaded);
            entry.loaded.reset();
        }
        entry.state = ChunkState::Unloaded;
    }
    chunksLru_.clear();
    index_.clear();
    spatialHash_.clear();
    core::Logger::info("ChunkManager: остановлен, все ресурсы освобождены (" +
                       std::to_string(unloadsTotal_.load()) + " выгрузок всего)");
}

// ================================ update =================================

void ChunkManager::update(const awdm::dvec3& playerGlobalPosition, double viewDistance) {
    ++frameCounter_;
    // FPS стриминга: частота вызовов update (главный цикл = кадры рендера).
    const auto now = std::chrono::steady_clock::now();
    if (lastUpdateTime_ != std::chrono::steady_clock::time_point{}) {
        const double dt = std::chrono::duration<double>(now - lastUpdateTime_).count();
        if (dt > 0.0) {
            // EWMA-сглаживание, чтобы HUD не «мигал».
            updateHz_ += (1.0 / dt - updateHz_) * 0.05;
        }
    }
    lastUpdateTime_ = now;

    viewDistance = std::max(viewDistance, config_.chunkSize);

    // 1) Floating origin: держим центр рендера рядом с игроком.
    shiftOriginForStreaming(playerGlobalPosition);

    // 2) Фоновые генерации, завершившиеся с прошлого кадра -> Task Queue.
    drainCompleted();

    // 3) Загрузка готового в GPU/физику/ECS — ДОЛЖНОСТЬЮ, не блоками.
    processReadyQueue(config_.uploadsPerFrame);

    // 4) Какие чанки нужны прямо сейчас + постановка новых задач.
    markDesiredAndEnqueue(playerGlobalPosition, viewDistance);
    dispatchPendingTasks();

    // 5) Выгрузка: дальние и переполнение LRU.
    cancelStaleRequests(config_.cancelsPerFrame);
    evictOutOfSight(playerGlobalPosition, viewDistance);
    enforceLruCapacity();
}

// --------------------------- floating origin -----------------------------

void ChunkManager::shiftOriginForStreaming(const awdm::dvec3& playerGlobal) {
    // Сдвиг origin НЕ должен попадать на кадр загрузки чанка — иначе два
    // тяжёлых события в одном кадре дадут фриз. Порог редкий (раз в ~1000 м),
    // а сам сдвиг O(LRU) с дешёвыми операциями, поэтому оставляем как есть.
    if (awdm::distance2D(playerGlobal, origin_) <= config_.originThreshold) return;

    // Новый origin — позиция игрока, округлённая до сетки чанков: границы
    // чанков в локальных координатах остаются целыми, не «плывут».
    const double s = config_.chunkSize;
    awdm::dvec3 newOrigin{std::round(playerGlobal.x / s) * s, 0.0,
                          std::round(playerGlobal.z / s) * s};
    const awdm::dvec3 delta = newOrigin - origin_;
    origin_ = newOrigin;
    applyOriginShift(delta);
}

awdm::dvec3 ChunkManager::teleportOrigin(const awdm::dvec3& newOrigin) {
    const awdm::dvec3 delta = newOrigin - origin_;
    origin_ = newOrigin;
    applyOriginShift(delta);
    return delta;
}

void ChunkManager::applyOriginShift(const awdm::dvec3& delta) {
    if (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0) return;
    // Каждый подписчик пересчитывает ЛОКАЛЬНЫЕ float-координаты из глобальных
    // double — так накопление ошибки float исключено (источник истины — double).
    for (ChunkEntry& entry : chunksLru_) {
        if (!entry.loaded) continue;
        LoadedChunk& chunk = *entry.loaded;
        chunk.localCenter = awdm::toLocal(chunk.globalCenter, origin_);
        // Transform в ECS.
        if (chunk.entity != entt::null && registry_.all_of<ecs::Transform>(chunk.entity)) {
            registry_.get<ecs::Transform>(chunk.entity).position = chunk.localCenter;
        }
        // Тело Jolt (главный поток — здесь мы и так в главном).
        if (physics_.isBodyValid(chunk.body)) {
            physics_.setBodyTransform(chunk.body, chunk.localCenter, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
        }
    }
    core::Logger::info("ChunkManager: floating origin сдвинут на (" +
                       std::to_string(delta.x) + ", " + std::to_string(delta.z) + ")");
}

// ---------------------- drain: future -> task queue ----------------------

void ChunkManager::drainCompleted() {
    for (ChunkEntry& entry : chunksLru_) {
        if (entry.state != ChunkState::Generating) continue;
        if (!entry.generationFuture.valid()) continue;
        if (entry.generationFuture.wait_for(std::chrono::seconds::zero()) !=
            std::future_status::ready) {
            continue;  // всё ещё считается в воркере — не ждём!
        }

        std::unique_ptr<ChunkData> data;
        try {
            data = entry.generationFuture.get();
        } catch (const std::exception& error) {
            // Генерация упала: чанк возвращается в очередь запросов, счётчик
            // ошибок растёт. Повторная попытка произойдёт в этом же кадре.
            //
            // ВАЖНО: слот пула освобождается ДО continue. Раньше `--inflight_`
            // стоял ниже по коду, и каждый упавший future навсегда съедал один
            // слот: после poolSlots_ неудач стриминг останавливался, новые
            // чанки не генерировались, и update() молча ничего не делал.
            --inflight_;
            entry.generationFuture = {};
            ++failedGenerations_;
            entry.state = ChunkState::Requested;
            // Именно false, а не true: dispatchPendingTasks() отбирает
            // кандидатов по условию «Requested && !queuedForPool». Прежнее
            // присваивание true делало комментарий «повтор в этом же кадре»
            // ложью — чанк больше НИКОГДА не перегенерировался, и дыра в мире
            // оставалась до конца сессии.
            entry.queuedForPool = false;
            core::Logger::error(std::string("ChunkManager: генерация чанка (") +
                                std::to_string(entry.coord.x) + "," +
                                std::to_string(entry.coord.z) + ") упала: " + error.what());
            continue;
        }
        --inflight_;
        entry.generationFuture = {};

        if (!data) {
            // Пустой результат — тот же контракт, что и у exception: отпускаем
            // чанк в Requested и снимаем флаг постановки в пул, иначе он
            // зависнет навсегда.
            entry.state = ChunkState::Requested;
            entry.queuedForPool = false;
            continue;
        }

        // Обновляем статистику времени генерации (EWMA среднего и максимум).
        const double ms = data->generationSeconds * 1000.0;
        generationsTotal_.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t n = generationCountEwmaN_.fetch_add(1, std::memory_order_relaxed);
        const double prevAvg = fromBits(generationAvgBits_.load(std::memory_order_relaxed));
        const double alpha = n == 0 ? 1.0 : std::min(0.25, 1.0 / static_cast<double>(n + 1));
        generationAvgBits_.store(toBits(prevAvg + (ms - prevAvg) * alpha),
                                 std::memory_order_relaxed);
        double curMax = fromBits(generationMaxBits_.load(std::memory_order_relaxed));
        if (ms > curMax) generationMaxBits_.store(toBits(ms), std::memory_order_relaxed);

        entry.state = ChunkState::Ready;
        {
            // Передача результата воркером главному потоку: mutex +
            // condition_variable (main может ждать, если решит блокироваться;
            // в update() мы неблокирующи, CV нужен для корректной синхронизации
            // и для будущих вариантов «ждать хотя бы один кадр»).
            const std::lock_guard<std::mutex> lock(queueMutex_);
            readyQueue_.push_back(ReadyTask{std::move(data),
                                            index_.at(entry.coord)});
        }
        queueCv_.notify_all();
    }
}

// ------------------ загрузка в GPU/Jolt/ECS (главный поток) --------------

void ChunkManager::processReadyQueue(std::size_t budget) {
    for (std::size_t i = 0; i < budget; ++i) {
        ReadyTask task{{}, {}};
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            if (readyQueue_.empty()) return;  // бюджет не выбран — выход по готовности
            task = std::move(readyQueue_.front());
            readyQueue_.pop_front();
        }
        // Итератор LRU мог протухнуть? Нет: std::list стабилен, а запись
        // удаляется только когда её состояние != Generating/Ready (см.
        // dropEntry/cancelStaleRequests — там задача либо уже снята, либо
        // чанк остаётся в кэше). Тем не менее проверяем состояние defensively.
        if (task.entry == chunksLru_.end() || task.entry->state != ChunkState::Ready) {
            continue;  // чанк отменили, пока данные ждали своей очереди
        }
        loadChunkToGpu(task.entry, std::move(task.data));
    }
}

void ChunkManager::loadChunkToGpu(ChunkIt it, std::unique_ptr<ChunkData> data) {
    LoadedChunk chunk;
    chunk.coord = data->coord;
    chunk.globalCenter = awdm::chunkCenter(data->coord, config_.chunkSize);
    chunk.localCenter = awdm::toLocal(chunk.globalCenter, origin_);

    // --- Vulkan: вершинный и индексный буферы mesh. СТРОГО главный поток. ---
    chunk.mesh = renderer_.createMesh(data->model);

    // --- Jolt: HeightFieldShape-коллайдер. СТРОГО главный поток. ---
    //
    // По контракту Jolt поверхность задаётся как
    //     mOffset + mScale * (x, height[y * width + x], y),  x,y из [0, width-1]
    // то есть по X она тянется от mOffset.x до mOffset.x + cellSize*(width-1),
    // то есть шириной ровно hm.sizeX(). Тело ставится в ЦЕНТР чанка, поэтому
    // поле надо сдвинуть на половину своей ширины:
    //     mOffset.x = -sizeX() / 2
    //
    // ВАЖНО: hm.sizeX() УЖЕ в мировых единицах ((width-1) * cellSize), а не
    // число узлов. Умножать его на cellSize повторно (как пробовали) — это
    // сдвиг на -8184 м вместо -512, и тела «зависали» на 400 м над рельефом,
    // цепляясь за кусок чужого чанка.
    const Heightmap& hm = data->heightmap;
    const JPH::Vec3 colliderOffset(-0.5f * hm.sizeX(), 0.0f, -0.5f * hm.sizeZ());
    JPH::HeightFieldShapeSettings settings(hm.heights().data(), colliderOffset,
                                           JPH::Vec3(hm.cellSize(), 1.0f, hm.cellSize()),
                                           static_cast<JPH::uint>(hm.width()));
    settings.mBlockSize = 8;
    const JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) {
        core::Logger::error("ChunkManager: Jolt не создал высотное поле: " +
                            std::string(shapeResult.GetError().c_str()));
        renderer_.destroyMesh(chunk.mesh);
        chunk.mesh = nullptr;
        it->state = ChunkState::Requested;
        // Слот воркера освобождён — генерация завершилась успешно, не Jolt.
        // Ставить здесь queuedForPool = true (как было) нельзя: слот уже
        // отдан пулу, и повторная постановка в очередь навсегда теряла чанк —
        // он больше никогда не генерировался.
        it->queuedForPool = false;
        return;
    }
    chunk.body = physics_.createStaticShape(*shapeResult.Get().GetPtr(),
                                            chunk.localCenter);

    // --- EnTT: сущность чанка (Transform + MeshRenderer + RigidBody). ---
    chunk.entity = registry_.create();
    ecs::Transform transform{};
    transform.position = chunk.localCenter;
    registry_.emplace<ecs::Transform>(chunk.entity, transform);
    registry_.emplace<ecs::MeshRenderer>(chunk.entity, ecs::MeshRenderer{chunk.mesh});
    registry_.emplace<ecs::RigidBody>(chunk.entity, ecs::RigidBody{chunk.body});

    residentVertices_.fetch_add(data->model.vertices.size(), std::memory_order_relaxed);
    loadsTotal_.fetch_add(1, std::memory_order_relaxed);

    it->loaded = std::make_unique<LoadedChunk>(std::move(chunk));
    // mesh-данные больше не нужны (GPU-копия создана) — освобождаем RAM сразу;
    // heightmap остаётся: по нему спрашиваем высоту и биомы.
    it->loaded->data = std::move(data);
    {
        renderer::ModelData emptyModel;
        it->loaded->data->model.vertices.swap(emptyModel.vertices);
        std::vector<uint32_t> emptyIndices;
        it->loaded->data->model.indices.swap(emptyIndices);
    }
    it->state = ChunkState::Loaded;

    spatialHash_.insert(static_cast<SpatialHash::ChunkId>(
                            encodeChunkId(it->loaded->data->coord)),
                        awdm::chunkCenter(it->loaded->data->coord.x, config_.chunkSize),
                        awdm::chunkCenter(it->loaded->data->coord.z, config_.chunkSize));
}

// ---------------------------- desired / enqueue --------------------------

void ChunkManager::markDesiredAndEnqueue(const awdm::dvec3& playerGlobal,
                                         double viewDistance) {
    // СБРОС МЕТКИ ПРОШЛОГО КАДРА. inViewThisFrame выставляется заново ниже для
    // каждого чанка текущего кольца. Без этого сброса флаг навсегда остался бы
    // true после первого кадра, и evictOutOfSight/cancelStaleRequests никогда
    // ничего не выгружали: память росла без ограничения, а игрок, ушедший на
    // 10 км, тянул за собой весь мир.
    for (ChunkEntry& entry : chunksLru_) {
        entry.inViewThisFrame = false;
    }
    const awdm::ChunkCoord center = worldToChunk(playerGlobal);
    const auto radiusChunks =
        static_cast<std::int64_t>(std::ceil(viewDistance / config_.chunkSize));

    struct Candidate {
        awdm::ChunkCoord coord;
        double distance;
    };
    std::vector<Candidate> fresh;  // ещё не в кэше — кандидаты на генерацию

    for (std::int64_t dz = -radiusChunks; dz <= radiusChunks; ++dz) {
        for (std::int64_t dx = -radiusChunks; dx <= radiusChunks; ++dx) {
            const awdm::ChunkCoord coord{center.x + dx, center.z + dz};
            // Круг радиуса viewDistance вокруг центра чанка игрока.
            const awdm::dvec3 c = awdm::chunkCenter(coord, config_.chunkSize);
            const double dist = awdm::distance2D(c, playerGlobal);
            if (dist > viewDistance + config_.chunkSize * 0.75) continue;

            ChunkIt it = findChunk(coord);
            if (it != chunksLru_.end()) {
                touch(it);  // LRU: нужный чанк никогда не вытесняется первым
                it->inViewThisFrame = true;
                it->priorityDistance = dist;
                // Запись в кэше может остаться в состоянии Unloaded (выгружена
                // по LRU либо сброшена как stale-ready) — тогда это «пустая
                // оболочка» без данных, и повторно поставить её в очередь
                // обязан вызывающий. Раньше здесь был безусловный continue,
                // из-за чего такой чанк больше НИКОГДА не регенерировался:
                // вернувшись в радиус, игрок видел дыру в мире до конца сессии.
                if (it->state == ChunkState::Unloaded) {
                    it->state = ChunkState::Requested;
                    it->queuedForPool = false;
                    fresh.push_back({coord, dist});
                }
                continue;
            }
            fresh.push_back({coord, dist});
        }
    }

    // Ближние чанки важнее дальних: сортировка по расстоянию даёт приоритет
    // заполнения «дыр» вокруг игрока, а не далёкого кольца.
    std::sort(fresh.begin(), fresh.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.distance < b.distance;
              });
    for (const Candidate& candidate : fresh) {
        ChunkIt it = getOrInsert(candidate.coord);
        it->state = ChunkState::Requested;
        it->queuedForPool = false;
        it->inViewThisFrame = true;
        it->priorityDistance = candidate.distance;
    }
}

void ChunkManager::dispatchPendingTasks() {
    if (inflight_ >= poolSlots_) return;  // пул забит — ждём следующий кадр

    // Собираем Requested и расставляем по приоритету (ближние первыми).
    std::vector<ChunkIt> pending;
    for (ChunkList::iterator it = chunksLru_.begin(); it != chunksLru_.end(); ++it) {
        if (it->state == ChunkState::Requested && !it->queuedForPool) {
            pending.push_back(it);
        }
    }
    std::sort(pending.begin(), pending.end(), [](ChunkIt a, ChunkIt b) {
        return a->priorityDistance < b->priorityDistance;
    });

    for (ChunkIt it : pending) {
        if (inflight_ >= poolSlots_) break;
        const awdm::ChunkCoord coord = it->coord;
        // ВАЖНО: в воркер уходит ТОЛЬКО значение координат. Никаких this-
        // ссылок на Vulkan/Jolt/ECS — генерация чисто функциональна и читает
        // неизменяемые config_/generator_ (оба immutable после конструктора).
        it->generationFuture =
            pool_->submit(&ChunkManager::generateChunkData, this, coord);
        it->state = ChunkState::Generating;
        it->queuedForPool = true;
        ++inflight_;
    }
}

// ------------------------- отмена устаревших задач -----------------------

void ChunkManager::cancelStaleRequests(std::size_t budget) {
    std::size_t cancelled = 0;
    for (ChunkList::iterator it = chunksLru_.begin();
         it != chunksLru_.end() && cancelled < budget;) {
        ChunkEntry& entry = *it;
        const bool stale = !entry.inViewThisFrame && !entry.loaded;
        if (stale && entry.state == ChunkState::Requested) {
            it = dropEntry(it);  // ещё не начал считаться — просто убираем
            ++cancelled;
            continue;
        }
        if (stale && entry.state == ChunkState::Ready) {
            // Данные уже в task queue. Запись стирать нельзя (в очереди лежит
            // итератор на неё), но саму задачу обязаны убрать — иначе
            // processReadyQueue подхватит данные чанка, который вышел из
            // view distance, и загрузит GPU-меш в никуда.
            const std::lock_guard<std::mutex> lock(queueMutex_);
            for (auto q = readyQueue_.begin(); q != readyQueue_.end(); ++q) {
                if (q->entry == it) {
                    readyQueue_.erase(q);
                    break;
                }
            }
            entry.state = ChunkState::Unloaded;
            ++cancelled;
        }
        ++it;
    }
}

// ------------------------------ выгрузка ---------------------------------

bool ChunkManager::unloadChunk(const awdm::ChunkCoord& coord) {
    ChunkIt it = findChunk(coord);
    if (it == chunksLru_.end() || !it->loaded) return false;
    destroyChunkResources(*it->loaded);
    it->loaded.reset();  // вместе с heightmap (освобождение CPU-памяти)
    it->state = ChunkState::Unloaded;
    unloadsTotal_.fetch_add(1, std::memory_order_relaxed);
    spatialHash_.remove(encodeChunkId(coord));
    return true;
}

void ChunkManager::destroyChunkResources(LoadedChunk& chunk) {
    // Порядок освобождения важен и зеркален порядку создания:
    // 1) EnTT: сущность удаляется ПЕРВОЙ — RenderSystem/MovementSystem больше
    //    не увидят MeshRenderer с висящим указателем на будущую free-mesh.
    if (chunk.entity != entt::null) {
        registry_.destroy(chunk.entity);
        chunk.entity = entt::null;
    }
    // 2) Jolt: тело снимается с broadphase и уничтожается (главный поток).
    if (physics_.isBodyValid(chunk.body)) {
        physics_.removeBody(chunk.body);
    }
    chunk.body = physics::PhysicsWorld::BodyHandle{};
    // 3) Vulkan: vkDeviceWaitIdle внутри destroyMesh гарантирует, что ни одна
    //    команда рисования не использует буферы; затем VertexBuffer/IndexBuffer
    //    уничтожаются (деструкторы Mesh/Buffer — RAII, утечек нет).
    //    Счётчик вершин снимаем по фактическому размеру heightmap'а: mesh-
    //    данные освобождаются сразу после загрузки в GPU (см. loadChunkToGpu).
    if (chunk.mesh) {
        const std::uint64_t verts =
            chunk.data ? static_cast<std::uint64_t>(chunk.data->heightmap.width()) *
                             chunk.data->heightmap.depth()
                       : 0;
        residentVertices_.fetch_sub(verts, std::memory_order_relaxed);
        renderer_.destroyMesh(chunk.mesh);
        chunk.mesh = nullptr;
    }
    // 4) Сырые CPU-данные (heightmap) освобождаются вместе с unique_ptr
    //    data -> ноль аллокаций не переживает выгрузку.
}

void ChunkManager::evictOutOfSight(const awdm::dvec3& playerGlobal, double viewDistance) {
    // Хистерезис: выгружаем только то, что вышло за viewDistance + один чанк.
    // Иначе на границе радиуса чанк выгружался бы и тут же грузился обратно
    // («флиппинг»), тратя генерацию и создавая видимые подгрузки.
    const double unloadRadius = viewDistance + config_.chunkSize;
    for (ChunkList::iterator it = chunksLru_.begin(); it != chunksLru_.end();) {
        ChunkEntry& entry = *it;
        if (entry.inViewThisFrame) {
            ++it;
            continue;
        }
        if (entry.loaded) {
            const double dist = awdm::distance2D(entry.loaded->globalCenter, playerGlobal);
            if (dist <= unloadRadius) {
                ++it;
                continue;
            }
            destroyChunkResources(*entry.loaded);
            entry.loaded.reset();
            entry.state = ChunkState::Unloaded;
            unloadsTotal_.fetch_add(1, std::memory_order_relaxed);
            spatialHash_.remove(encodeChunkId(entry.coord));
        }
        // Недавний чанк оставляем в LRU «скелетом» (без данных): повторный
        // вход в радиус не будет выглядеть как новый. Записи дальше одного
        // чанка за хистерезисом удаляем полностью.
        const awdm::dvec3 c = awdm::chunkCenter(entry.coord, config_.chunkSize);
        if (awdm::distance2D(c, playerGlobal) > unloadRadius + config_.chunkSize) {
            it = dropEntry(it);
            continue;
        }
        ++it;
    }
}

void ChunkManager::enforceLruCapacity() {
    // «Больше 500 чанков в памяти» -> выгружаемLeast Recently Used. Двигаемся
    // от хвоста списка (самые давно не использовавшиеся) к голове.
    while (loadedChunkCount() > config_.maxChunksInMemory) {
        bool evicted = false;
        for (ChunkList::reverse_iterator rit = chunksLru_.rbegin();
             rit != chunksLru_.rend(); ++rit) {
            if (rit->loaded) {
                unloadChunk(rit->coord);
                lruEvictions_.fetch_add(1, std::memory_order_relaxed);
                evicted = true;
                break;
            }
        }
        if (!evicted) break;  // защищённый цикл: loadedChunkCount монотонно падает
    }
}

// ------------------------- фоновая генерация -----------------------------

std::unique_ptr<ChunkData> ChunkManager::generateChunkData(
    awdm::ChunkCoord coord) const {
    const auto t0 = std::chrono::steady_clock::now();
    const double chunkSize = config_.chunkSize;
    const std::uint32_t res = config_.resolution;
    const float cellSize = static_cast<float>(chunkSize / static_cast<double>(res - 1));

    auto data = std::make_unique<ChunkData>();
    data->coord = coord;

    // --- 1) Карта высот: fBm + глобальная континентальная маска ---
    //
    // ВАЖНО: seed шума зависит ТОЛЬКО от мира, но НЕ от координат чанка.
    // Раньше здесь стояло PerlinNoise(mixSeed(config_.seed, coord.x, coord.z)) —
    // то есть у СОСЕДНИХ чанков было РАЗНОЕ поле шума. На границе чанков
    // высоты прыгали на десятки метров: мир выглядел разорванным на куски.
    // Теперь шум — чистая функция глобальной точки, поэтому чанк, сгенерированный
    // с любой стороны границы, даёт в общей точке ту же самую высоту.
    PerlinNoise relief(config_.seed);
    PerlinNoise continents(config_.seed ^ 0x5DEECE66Du);
    const TerrainGenerator::Config& tc = config_.terrain;

    std::vector<float> heights(static_cast<std::size_t>(res) * res);
    const double gx = static_cast<double>(coord.x) * chunkSize;
    const double gz = static_cast<double>(coord.z) * chunkSize;
    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();

    const GeographyConfig& geo = tc.geography;
    // Маска «материк/океан» — ГЛОБАЛЬНАЯ функция точки, без нормализации
    // внутри чанка.
    //
    // Раньше доля воды задавалась квантилем, посчитанным ПО ВНУТРИ чанка: каждый
    // чанк растягивал себя так, чтобы ровно 62% его узлов оказалось под водой.
    // Два следствия: (1) материков не существовало вообще — каждый чанк был
    // одинаково «океаническим» независимо от координат; (2) соседние чанки
    // нормализовались к разным квантилям, поэтому береговая линия рвалась на
    // границах. Теперь знак маски решает всё: mask >= coastBias — суша,
    // иначе океан. Берег получается непрерывной кривой на всю карту.
    // Форма высоты живёт в TerrainGenerator::sampleHeightAt() — общей с
    // probeHeightAt() и с картой мира (world::WorldMap), поэтому проба спавна,
    // загруженный рельеф и карта физически не могут разойтись.
    // Уровень моря в точке: базовая географическая константа плюс локальный
    // климатический сдвиг. Именно поэтому «вода» считается по локальному
    // уровню, а не по geography.seaLevel: тёплый океан стоит на десятки метров
    // выше холодного, и берег от этого уезжает.
    const float baseSeaLevel = geo.enabled ? geo.seaLevel : 0.0f;
    const auto seaLevelAt = [this, baseSeaLevel](float wx, float wz) {
        return climate_ != nullptr ? climate_->seaLevelAt(wx, wz) : baseSeaLevel;
    };

    std::size_t waterNodes = 0;
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const double wx = gx + static_cast<double>(x) * cellSize;
            const double wz = gz + static_cast<double>(z) * cellSize;
            const std::size_t idx = static_cast<std::size_t>(z) * res + x;
            const float h = TerrainGenerator::sampleHeightAt(config_.terrain, config_.seed,
                                                             wx, wz);
            heights[idx] = h;
            if (h < seaLevelAt(static_cast<float>(wx), static_cast<float>(wz))) ++waterNodes;
        }
    }
    const float oceanFraction =
        static_cast<float>(waterNodes) / static_cast<float>(heights.size());

    for (float h : heights) {
        minHeight = std::min(minHeight, h);
        maxHeight = std::max(maxHeight, h);
    }

    Heightmap& hm = data->heightmap;
    hm = Heightmap(res, res, cellSize, std::move(heights));
    // У Heightmap остаётся БАЗОВЫЙ уровень: он нужен как опорное значение для
    // logs, physics и любых потребителей, которые не знают про климатологию.
    // Покомпонентный уровень (у каждой вершины свой) живёт в вершинах mesh.
    hm.setSeaLevel(baseSeaLevel);
    hm.setWaterFraction(oceanFraction);
    // Примечание: minHeight/maxHeight пересчитаны Heightmap из своих данных.

    // --- 2) Биомы: температура/влажность из климата (или константы) ---
    BiomeParams params = config_.biomeParams;
    fitBiomeHeights(params, minHeight, maxHeight);

    std::array<std::uint64_t, kBiomeCount> biomePixels{};
    const auto tempAt = [this](float wx, float wz) {
        return climate_ ? climate_->annualMeanTemperatureAt(wx, wz) : 12.0f;
    };
    const auto humidAt = [this](float wx, float wz) {
        return climate_ ? climate_->annualMeanHumidityAt(wx, wz) : 0.5f;
    };
    // Доля вечного льда: без климатологии ноль, то есть прежнее поведение.
    const auto iceAt = [this](float wx, float wz) {
        return climate_ ? climate_->permanentIceAt(wx, wz) : 0.0f;
    };

    // --- 3) Вершины mesh: позиция (локально, центр чанка = 0), нормаль,
    //        uv, цвет биома, snowBias, water flag. ЧИСТЫЙ CPU. ---
    data->model.vertices.resize(static_cast<std::size_t>(res) * res);
    data->model.indices.reserve(static_cast<std::size_t>(res - 1) * (res - 1) * 6);

    const auto heightClamped = [&hm, res](int x, int z) {
        const int cx = std::clamp(x, 0, static_cast<int>(res) - 1);
        const int cz = std::clamp(z, 0, static_cast<int>(res) - 1);
        return hm.heightAt(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz));
    };

    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const std::size_t idx = static_cast<std::size_t>(z) * res + x;
            const float h = hm.heightAt(x, z);
            renderer::Vertex& v = data->model.vertices[idx];

            // Локальные координаты: чанк центрирован в нуле (floating origin
            // передвинет mesh через Transform, вершины остаются маленькими).
            const float half = static_cast<float>(chunkSize) * 0.5f;
            v.position[0] = static_cast<float>(x) * cellSize - half;
            v.position[1] = h;
            v.position[2] = static_cast<float>(z) * cellSize - half;

            const float slopeX = (heightClamped(x + 1, z) - heightClamped(x - 1, z)) /
                                 (2.0f * cellSize);
            const float slopeZ = (heightClamped(x, z + 1) - heightClamped(x, z - 1)) /
                                 (2.0f * cellSize);
            const glm::vec3 normal = glm::normalize(glm::vec3(-slopeX, 1.0f, -slopeZ));
            v.normal[0] = normal.x;
            v.normal[1] = normal.y;
            v.normal[2] = normal.z;

            v.uv[0] = static_cast<float>(x) / static_cast<float>(res - 1);
            v.uv[1] = static_cast<float>(z) / static_cast<float>(res - 1);

            const double wx = gx + static_cast<double>(x) * cellSize;
            const double wz = gz + static_cast<double>(z) * cellSize;
            const float seaLevel = seaLevelAt(static_cast<float>(wx), static_cast<float>(wz));
            const bool underwater = h < seaLevel;
            v.water = underwater ? 1.0f : 0.0f;
            v.localSeaLevel = seaLevel;
            const BiomeBlend blend = sampleBiome(h, tempAt(static_cast<float>(wx),
                                                           static_cast<float>(wz)),
                                                  humidAt(static_cast<float>(wx),
                                                          static_cast<float>(wz)),
                                                  params,
                                                  iceAt(static_cast<float>(wx),
                                                        static_cast<float>(wz)));

            const glm::vec3 color = blend.color();
            v.color[0] = color.r;
            v.color[1] = color.g;
            v.color[2] = color.b;
            v.snowBias = blend.snowBias();

            if (!underwater) {
                biomePixels[static_cast<std::size_t>(blend.dominant())]++;
            }
        }
    }

    for (std::uint32_t z = 0; z + 1 < res; ++z) {
        for (std::uint32_t x = 0; x + 1 < res; ++x) {
            const auto tl = static_cast<std::uint32_t>(static_cast<std::size_t>(z) * res + x);
            const auto tr = tl + 1;
            const auto bl = static_cast<std::uint32_t>(static_cast<std::size_t>(z + 1) * res + x);
            const auto br = bl + 1;
            data->model.indices.insert(data->model.indices.end(), {tl, bl, tr, tr, bl, br});
        }
    }

    const std::uint64_t landPixels = std::max<std::uint64_t>(
        1, biomePixels[0] + biomePixels[1] + biomePixels[2] + biomePixels[3]);
    // Доля площади суши каждого биома (0..1) и доминирующий биом чанка.
    std::size_t dominantIdx = 0;
    for (std::size_t b = 0; b < kBiomeCount; ++b) {
        data->biomeCoverage[b] = static_cast<float>(biomePixels[b]) /
                                 static_cast<float>(landPixels);
        if (biomePixels[b] > biomePixels[dominantIdx]) dominantIdx = b;
    }
    data->dominantBiome = static_cast<Biome>(dominantIdx);

    data->generationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return data;
}

// ============================ вспомогательные ============================

ChunkManager::ChunkIt ChunkManager::findChunk(const awdm::ChunkCoord& coord) {
    const auto it = index_.find(coord);
    return it == index_.end() ? chunksLru_.end() : it->second;
}

ChunkManager::ChunkIt ChunkManager::getOrInsert(const awdm::ChunkCoord& coord) {
    const auto existing = index_.find(coord);
    if (existing != index_.end()) return existing->second;
    chunksLru_.push_front(ChunkEntry{});
    ChunkEntry& entry = chunksLru_.front();
    entry.coord = coord;
    const auto inserted = index_.emplace(coord, chunksLru_.begin()).first;
    return inserted->second;
}

void ChunkManager::touch(ChunkIt it) {
    if (it != chunksLru_.begin()) chunksLru_.splice(chunksLru_.begin(), chunksLru_, it);
}

ChunkManager::ChunkIt ChunkManager::dropEntry(ChunkIt it) {
    // Запись можно удалять только если её чанк не держит ресурсы и не в полёте.
    //
    // ВАЖНО: отказ и для состояния Ready. В readyQueue_ лежит ReadyTask с
    // КОПИЕЙ итератора ChunkIt на эту запись. Если erase() сделать, итератор
    // станет висячим, и processReadyQueue() разыменует освобождённый узел
    // списка (use-after-free). Поэтому Ready-записи тоже нельзя стирать, пока
    // их данные не заберут (или пока очередь не опустеет).
    if (it->loaded || it->state == ChunkState::Generating ||
        it->state == ChunkState::Ready) {
        ++it;
        return it;  // вызывающий продолжит итерацию (безопасный отказ)
    }
    index_.erase(it->coord);
    return chunksLru_.erase(it);
}

std::size_t ChunkManager::loadedChunkCount() const {
    std::size_t count = 0;
    for (const ChunkEntry& entry : chunksLru_) {
        if (entry.loaded) ++count;
    }
    return count;
}

bool ChunkManager::isLoaded(const awdm::ChunkCoord& coord) const {
    const auto it = index_.find(coord);
    return it != index_.end() && it->second->loaded != nullptr;
}

float ChunkManager::heightAtGlobal(double globalX, double globalZ) const {
    const awdm::ChunkCoord coord = awdm::worldToChunk(
        awdm::dvec3{globalX, 0.0, globalZ}, config_.chunkSize);
    const auto it = index_.find(coord);
    if (it == index_.end() || !it->second->loaded || !it->second->loaded->data) {
        return kNaNf;
    }
    const LoadedChunk& chunk = *it->second->loaded;
    const Heightmap& hm = chunk.data->heightmap;
    // Heightmap::sample() ждёт координату ОТНОСИТЕЛЬНО ЦЕНТРА чанка: mesh и
    // collider строятся с offsetX = -sizeX()/2, поэтому узел (width-1)/2 лежит
    // ровно в local 0 (см. gridX = localX/cellSize + (width-1)/2).
    //
    // Раньше здесь стояло `+ config_.chunkSize * 0.5`, то есть выборка
    // сдвигалась на ПОЛЧИНЫ чанка (512 м = 32 узла): heightAtGlobal() врала на
    // десятки метров и ставила камеру/агентов не на ту высоту, что нарисована.
    const double localX = globalX - chunk.globalCenter.x;
    const double localZ = globalZ - chunk.globalCenter.z;
    return hm.sample(static_cast<float>(localX), static_cast<float>(localZ));
}

float ChunkManager::probeHeightAt(double globalX, double globalZ) const {
    if (!std::isfinite(globalX) || !std::isfinite(globalZ)) {
        return kNaNf;
    }
    // Точная формула (без округления до центра чанка) — проба должна
    // совпадать с реальным рельефом, а не с его приближением.
    return TerrainGenerator::sampleHeightAt(config_.terrain, config_.seed,
                                            globalX, globalZ);
}

bool ChunkManager::biomeAtGlobal(double globalX, double globalZ, Biome& out) const {
    const awdm::ChunkCoord coord = awdm::worldToChunk(
        awdm::dvec3{globalX, 0.0, globalZ}, config_.chunkSize);
    const auto it = index_.find(coord);
    if (it == index_.end() || !it->second->loaded || !it->second->loaded->data) {
        return false;
    }
    out = it->second->loaded->data->dominantBiome;
    return true;
}

StreamingStats ChunkManager::stats() const {
    StreamingStats s;
    s.loadedChunks = loadedChunkCount();
    s.cachedChunks = chunksLru_.size();
    s.generatingChunks = inflight_;
    s.readyQueueSize = readyQueue_.size();
    std::size_t pending = 0;
    for (const ChunkEntry& entry : chunksLru_) {
        if (entry.state == ChunkState::Requested) ++pending;
    }
    s.pendingRequests = pending;
    s.loadsTotal = loadsTotal_.load(std::memory_order_relaxed);
    s.unloadsTotal = unloadsTotal_.load(std::memory_order_relaxed);
    s.generationsTotal = generationsTotal_.load(std::memory_order_relaxed);
    s.failedGenerations = failedGenerations_.load(std::memory_order_relaxed);
    s.lruEvictions = lruEvictions_.load(std::memory_order_relaxed);
    s.avgGenerationMs = fromBits(generationAvgBits_.load(std::memory_order_relaxed));
    s.maxGenerationMs = fromBits(generationMaxBits_.load(std::memory_order_relaxed));
    s.residentVertices = residentVertices_.load(std::memory_order_relaxed);
    s.workerThreads = poolSlots_;
    return s;
}

std::string ChunkManager::debugLine() const {
    const StreamingStats s = stats();
    std::string out;
    out.reserve(256);
    out += "chunks loaded=";
    appendNum(out, s.loadedChunks);
    out += "/";
    appendNum(out, config_.maxChunksInMemory);
    out += " gen=";
    appendNum(out, s.generatingChunks);
    out += " queued=";
    appendNum(out, s.pendingRequests);
    out += " readyQ=";
    appendNum(out, s.readyQueueSize);
    out += " loads=";
    appendNum(out, s.loadsTotal);
    out += " unloads=";
    appendNum(out, s.unloadsTotal);
    out += " lruEvict=";
    appendNum(out, s.lruEvictions);
    out += " avgGen=";
    appendNumFixed(out, s.avgGenerationMs, 1);
    out += "ms maxGen=";
    appendNumFixed(out, s.maxGenerationMs, 1);
    out += "ms verts=";
    appendNum(out, s.residentVertices);
    return out;
}

void ChunkManager::debugHud(std::string& outHud) {
    const StreamingStats s = stats();
    outHud.clear();
    outHud += "=== ChunkManager streaming ===\n";
    outHud += "FPS(update)=";
    appendNumFixed(outHud, updateHz_, 1);
    outHud += "\n";
    outHud += "Loaded chunks: ";
    appendNum(outHud, s.loadedChunks);
    outHud += " / LRU cap ";
    appendNum(outHud, config_.maxChunksInMemory);
    outHud += " (cached entries ";
    appendNum(outHud, s.cachedChunks);
    outHud += ")\n";
    outHud += "Workers: ";
    appendNum(outHud, s.workerThreads);
    outHud += " | generating: ";
    appendNum(outHud, s.generatingChunks);
    outHud += " | pending: ";
    appendNum(outHud, s.pendingRequests);
    outHud += " | ready queue: ";
    appendNum(outHud, s.readyQueueSize);
    outHud += "\n";
    outHud += "Gen time avg ";
    appendNumFixed(outHud, s.avgGenerationMs, 1);
    outHud += " ms, max ";
    appendNumFixed(outHud, s.maxGenerationMs, 1);
    outHud += " ms | failures: ";
    appendNum(outHud, s.failedGenerations);
    outHud += "\n";
    outHud += "Loads ";
    appendNum(outHud, s.loadsTotal);
    outHud += " / Unloads ";
    appendNum(outHud, s.unloadsTotal);
    outHud += " / LRU evictions ";
    appendNum(outHud, s.lruEvictions);
    outHud += "\n";
    outHud += "GPU vertices: ";
    appendNum(outHud, s.residentVertices);
    outHud += " (~";
    appendNumFixed(outHud, static_cast<double>(s.residentVertices) * sizeof(renderer::Vertex) /
                               (1024.0 * 1024.0),
                   1);
    outHud += " MB) | origin(";
    appendNumFixed(outHud, origin_.x, 0);
    outHud += ", ";
    appendNumFixed(outHud, origin_.z, 0);
    outHud += ")\n";
}

}  // namespace world
