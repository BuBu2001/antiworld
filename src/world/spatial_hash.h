#pragma once

// Spatial Hash Grid — хэш-сетка для быстрого поиска чанков по координатам.
//
// Мир разбит на регулярную сетку ячеек размером cellSize (по умолчанию —
// размер чанка, например 1024 м). Каждая ячейка хранит список id чанков,
// которые её покрывают. Поиск «что вокруг точки за радиусом R» сводится к
// обходу ~(2R/cellSize)^2 ячеек вместо перебора всех чанков мира — это
// критично при стриминге десятков тысяч чанков одновременно.
//
// Работа с 64-битными координатами: индексы ячеек — int64 (при cellSize
// 1 км и радиусе мира 12,7 млн м координата ячейки ~1,3e7 — но хэш обязан
// корректно работать и с отрицательными, и с будущими гигантскими значениями,
// поэтому никаких int32 в ключах). Хэш-функция — классическая комбинация
// больших простых чисел Фибоначчи/Голдмана с перемешиванием бит (splitmix64),
// устойчивая к кластерам соседних ячеек:
//     h(x, z) = splitmix64(x * K1 ^ z * K2 * 0x9E3779B97F4A7C15)
//
// Потокобезопасность: всё состояние спрятано под std::shared_mutex.
// Readers (get_chunks_in_radius, contains, size) берут shared lock и работают
// параллельно; writers (insert/remove/clear) — эксклюзивный. Это позволяет
// потокам генерации чанков вставлять результаты, пока поток рендера выбирает
// видимые чанки. Методы не рекурсивны — не вызывать их из-под другого замка
// этой же сетки.
//
// chunk_id — произвольный идентификатор (entt::entity как uint, индекс пула
// чанков, глобальный id). Один чанк может занимать несколько ячеек (если его
// AABB больше ячейки) — insert принимает прямоугольник покрытия либо одну
// ячейку по координатам чанка.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace world {

// Ключ ячейки сетки — 64-битные целые (НИКАКИХ float/int32).
struct CellKey {
    std::int64_t x = 0;
    std::int64_t z = 0;

    constexpr bool operator==(const CellKey&) const = default;
};

struct CellKeyHash {
    // Смешиваем обе координаты splitmix64-подобным перемешиванием. Константы —
    // иррациональные пропорции золотого сечения в 64-битном целом: соседние
    // ключи (частый случай!) дают равномерно разбросанные хэши.
    static constexpr std::uint64_t kMulX = 0x9E3779B97F4A7C15ull;
    static constexpr std::uint64_t kMulZ = 0xC2B2AE3D27D4EB4Full;

    std::size_t operator()(const CellKey& k) const noexcept {
        std::uint64_t h = static_cast<std::uint64_t>(k.x) * kMulX ^
                          static_cast<std::uint64_t>(k.z) * kMulZ;
        // Финальный avalanche (splitmix64 mix): разбрасываем младшие биты,
        // чтобы таблица C++ не собирала коллизии по mod bucket_count.
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

class SpatialHash {
public:
    using ChunkId = std::uint64_t;

    // cellSize — ребро ячейки в метрах (double!). Обычно равен размеру чанка.
    explicit SpatialHash(double cellSize = 1024.0);

    SpatialHash(const SpatialHash&) = delete;
    SpatialHash& operator=(const SpatialHash&) = delete;

    double cellSize() const noexcept { return cellSize_; }

    // Мировая координата (double) -> индекс ячейки (int64, floor-деление).
    std::int64_t toCell(double worldCoord) const noexcept;
    CellKey toCell(double worldX, double worldZ) const noexcept;

    // === Вставка/удаление (writers, exclusive lock) ===

    // Вставляет чанк в ячейку, содержащую точку (x, z) — базовый API по ТЗ.
    // true — если чанк добавлен, false — если уже был там же (идемпотентно).
    bool insert(ChunkId chunk_id, double x, double z);

    // Вставляет чанк во ВСЕ ячейки, покрытые прямоугольником
    // [minX,maxX] x [minZ,maxZ] (глобальные метры). Нужно, когда габарит
    // чанка больше ячейки или когда чанок перекрывает границы соседей.
    bool insertRect(ChunkId chunk_id, double minX, double minZ,
                    double maxX, double maxZ);

    // Полностью удаляет чанк изо всех ячеек. true — если чанк найден.
    bool remove(ChunkId chunk_id);

    // Перемещение чанка: remove + insert в новую ячейку (атомарно под одним
    // замком — читатели никогда не увидят «исчезнувший посередине кадра» чанк).
    bool move(ChunkId chunk_id, double x, double z);

    // Очищает всю сетку (выгрузка мира / рестарт).
    void clear();

    // === Запросы (readers, shared lock) ===

    // Возвращает id чанков, чьи ячейки пересекаются кругом радиуса radius
    // (метры) вокруг точки (x, z). Результат отсортирован по расстоянию до
    // центра запроса (ближайшие первыми) — удобно для order-independent
    // стриминга и frustum-culling приоритетов.
    std::vector<ChunkId> get_chunks_in_radius(double x, double z, double radius) const;

    // То же, но без сортировки (дешевле, если порядок не важен).
    std::vector<ChunkId> collect_in_radius_unsorted(double x, double z, double radius) const;

    // Есть ли чанк в сетке вообще.
    bool contains(ChunkId chunk_id) const;

    // Сколько чанков хранится (уникальных id).
    std::size_t chunkCount() const noexcept;
    // Сколько непустых ячеек.
    std::size_t cellCount() const noexcept;

private:
    // Ячейка — набор id (быстрый поиск дублей при insert/remove).
    using Bucket = std::unordered_set<ChunkId>;

    // Реверс-индекс chunk -> покрытые ячейки, чтобы remove() был O(покрытие),
    // а не O(вся сетка).
    struct Entry {
        std::vector<CellKey> cells;
    };

    // Инвариант: buckets[k] содержит c тогда и только тогда, когда
    // chunks[c].cells содержит k. Все публичные методы поддерживают его
    // под эксклюзивным замком.
    double cellSize_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<CellKey, Bucket, CellKeyHash> buckets_;
    std::unordered_map<ChunkId, Entry> chunks_;
};

}  // namespace world
