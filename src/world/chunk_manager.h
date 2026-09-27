#pragma once

// ChunkManager — менеджер чанков бесконечного мира с асинхронным стримингом.
//
// Контекст архитектуры:
//   * мир ~510 млн км², глобальные координаты — double (core/double_math.h),
//     рендер/физика — float в локальных координатах вокруг Floating Origin;
//   * мир делится на квадратные чанки (по умолчанию 1024 м). Индексы чанков —
//     int64, поэтому координатное пространство чанков конечному размеру мира
//     не ограничивает;
//   * в памяти держится только окрестность игрока: LRU-кэш на maxChunksInMemory
//     (по ТЗ — 500) чанков. Переполнение выгружает «самые старые/далёкие».
//
// Жизненный цикл чанка (никогда не блокирует главный поток):
//
//   Requested ──пул потоков──▶ Generating ──future готов──▶ Ready ──main──▶ Loaded
//       │  submit в ThreadPool      │  (CPU-side данные)      │  upload GPU + Jolt
//       │  (главный поток)          │                         │  + EnTT (главный)
//       ▼                           ▼                         ▼
//   (отменён при выходе        (исключение -> снова       Unloading ──▶ (кэш очищен)
//    из view distance)         Requested в главном)       unloadChunk(): ECS/Jolt/GPU
//
//   * фоновые потоки ТОЛЬКО считают сырые CPU-данные (heightmap, биомы,
//     вершины/индексы mesh). Никаких Vulkan и Jolt вызовов в воркерах — обе
//     библиотеки не потокобезопасны;
//   * готовые данные передаются в главный поток через Task Queue
//     (deque + mutex + condition_variable) и забираются порциями perFrameBudget
//     — это и есть защита от фризов: за кадр загружается максимум N чанков;
//   * update(playerGlobalPosition, viewDistance) вызывается каждый кадр из
//     главного потока: считает нужное кольцо чанков, ставит задачи на генерацию
//     (в порядке близости к игроку), забирает готовые, выгружает лишние и те,
//     что не влезли в LRU-кэш.
//
// Потокобезопасность: публичные методы предназначены для главного потока;
// внутренняя синхронизация нужна только между главным и пулом рабочих потоков.
// Исключение — stats() и debugLine()/debugHud(): они читают атомарные счётчики
// и могут вызываться из HUD в любом потоке.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include "core/double_math.h"
#include "physics/physics_world.h"
#include "renderer/model_loader.h"
#include "renderer/vulkan_base.h"
#include "world/biome.h"
#include "world/spatial_hash.h"
#include "world/terrain_generator.h"
#include "world/thread_pool.h"

namespace world {

class Climate;

// Координаты чанка в сетке мира (int64 — мир огромный, float/int32 запрещены).
struct ChunkCoordHash {
    std::size_t operator()(const awdm::ChunkCoord& c) const noexcept {
        // splitmix64-лавина от обеих осей — та же схема, что в CellKeyHash.
        std::uint64_t h = static_cast<std::uint64_t>(c.x) * 0x9E3779B97F4A7C15ull ^
                          static_cast<std::uint64_t>(c.z) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

// Состояние чанка в конвейере стриминга.
enum class ChunkState : std::uint8_t {
    Unloaded,    // в кэше нет вообще (или только LRU-запись о прошлой жизни)
    Requested,   // поставлен в очередь генерации (pending), данных ещё нет
    Generating,  // задача ушла в пул потоков, future в полёте
    Ready,       // CPU-данные посчитаны, лежат в очереди задач главного потока
    Loaded,      // GPU-буферы + тело Jolt + entity ECS созданы, чанк виден
};

const char* toString(ChunkState state);

// === Сырые данные чанка (результат фоновой генерации, чисто CPU-side) ===
// Передаётся из воркера в главный поток перемещением; после загрузки mesh-
// данные освобождаются, heightmap остаётся до выгрузки (нужен для запросов
// высоты под ногами и для коллайдера).
struct ChunkData {
    awdm::ChunkCoord coord{};
    Heightmap heightmap;                 // карта высот (float, локальные метры)
    renderer::ModelData model;           // вершины/индексы mesh (локально!)
    std::array<float, kBiomeCount> biomeCoverage{};  // доли площади сушебиомов 0..1
    Biome dominantBiome{Biome::Forest};
    double generationSeconds{0.0};       // время генерации (для статистики)
};

// Упаковка координат чанка в 64-битный id для SpatialHash: int64 трактуется
// как uint64 с сохранением порядка (two's complement), поэтому смещение
// 1<<32 держит обе оси неотрицательными и инъективность гарантирована для
// |x|, |z| < 2^31 — с огромным запасом покрывает мир ~510 млн км².
inline std::uint64_t encodeChunkId(const awdm::ChunkCoord& c) noexcept {
    const auto ux = static_cast<std::uint64_t>(static_cast<std::int64_t>(c.x));
    const auto uz = static_cast<std::uint64_t>(static_cast<std::int64_t>(c.z));
    return ((ux + (1ull << 31)) << 32) | (uz + (1ull << 31));
}

// Загруженный чанк: всё, что нужно для уничтожения ресурсов при выгрузке.
struct LoadedChunk {
    awdm::ChunkCoord coord{};
    // Локальная позиция центра чанка (float, вокруг Floating Origin). Меняется
    // при shiftOriginForStreaming().
    glm::vec3 localCenter{0.0f};
    // Глобальная позиция центра (double) — источник истины для локальной.
    awdm::dvec3 globalCenter{0.0, 0.0, 0.0};

    entt::entity entity{static_cast<entt::entity>(~0u)};  // entt::null
    physics::PhysicsWorld::BodyHandle body{};             // JPH::BodyID (invalid)
    renderer::Mesh* mesh{nullptr};                        // владеет renderer

    std::unique_ptr<ChunkData> data;                      // heightmap и пр.
};

// Статистика стриминга (для консоли/HUD; поля — атомики, читается без замка).
struct StreamingStats {
    std::size_t loadedChunks{0};     // чанков с GPU/физикой в памяти
    std::size_t cachedChunks{0};     // записей LRU (включая выгруженные)
    std::size_t generatingChunks{0}; // задач генерации в полёте
    std::size_t readyQueueSize{0};   // готовых к загрузке в главном потоке
    std::size_t pendingRequests{0};  // ждут слота в пуле / снятия с генерации
    std::uint64_t loadsTotal{0};
    std::uint64_t unloadsTotal{0};
    std::uint64_t generationsTotal{0};
    std::uint64_t failedGenerations{0};
    std::uint64_t lruEvictions{0};   // вытеснений по переполнению кэша
    // Среднее/максимальное время генерации одного чанка (секунды, EWMA).
    double avgGenerationMs{0.0};
    double maxGenerationMs{0.0};
    // Вершины в GPU сейчас (грубая оценка потребления памяти).
    std::uint64_t residentVertices{0};
    unsigned workerThreads{0};
};

class ChunkManager {
public:
    struct Config {
        // Ребро чанка в метрах (global-размерность). 1024 м — компромисс:
        // мир 510 млн км² = ~487 тыс. × ~1047 тыс. чанков, индексы int64.
        // С чанком 1024 м и 64 узлами сетки шаг узла = 16 м.
        double chunkSize{1024.0};
        // Узлов карты высот на чанк по каждой оси (сетка квадратная —
        // требование Jolt HeightFieldShape). cellSize = chunkSize/(res-1).
        // Jolt требует (resolution-1) кратным 4 для своей оптимизации
        // последовательных рёбер, поэтому берём 65 (64 отрезка).
        std::uint32_t resolution{65};
        // Максимум чанков в памяти: переполнение LRU вызывает выгрузку самых
        // давно не использовавшихся (по ТЗ — 500).
        std::size_t maxChunksInMemory{500};
        // Сколько чанков загружать (GPU upload + Jolt + ECS) за один кадр.
        // Ограничивает худший случай времени кадра — защита от фризов.
        std::size_t uploadsPerFrame{4};
        // Сколько чанков снимать с генерации за кадр при выходе из радиуса.
        std::size_t cancelsPerFrame{8};
        // Число фоновых потоков генерации (0 = авто: hw_threads - 1).
        unsigned workerThreads{0};
        // Seed процедурного мира (детерминирован: тот же seed — тот же мир).
        std::uint32_t seed{PerlinNoise::kDefaultSeed};
        // Радиус интереса в метрах (view distance). 6 км при чанке 1024 м —
        // это кольцо радиусом ~6 чанков, то есть ~113 чанков в кадре.
        double viewDistance{6144.0};
        // Параметры рельефа под МИРОВОЙ масштаб.
        //
        // ВАЖНО: значения по умолчанию у TerrainGenerator::Config подобраны под
        // игрушку 256x256 м. На мире 22 585 км они давали бы рельеф с длиной
        // волны ~200 м (scale=0.005) и «материки» размером 286 м
        // (continentScale=0.0035) — то есть однородный шум без континентов.
        // Здесь частоты пересчитаны под километровые формы.
        TerrainGenerator::Config terrain{};
        // Параметры биомов для раскраски вершин.
        BiomeParams biomeParams{};
        // Порог сдвига floating origin (метры). При |origin - player| > порога
        // origin «переезжает» к игроку, а локальные координаты всех чанков
        // сдвигаются (см. core/floating_origin.h).
        double originThreshold{1000.0};
        // Базовый уровень Y для тел Jolt (коллайдер лежит на карте высот,
        // вершины которой уже содержат абсолютную высоту).
        float baseY{0.0f};

        // Пересчитывает частоты шума под мировой масштаб. Вызывается из
        // конструктора, когда terrain передана «как есть» (isWorldScaleDefault),
        // и оставляет нетронутой явно настроенную пользователем конфигурацию.
        void applyWorldScaleDefaults();
    };

    // Все внешние объекты должны переживать ChunkManager (он не владеет ими).
    // climate может быть nullptr — тогда раскраска берёт фиксированную
    // среднегодовую температуру (автономный тестовый режим).
    //
    // ВНИМАНИЕ: у вложенного Config НЕЛЬЗЯ написать default-аргумент
    // `Config config = {}` в этом же классе — вложенный класс ещё не считается
    // завершённым, и GCC отклоняет default-аргумент
    // ("could not convert braced-init-list to ChunkManager::Config").
    // Поэтому конфиг по умолчанию даёт отдельная перегрузка ниже.
    ChunkManager(renderer::VulkanBase& renderer,
                 physics::PhysicsWorld& physics,
                 entt::registry& registry,
                 Climate* climate,
                 Config config);
    // Конфигурация по умолчанию (chunkSize/resolution/LRU из Config).
    ChunkManager(renderer::VulkanBase& renderer,
                 physics::PhysicsWorld& physics,
                 entt::registry& registry,
                 Climate* climate);
    ~ChunkManager();

    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

    const Config& config() const noexcept { return config_; }

    // === Главный вход каждый кадр (ВЫЗЫВАТЬ ИЗ ГЛАВНОГО ПОТОКА) ===
    //
    // playerGlobalPosition — позиция игрока в глобальных double-координатах.
    // viewDistance — радиус интереса в метрах (например 4096 = 4 чанка).
    //
    // Порядок работы внутри одного вызова (ничего не блокируется):
    //   1) floating origin: при необходимости сдвиг origin к игроку и
    //      пересчёт локальных позиций загруженных чанков;
    //   2) drainCompleted(): забираем из future'ов готовые ChunkData и кладём
    //      их в Task Queue;
    //   3) processReadyQueue(): загружаем до uploadsPerFrame чанк в кадр
    //      (Vulkan/Jolt/ECS — только здесь, только в главном потоке);
    //   4) desired set: помечаем использованные LRU, новые чанки кольца
    //      отправляем в генерацию (ближние первыми, дальние — если есть
    //      свободные слоты пула);
    //   5) выгрузка: чанки вне viewDistance и всё, что не влезло в LRU-кэш.
    void update(const awdm::dvec3& playerGlobalPosition, double viewDistance);

    // Принудительно выгружает чанк (освобождает GPU/физику/ECS/память).
    // Идемпотентно: несуществующий чанк — false.
    bool unloadChunk(const awdm::ChunkCoord& coord);

    // Высота ландшафта под глобальной точкой (XZ), если чанк загружен;
    // иначе NaN. Нужна логике игры (спавн объектов, raycast «под ногами»).
    float heightAtGlobal(double globalX, double globalZ) const;

    // Высота рельефа в ЛЮБОЙ глобальной точке, даже если чанк не загружен.
    //
    // Нужна для поиска суши при спавне: viewDistance ограничен (6 км), а суша
    // может быть за 30 км, и heightAtGlobal() вернул бы NaN. Здесь координаты
    // округляются до центра чанка и высота берётся напрямую из той же формулы
    // генерации, что и при загрузке, — поэтому результат совпадает с реальным
    // рельефом. Точность — до размера чанка (шаг ~1 м), чего достаточно для
    // выбора точки спавна.
    // Возвращает NaN только при нечисловых координатах.
    float probeHeightAt(double globalX, double globalZ) const;

    // Доминирующий биом в глобальной точке (NaN-биом, если чанк не загружен).
    bool biomeAtGlobal(double globalX, double globalZ, Biome& out) const;

    // === Floating Origin ===
    const awdm::dvec3& origin() const noexcept { return origin_; }
    // Экстренный телепорт origin (сейв/лоад, спавн). Возвращает delta сдвига.
    awdm::dvec3 teleportOrigin(const awdm::dvec3& newOrigin);

    // === Диагностика ===
    StreamingStats stats() const;
    // Однострочник для периодического вывода в консоль.
    std::string debugLine() const;
    // Простой текстовый HUD (мутирует строку in-place, без аллокаций кадра).
    // Вызывать из главного потока перед печатью/выводом.
    void debugHud(std::string& outHud);

    std::size_t loadedChunkCount() const;
    bool isLoaded(const awdm::ChunkCoord& coord) const;

private:
    // LRU-запись: состояние чанка + порядок использования. list<> даёт O(1)
    // move-to-front и стабильные итераторы для unordered_map.
    struct ChunkEntry {
        awdm::ChunkCoord coord{};
        ChunkState state{ChunkState::Unloaded};
        // Данные загруженного чанка (только для Loaded).
        std::unique_ptr<LoadedChunk> loaded;
        // Future генерации (Generating). По завершении перемещаем результат
        // в task queue и сбрасываем.
        std::future<std::unique_ptr<ChunkData>> generationFuture;
        // Ждёт освобождения слота в пуле (Requested) или снят с генерации.
        bool queuedForPool{false};
        // Метка «в текущем кольце видимости» (ставится каждый update).
        bool inViewThisFrame{false};
        // Для приоритетной постановки в пул: расстояние до игрока (метры).
        double priorityDistance{0.0};
    };

    using ChunkList = std::list<ChunkEntry>;
    using ChunkIt = ChunkList::iterator;

    // Элемент Task Queue: результат фоновой генерации, ожидающий загрузки в
    // главном потоке. Очередь защищена mutex_ + condition_variable_
    // (worker пишет, main ждёт/забирает).
    struct ReadyTask {
        std::unique_ptr<ChunkData> data;
        ChunkIt entry;  // позиция в LRU (стабильна для std::list)
    };

    // --- Внутренние шаги update() ---
    void shiftOriginForStreaming(const awdm::dvec3& playerGlobal);
    void applyOriginShift(const awdm::dvec3& delta);
    void drainCompleted();                       // future -> task queue
    void processReadyQueue(std::size_t budget);  // task queue -> GPU/Jolt/ECS
    void markDesiredAndEnqueue(const awdm::dvec3& playerGlobal,
                               double viewDistance);
    void dispatchPendingTasks();  // Requested -> Generating (по слотам пула)
    void cancelStaleRequests(std::size_t budget);
    void evictOutOfSight(const awdm::dvec3& playerGlobal, double viewDistance);
    void enforceLruCapacity();

    // --- Загрузка/выгрузка одного чанка (главный поток) ---
    // Создаёт Vulkan mesh, тело Jolt и entity ECS из готовых CPU-данных.
    void loadChunkToGpu(ChunkIt it, std::unique_ptr<ChunkData> data);
    // Полная очистка ресурсов чанка: ECS -> Jolt -> Vulkan -> CPU-данные.
    void destroyChunkResources(LoadedChunk& chunk);

    // --- Фоновая генерация (воркер пула) ---
    // Только CPU: шум -> heightmap -> биомы -> вершины. Возвращает владение
    // результатом; исключения пробрасываются через future в главный поток.
    std::unique_ptr<ChunkData> generateChunkData(awdm::ChunkCoord coord) const;

    awdm::ChunkCoord worldToChunk(const awdm::dvec3& global) const noexcept {
        return awdm::worldToChunk(global, config_.chunkSize);
    }

    // Поиск/вставка записи LRU. findExisting — без вставки.
    ChunkIt findChunk(const awdm::ChunkCoord& coord);
    ChunkIt getOrInsert(const awdm::ChunkCoord& coord);
    void touch(ChunkIt it);  // move-to-front LRU
    // Удаляет запись целиком (после выгрузки чанков вне rings+буфер).
    // Возвращает итератор на следующий элемент (для безопасного erase в цикле).
    ChunkIt dropEntry(ChunkIt it);

    Config config_;
    TerrainGenerator generator_;

    renderer::VulkanBase& renderer_;
    physics::PhysicsWorld& physics_;
    entt::registry& registry_;
    Climate* climate_;  // не владеет; может быть nullptr

    // Пул рабочих потоков + его габариты для учёта свободных слотов.
    std::unique_ptr<ThreadPool> pool_;
    unsigned poolSlots_{0};
    unsigned inflight_{0};  // Generating-чанков прямо сейчас (только main)

    // LRU: front — недавно использованный, back — кандидат на выгрузку.
    ChunkList chunksLru_;
    std::unordered_map<awdm::ChunkCoord, ChunkIt, ChunkCoordHash> index_;

    // Task Queue между воркерами и главным потоком.
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<ReadyTask> readyQueue_;

    // Spatial hash загруженных чанков (ячейка = чанк) — для быстрых запросов
    // высоты/биома и отладки покрытия.
    SpatialHash spatialHash_;

    // Floating origin (глобальная координата центра рендера, double).
    awdm::dvec3 origin_{0.0, 0.0, 0.0};

    // Атомарная статистика (пишет main/worker, читает HUD).
    std::atomic<std::uint64_t> loadsTotal_{0};
    std::atomic<std::uint64_t> unloadsTotal_{0};
    std::atomic<std::uint64_t> generationsTotal_{0};
    std::atomic<std::uint64_t> failedGenerations_{0};
    std::atomic<std::uint64_t> lruEvictions_{0};
    std::atomic<std::uint64_t> residentVertices_{0};
    std::atomic<std::uint64_t> generationCountEwmaN_{0};
    std::atomic<std::uint64_t> generationAvgBits_{0};  // double as bits
    std::atomic<std::uint64_t> generationMaxBits_{0};  // double as bits

    // Для debugHud: частота вызовов update (≈ FPS стриминга).
    std::chrono::steady_clock::time_point lastUpdateTime_{};
    double updateHz_{0.0};
    std::uint64_t frameCounter_{0};
};

}  // namespace world
