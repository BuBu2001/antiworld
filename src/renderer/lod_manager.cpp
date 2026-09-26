#include "renderer/lod_manager.h"

#include <stdexcept>
#include <utility>

#include "core/logger.h"
#include "renderer/mesh.h"
#include "renderer/vulkan_base.h"

namespace renderer {

// ============================ LodMesh ==================================

LodMesh::LodMesh(LodMesh&& other) noexcept {
    for (int level = 0; level < kLodLevels; ++level) {
        levels[level] = std::move(other.levels[level]);
    }
}

LodMesh& LodMesh::operator=(LodMesh&& other) noexcept {
    if (this != &other) {
        for (int level = 0; level < kLodLevels; ++level) {
            levels[level] = std::move(other.levels[level]);
        }
    }
    return *this;
}

std::uint64_t LodMesh::totalVertices() const noexcept {
    std::uint64_t total = 0;
    for (const LodLevel& level : levels) {
        total += level.vertexCount;
    }
    return total;
}

// ========================== построение LOD ==============================

namespace {

// Прореживание heightmap-сетки шагом `step` по обеим осям.
//
// Входная геометрия — регулярная сетка res x res узлов, индексы идут
// квадами по строкам: для ячейки (x, z) шесть индексов
// {tl, bl, tr, tr, bl, br}, где tl = z*res + x. Такой порядок генерирует и
// world::ChunkManager, и world::TerrainGenerator — см. их циклы сборки mesh.
//
// Алгоритм («удаление нечётных вершин» в обобщённом виде):
//   1) выжившие узлы — те, у обоих индексов остаток по модулю step равен 0;
//      шаг 2 даёт ~50% вершин, шаг 4 — ~25%;
//   2) каждой выжившей вершине назначается новый компактный индекс;
//   3) каждый треугольник исходной сетки отображается в треугольник из
//      «ближайших снизу» выживших узлов: вершина (x, z) -> (x - x%step,
//      z - z%step). Это всегда корректно (образ — выродившийся или настоящий
//      треугольник новой сетки) и сохраняет поверхность с точностью до
//      шага прореживания. Выродившиеся треугольники (все три вершины
//      отобразились в одну точку / в одну линию) отбрасываются — на них
//      нулевая площадь, GPU всё равно не рисует ни пикселя, а так экономится
//      место в индексном буфере.
//
// Сложность O(V + I), одна аллокация под карту реиндексации и два вектора.
ModelData decimateGrid(const ModelData& base, std::uint32_t res, std::uint32_t step) {
    ModelData out;
    if (res < 2 || base.vertices.size() != static_cast<std::size_t>(res) * res) {
        // Не сетка — прореживать нельзя; возвращаем копию (см. комментарий
        // в заголовке: безопасная деградация вместо битой геометрии).
        return base;
    }

    // --- 1/2) выжившие вершины и карта старых индексов -> новые ---
    std::vector<uint32_t> remap(static_cast<std::size_t>(res) * res, 0xFFFFFFFFu);
    out.vertices.reserve(base.vertices.size() / (step * step) + step);
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            if ((x % step) != 0 || (z % step) != 0) continue;
            const std::size_t oldIndex = static_cast<std::size_t>(z) * res + x;
            remap[oldIndex] = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(base.vertices[oldIndex]);
        }
    }
    // Крайние ряды обязательно входят в любую степень прореживания (0 кратно
    // любому step), поэтому пустых результатов здесь быть не может; проверка —
    // страховка от порчи входных данных.
    if (out.vertices.size() < 4) return base;

    // --- 3) индексы: каждый квад -> один квад уменьшенной сетки ---
    const std::size_t quads = static_cast<std::size_t>(res - 1) * (res - 1);
    if (base.indices.size() != quads * 6) {
        // Индексы не в ожидаемом порядке — не гадаем, оставляем полный mesh.
        return base;
    }
    out.indices.reserve(quads * 6 / (step * step));
    const auto snap = [res, step](uint32_t index) -> uint32_t {
        const uint32_t z = index / res;
        const uint32_t x = index % res;
        return (z - (z % step)) * res + (x - (x % step));
    };
    for (std::size_t q = 0; q < quads; ++q) {
        // Квад генерируется как два треугольника {tl, bl, tr} и {tr, bl, br}.
        const uint32_t a = snap(base.indices[q * 6 + 0]);  // tl
        const uint32_t b = snap(base.indices[q * 6 + 1]);  // bl
        const uint32_t c = snap(base.indices[q * 6 + 2]);  // tr
        // Второй треугольник квада — те же три узла + br; если первый
        // выродился (a==b||a==c||b==c), выродится и второй.
        if (a == b || a == c || b == c) continue;
        const uint32_t d = snap(base.indices[q * 6 + 5]);  // br
        if (c == b || c == d || b == d) {
            // Треугольник tr/bl/br выродился, но tl-треугольник нет —
            // добавляем только его.
            out.indices.insert(out.indices.end(), {remap[a], remap[b], remap[c]});
            continue;
        }
        out.indices.insert(out.indices.end(),
                           {remap[a], remap[b], remap[c], remap[c], remap[b], remap[d]});
    }
    return out;
}

}  // namespace

void LodManager::buildLodLevels(const ModelData& base, std::uint32_t res,
                                LodMesh& out) {
    // LOD0 — полный mesh (копия входа: LodMesh владеет своей геометрией).
    out.levels[0].model = base;
    // LOD1 — шаг 2 (~50% вершин), LOD2 — шаг 4 (~25% вершин).
    out.levels[1].model = decimateGrid(base, res, 2);
    out.levels[2].model = decimateGrid(base, res, 4);

    for (int level = 0; level < kLodLevels; ++level) {
        LodLevel& dst = out.levels[level];
        dst.mesh = nullptr;
        dst.vertexCount = static_cast<uint32_t>(dst.model.vertices.size());
        dst.indexCount = static_cast<uint32_t>(dst.model.indices.size());
    }
}

// ========================= загрузка / выгрузка ==========================

void LodManager::upload(VulkanBase& renderer, LodMesh& lod) {
    for (int level = 0; level < kLodLevels; ++level) {
        LodLevel& dst = lod.levels[level];
        if (dst.mesh != nullptr) continue;  // уже загружен — идемпотентно
        if (dst.model.vertices.empty() || dst.model.indices.empty()) continue;
        dst.mesh = renderer.createMesh(dst.model);
        // GPU-копия есть — CPU-геометрия больше не нужна. swap с пустым
        // вектором освобождает ёмкость (clear() оставил бы capacity в RAM).
        ModelData empty;
        dst.model.vertices.swap(empty.vertices);
        dst.model.indices.swap(empty.indices);
    }
}

void LodManager::destroy(VulkanBase& renderer, LodMesh& lod) {
    for (int level = 0; level < kLodLevels; ++level) {
        LodLevel& dst = lod.levels[level];
        if (dst.mesh == nullptr) continue;
        renderer.destroyMesh(dst.mesh);  // внутри vkDeviceWaitIdle
        dst.mesh = nullptr;
    }
}

}  // namespace renderer
