#pragma once

// lod_manager — геометрический Level Of Detail для чанков ландшафта.
//
// Контекст: мир огромный, чанки асинхронно грузятся/выгружаются, и рисовать
// каждый чанк полной сеткой 65x65 на дистанции в километры бессмысленно —
// пиксель крупнее треугольника. Поэтому при генерации mesh чанка создаются
// три уровня детализации:
//
//   * LOD0 — полный mesh (все вершины heightmap-сетки);
//   * LOD1 — ~50% вершин (каждая 2-я строка/столбец сетки);
//   * LOD2 — ~25% вершин (каждый 4-й узел сетки).
//
// Почему именно «прореживание сетки», а не QEM/decimation: heightmap —
// регулярная сетка, и прореживание по чётным индексам строк/столбцов даёт
// ровно нужную долю вершин за O(N) без аллокаций на точку, динамически
// устойчиво к любой высоте и тривиально корректно по индексам (см. .cpp).
//
// Выбор уровня — по расстоянию от камеры до AABB чанка (не центра!): >500 м
// -> LOD1, >1000 м -> LOD2 (пороги настраиваются в ChunkManager::Config).
// Смена LOD не требует никаких GPU-операций: все три уровня загружены на GPU
// один раз при загрузке чанка, переключение — это просто другая пара буферов
// в другом draw-вызове (см. renderer::LodMesh / MeshDraw::lod).

#include <cstdint>
#include <vector>

#include "renderer/model_loader.h"  // ModelData (Vertex + индексы)

namespace renderer {

class VulkanBase;
class Mesh;

// Число уровней детализации: LOD0 (полный), LOD1 (50%), LOD2 (25%).
inline constexpr int kLodLevels = 3;

// Один уровень: CPU-геометрия (до загрузки) + GPU-буферы (после загрузки).
struct LodLevel {
    // Вершины/индексы уровня. После upload() освобождаются swap'ом с пустым
    // вектором — держать их в RAM смысла нет, копия уже на GPU.
    ModelData model;
    // Не владеет: адрес стабильна (VulkanBase хранит unique_ptr, vector не
    // переезжает). Пустой указатель означает «уровень не загружен».
    Mesh* mesh{nullptr};
    std::uint32_t vertexCount{0};
    std::uint32_t indexCount{0};
};

// Все LOD одного чанка как единое целое: создаётся в воркере генерации
// (только CPU), загружается на GPU в главном потоке, уничтожается вместе
// с чанком. Копирование запрещено (внутри — невладеющие GPU-указатели),
// перемещение разрешено (RAII-очистка CPU-данных при move обеспечена
// конструктором перемещения).
struct LodMesh {
    LodLevel levels[kLodLevels]{};

    LodMesh() = default;
    LodMesh(const LodMesh&) = delete;
    LodMesh& operator=(const LodMesh&) = delete;
    LodMesh(LodMesh&& other) noexcept;
    LodMesh& operator=(LodMesh&& other) noexcept;

    // Общее число вершин по всем уровням (для статистики residentVertices).
    std::uint64_t totalVertices() const noexcept;
    bool empty() const noexcept { return levels[0].indexCount == 0; }
};

class LodManager {
public:
    LodManager() = default;

    // Из полного mesh (LOD0) строит уровни LOD1 и LOD2 прореживанием
    // heightmap-сетки. Функция чисто вычислительная (без Vulkan) — её можно
    // вызывать из фоновых потоков генерации чанков.
    //
    // Ожидается регулярная сетка: vertices.size() == res*res, индексы —
    // квады (tl, bl, tr, tr, bl, br) по строкам. Если геометрия не сетка
    // или разрешение слишком мало для прореживания, соответствующий уровень
    // остаётся копией предыдущего (деградация безопасна, mesh не ломается).
    static void buildLodLevels(const ModelData& base, std::uint32_t res,
                               LodMesh& out);

    // Загрузка всех уровней на GPU (ГЛАВНЫЙ ПОТОК: Vulkan не потокобезопасен).
    // mesh'и принадлежат renderer; LodMesh хранит только указатели.
    static void upload(VulkanBase& renderer, LodMesh& lod);

    // Уничтожение GPU-буферов всех уровней через renderer.destroyMesh().
    // Идемпотентно: незагруженные уровни пропускаются.
    static void destroy(VulkanBase& renderer, LodMesh& lod);
};

}  // namespace renderer
