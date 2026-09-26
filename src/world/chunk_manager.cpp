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

ChunkManager::ChunkManager(renderer::VulkanBase& renderer,
                           physics::PhysicsWorld& physics,
                           entt::registry& registry,
                           Climate* climate,
                           Config config)
    : config_(config),
      generator_([&config] {
          TerrainGenerator::Config terrain = config.terrain;
          terrain.seed = config.seed;
          return TerrainGenerator(terrain);
      }()),
      renderer_(renderer),
      physics_(physics),
      registry_(registry),
      climate_(climate),
      pool_(std::make_unique<ThreadPool>(config.workerThreads)),
      spatialHash_(config.chunkSize) {
    poolSlots_ = pool_->threadCount();
    if (config_.maxChunksInMemory == 0) config_.maxChunksInMemory = 1;
    if (config_.uploadsPerFrame == 0) config_.uploadsPerFrame = 1;
    if (config_.resolution < 2) config_.resolution = 2;
    core::Logger::info("ChunkManager: старт, пул " + std::to_string(poolSlots_) +
                       " потоков, LRU-кэш " + std::to_string(config_.maxChunksInMemory) +
                       " чанков, chunkSize " + std::to_string(config_.chunkSize) + " м");
}

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
            ++failedGenerations_;
            entry.state = ChunkState::Requested;
            entry.queuedForPool = true;
            core::Logger::error(std::string("ChunkManager: генерация чанка (") +
                                std::to_string(entry.coord.x) + "," +
                                std::to_string(entry.coord.z) + ") упала: " + error.what());
            continue;
        }
        --inflight_;
        entry.generationFuture = {};

        if (!data) {
            entry.state = ChunkState::Requested;
            entry.queuedForPool = true;
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
    const Heightmap& hm = data->heightmap;
    JPH::HeightFieldShapeSettings settings(
        hm.heights().data(),
        JPH::Vec3(-0.5f * hm.sizeX(), 0.0f, -0.5f * hm.sizeZ()),
        JPH::Vec3(hm.cellSize(), 1.0f, hm.cellSize()),
        static_cast<JPH::uint>(hm.width()));
    settings.mBlockSize = 8;
    const JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) {
        core::Logger::error("ChunkManager: Jolt не создал высотное поле: " +
                            std::string(shapeResult.GetError().c_str()));
        renderer_.destroyMesh(chunk.mesh);
        it->state = ChunkState::Requested;
        it->queuedForPool = true;
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
            // Данные уже в task queue — снимаем отметку, чтобы processReadyQueue
            // их выбросил (сама очистка deque — при drain или в деструкторе).
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

    // --- 1) Карта высот: детерминированный seed чанка + fBm + континенты ---
    PerlinNoise relief(mixSeed(config_.seed, coord.x, coord.z));
    PerlinNoise continents(mixSeed(config_.seed ^ 0x5DEECE66Du, coord.x, coord.z));
    const TerrainGenerator::Config& tc = config_.terrain;

    std::vector<float> heights(static_cast<std::size_t>(res) * res);
    const double gx = static_cast<double>(coord.x) * chunkSize;
    const double gz = static_cast<double>(coord.z) * chunkSize;
    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();

    const GeographyConfig& geo = tc.geography;
    // Масштабирование рельефа под целевую долю океана (по аналогии с
    // нормировкой в TerrainGenerator): подбираем множитель landGain так, чтобы
    // ровно targetOceanFraction узлов оказалась ниже seaLevel. Один проход по
    // сетке собирает сырые значения, второй — масштабирует.
    std::vector<float> raw(static_cast<std::size_t>(res) * res);
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const double wx = gx + static_cast<double>(x) * cellSize;
            const double wz = gz + static_cast<double>(z) * cellSize;
            // Детальный рельеф (fBm) в [-1, 1].
            const float fbm = relief.fbm2D(static_cast<float>(wx) * tc.scale,
                                           static_cast<float>(wz) * tc.scale,
                                           tc.octaves, tc.lacunarity, tc.gain);
            float h = tc.baseLevel + tc.amplitude * fbm;
            if (geo.enabled) {
                // Континентальная маска: очень крупный шум (-1..1) -> суша/вода.
                const float mask = continents.fbm2D(
                    static_cast<float>(wx) * geo.continentScale,
                    static_cast<float>(wz) * geo.continentScale, 3, 2.0f, 0.5f);
                raw[z * static_cast<std::size_t>(res) + x] =
                    h + mask * (geo.maxLandHeight + geo.oceanDepth) * 0.5f;
            } else {
                raw[z * static_cast<std::size_t>(res) + x] = h;
            }
        }
    }

    float oceanFraction = 0.0f;
    if (geo.enabled) {
        // Простая калибровка: сортировка выборки (до 4096 точек) даёт квантиль
        // для targetOceanFraction; линейный gain приводит маску к нужной доле.
        std::vector<float> sample;
        const std::size_t step = std::max<std::size_t>(1, raw.size() / 4096);
        for (std::size_t i = 0; i < raw.size(); i += step) sample.push_back(raw[i]);
        std::sort(sample.begin(), sample.end());
        const std::size_t qIdx = static_cast<std::size_t>(
            std::clamp(geo.targetOceanFraction, 0.05f, 0.95f) *
            static_cast<float>(sample.size()));
        const float q = sample[std::min(qIdx, sample.size() - 1)];
        // Сдвигаем так, чтобы квантиль уровня targetOceanFraction лёг точно на
        // береговую линию (seaLevel), и растягиваем сушу/океан по амплитудам.
        const float spread = std::max(1e-3f, sample.back() - sample.front());
        const float shoreGain = 0.5f * spread;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const float t = (raw[i] - q) / shoreGain;  // >0 суша, <0 вода
            float h;
            if (t >= 0.0f) {
                const float sharp = std::pow(std::min(t, 1.0f), 1.0f / geo.coastSharpness);
                h = geo.seaLevel + sharp * geo.maxLandHeight;
            } else {
                const float depthT = std::min(-t, 1.0f);
                h = geo.seaLevel - depthT * geo.oceanDepth;
            }
            heights[i] = h;
            if (h < geo.seaLevel) oceanFraction += 1.0f;
        }
        oceanFraction /= static_cast<float>(raw.size());
    } else {
        heights.swap(raw);
    }

    for (float h : heights) {
        minHeight = std::min(minHeight, h);
        maxHeight = std::max(maxHeight, h);
    }

    Heightmap& hm = data->heightmap;
    hm = Heightmap(res, res, cellSize, std::move(heights));
    hm.setSeaLevel(geo.enabled ? geo.seaLevel : 0.0f);
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

            const bool underwater = h < hm.seaLevel();
            v.water = underwater ? 1.0f : 0.0f;

            const double wx = gx + static_cast<double>(x) * cellSize;
            const double wz = gz + static_cast<double>(z) * cellSize;
            const BiomeBlend blend = sampleBiome(h, tempAt(static_cast<float>(wx),
                                                           static_cast<float>(wz)),
                                                 humidAt(static_cast<float>(wx),
                                                         static_cast<float>(wz)),
                                                 params);
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
    if (it->loaded || it->state == ChunkState::Generating) {
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
    const double localX = globalX - chunk.globalCenter.x + config_.chunkSize * 0.5;
    const double localZ = globalZ - chunk.globalCenter.z + config_.chunkSize * 0.5;
    return hm.sample(static_cast<float>(localX), static_cast<float>(localZ));
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
