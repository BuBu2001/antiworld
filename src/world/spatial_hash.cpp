// Реализация SpatialHash — см. описание в spatial_hash.h.

#include "world/spatial_hash.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace world {

SpatialHash::SpatialHash(double cellSize)
    : cellSize_(cellSize > 0.0 && std::isfinite(cellSize) ? cellSize : 1024.0) {}

std::int64_t SpatialHash::toCell(double worldCoord) const noexcept {
    // Floor-деление: ячейка [-c,0) имеет индекс -1, а не 0 (округление к нулю
    // склеило бы отрицательные и положительные координаты в одну ячейку).
    return static_cast<std::int64_t>(std::floor(worldCoord / cellSize_));
}

CellKey SpatialHash::toCell(double worldX, double worldZ) const noexcept {
    return {toCell(worldX), toCell(worldZ)};
}

bool SpatialHash::insert(ChunkId chunk_id, double x, double z) {
    if (!std::isfinite(x) || !std::isfinite(z)) return false;

    std::unique_lock lock(mutex_);

    auto& entry = chunks_[chunk_id];
    const CellKey key = toCell(x, z);

    // Идемпотентность: если чанк уже занимает ровно эту ячейку — ничего нет.
    if (!entry.cells.empty() &&
        std::find(entry.cells.begin(), entry.cells.end(), key) != entry.cells.end()) {
        return false;
    }

    buckets_[key].insert(chunk_id);
    entry.cells.push_back(key);
    return true;
}

bool SpatialHash::insertRect(ChunkId chunk_id, double minX, double minZ,
                             double maxX, double maxZ) {
    if (!std::isfinite(minX) || !std::isfinite(minZ) ||
        !std::isfinite(maxX) || !std::isfinite(maxZ)) {
        return false;
    }
    if (maxX < minX) std::swap(minX, maxX);
    if (maxZ < minZ) std::swap(minZ, maxZ);

    std::unique_lock lock(mutex_);

    // Предупреждение о вырожденном случае: покрытие тысяч ячеек одним чанком
    // означает, что cellSize выбран много меньше габарита чанка.
    const std::int64_t cx0 = toCell(minX), cx1 = toCell(maxX);
    const std::int64_t cz0 = toCell(minZ), cz1 = toCell(maxZ);

    auto& entry = chunks_[chunk_id];
    bool addedAny = false;
    for (std::int64_t cz = cz0; cz <= cz1; ++cz) {
        for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
            const CellKey key{cx, cz};
            if (std::find(entry.cells.begin(), entry.cells.end(), key) ==
                entry.cells.end()) {
                buckets_[key].insert(chunk_id);
                entry.cells.push_back(key);
                addedAny = true;
            }
        }
    }
    if (!addedAny) chunks_.erase(chunk_id);  // не оставляем пустой запись
    return addedAny;
}

bool SpatialHash::remove(ChunkId chunk_id) {
    std::unique_lock lock(mutex_);

    auto it = chunks_.find(chunk_id);
    if (it == chunks_.end()) return false;

    for (const CellKey& key : it->second.cells) {
        auto bucketIt = buckets_.find(key);
        if (bucketIt == buckets_.end()) continue;
        bucketIt->second.erase(chunk_id);
        if (bucketIt->second.empty()) buckets_.erase(bucketIt);  // GC ячеек
    }
    chunks_.erase(it);
    return true;
}

bool SpatialHash::move(ChunkId chunk_id, double x, double z) {
    if (!std::isfinite(x) || !std::isfinite(z)) return false;

    // Одна эксклюзивная сессия на remove+insert: читатели (shared lock) либо
    // видят старый вариант, либо новый — промежуточного «нет чанка» нет.
    std::unique_lock lock(mutex_);

    const CellKey newKey = toCell(x, z);

    if (auto it = chunks_.find(chunk_id); it != chunks_.end()) {
        Entry& entry = it->second;
        // Быстрый путь: чанк уже в нужной ячейке и только в ней.
        if (entry.cells.size() == 1 && entry.cells[0] == newKey) return false;

        for (const CellKey& key : entry.cells) {
            auto bucketIt = buckets_.find(key);
            if (bucketIt == buckets_.end()) continue;
            bucketIt->second.erase(chunk_id);
            if (bucketIt->second.empty()) buckets_.erase(bucketIt);
        }
        entry.cells.clear();
        entry.cells.push_back(newKey);
        buckets_[newKey].insert(chunk_id);
        return true;
    }

    buckets_[newKey].insert(chunk_id);
    chunks_[chunk_id] = Entry{{newKey}};
    return true;
}

void SpatialHash::clear() {
    std::unique_lock lock(mutex_);
    buckets_.clear();
    chunks_.clear();
}

std::vector<SpatialHash::ChunkId> SpatialHash::collect_in_radius_unsorted(
    double x, double z, double radius) const {
    std::vector<ChunkId> result;
    if (!std::isfinite(x) || !std::isfinite(z) || radius < 0.0 ||
        !std::isfinite(radius)) {
        return result;
    }

    std::shared_lock lock(mutex_);

    // Обходим прямоугольник ячеек, покрывающий круг, и отбрасываем чанки из
    // ячеек, чей квадрат за пределами круга (по центру ячейки — консервативно
    // и достаточно для стриминга: лишние чанки на границе не помеха).
    const std::int64_t cx0 = toCell(x - radius);
    const std::int64_t cx1 = toCell(x + radius);
    const std::int64_t cz0 = toCell(z - radius);
    const std::int64_t cz1 = toCell(z + radius);

    std::unordered_set<ChunkId> seen;  // дубли: чанк мог покрыть несколько ячеек

    for (std::int64_t cz = cz0; cz <= cz1; ++cz) {
        for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
            // Центр ячейки в мировых координатах (double).
            const double cellCenterX = (static_cast<double>(cx) + 0.5) * cellSize_;
            const double cellCenterZ = (static_cast<double>(cz) + 0.5) * cellSize_;
            const double dx = cellCenterX - x;
            const double dz = cellCenterZ - z;
            // Скруглённый тест: расстояние от точки до БЛИЖАЙШЕЙ точки квадрата
            // ячейки (не до центра) — иначе радиус «съедался» бы на углах.
            const double half = cellSize_ * 0.5;
            const double ox = std::max(std::abs(dx) - half, 0.0);
            const double oz = std::max(std::abs(dz) - half, 0.0);
            if (ox * ox + oz * oz > radius * radius) continue;

            auto it = buckets_.find(CellKey{cx, cz});
            if (it == buckets_.end()) continue;
            for (ChunkId id : it->second) {
                if (seen.insert(id).second) result.push_back(id);
            }
        }
    }
    return result;
}

std::vector<SpatialHash::ChunkId> SpatialHash::get_chunks_in_radius(
    double x, double z, double radius) const {
    std::vector<ChunkId> result = collect_in_radius_unsorted(x, z, radius);

    // Сортировка по расстоянию от центра запроса до центра ячейки чанка.
    // Держим карту id -> d2, чтобы не пересчитывать geometry при компараторе.
    std::shared_lock lock(mutex_);
    std::unordered_map<ChunkId, double> dist2;
    dist2.reserve(result.size());
    for (ChunkId id : result) {
        const auto it = chunks_.find(id);
        if (it == chunks_.end() || it->second.cells.empty()) {
            dist2[id] = std::numeric_limits<double>::infinity();
            continue;
        }
        double best = std::numeric_limits<double>::infinity();
        for (const CellKey& k : it->second.cells) {
            const double cx = (static_cast<double>(k.x) + 0.5) * cellSize_ - x;
            const double cz = (static_cast<double>(k.z) + 0.5) * cellSize_ - z;
            best = std::min(best, cx * cx + cz * cz);
        }
        dist2[id] = best;
    }
    std::sort(result.begin(), result.end(),
              [&dist2](ChunkId a, ChunkId b) { return dist2[a] < dist2[b]; });
    return result;
}

bool SpatialHash::contains(ChunkId chunk_id) const {
    std::shared_lock lock(mutex_);
    return chunks_.find(chunk_id) != chunks_.end();
}

std::size_t SpatialHash::chunkCount() const noexcept {
    std::shared_lock lock(mutex_);
    return chunks_.size();
}

std::size_t SpatialHash::cellCount() const noexcept {
    std::shared_lock lock(mutex_);
    return buckets_.size();
}

}  // namespace world
