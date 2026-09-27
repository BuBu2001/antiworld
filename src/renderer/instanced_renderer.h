#pragma once

// instanced_renderer — отрисовка тысяч одинаковых объектов за один draw call.
//
// Контекст: деревья, камни и трава — это одна и та же геометрия, повёрнутая/
// масштабированная в сотнях тысяч точек ландшафта. Рисовать их по одному
// объекту = по одному draw call'у: на 10 000 деревьев CPU утонет в валидации
// команд и записи буферов задолго до того, как GPU что-нибудь нарисует.
//
// Решение (то, что уже умеет конвейер проекта): матрица модели каждого
// экземпляра приходит вершинным атрибутом с VK_VERTEX_INPUT_RATE_INSTANCE
// (binding 1 в GraphicsPipeline), а VulkanBase::drawFrame() группирует все
// DrawData одного mesh в ОДИН вызов vkCmdDrawIndexed с instanceCount=N —
// то есть фактически vkCmdDrawIndexedInstanced(indexCount, N) без изменения
// пайплайна. Матрицы инстансов лежат подряд в общем vertex-style instance
// буфере (storage данных «per-instance»), поэтому добавление 10 000 деревьев
// стоит CPU ровно одну запись memcpy-подобного цикла в маппенный буфер.
//
// Роль InstancedRenderer:
//   * хранит набор геометрий-«примитивов» (ствол+крона дерева, камень),
//     загруженных на GPU один раз;
//   * генерирует тестовые массивы объектов (10 000 деревьев) над ландшафтом;
//   * каждый кадр выполняет дешёвый CPU culling own AABB'ей против frustum
//     камеры и складывает выжившие инстансы в выходной span DrawData —
//     дальше они сливаются в единичные draw calls внутри drawFrame().
//
// Пул объектов плоский (vector<InstancedObject>), «удаление» — swap-remove,
// никаких аллокаций в горячем пути кадра. Вся GPU-память принадлежит
// VulkanBase (он владеет Mesh), деструктор InstancedRenderer ничего не
// утекает: объекты — только POD-данные в vector.

#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "core/double_math.h"
#include "renderer/frustum_culling.h"
#include "renderer/mesh.h"
#include "renderer/model_loader.h"

namespace renderer {

class VulkanBase;

// Один экземпляр инстансного объекта: позиция (float, локальные координаты
// вокруг floating origin), поворот вокруг Y, равномерный масштаб и ссылка на
// примитив. AABB вычисляется на лету — примитивы центрированы, радиус =
// scale * radius (консервативная сфера -> AABB).
struct InstancedObject {
    glm::vec3 position{0.0f};
    float yawRadians{0.0f};
    float scale{1.0f};
    std::uint16_t primitive{0};
    float radius{1.0f};   // bounding radius юнита (до масштаба)
    glm::vec3 colorTint{1.0f};  // альбедо-множитель (в будущем — через атрибут)
};

class InstancedRenderer {
public:
    InstancedRenderer() = default;

    // Копирование запрещено: указатели mesh принадлежат VulkanBase, а пул
    // объектов большой — случайная копия стоила бы десятков мегабайт.
    InstancedRenderer(const InstancedRenderer&) = delete;
    InstancedRenderer& operator=(const InstancedRenderer&) = delete;

    // Регистрирует геометрию-примитив (загружается на GPU ОДИН раз, далее
    // переиспользуется всеми инстансами). Возвращает id примитива для
    // InstancedObject::primitive. Пустая геометрия — исключение.
    std::uint16_t addPrimitive(VulkanBase& renderer, const ModelData& geometry);
    // Освобождает примитивы (mesh'и удаляет renderer). Идемпотентно; вызывать
    // ДО уничтожения VulkanBase (как и весь rest of world-ресурсов).
    void destroy(VulkanBase& renderer);

    // === Тестовый полигон: `count` «деревьев» (стволок + крона-кубик) ===
    // Расставляет деревья по сетке с джиттером над ландшафтом: высота берётся
    // из heightSampler (глобальные XZ -> Y), ниже seaLevel+minAboveSea —
    // пропуск (на воде деревьев не бывает). Детерминированный seed.
    using HeightSampler = float (*)(double globalX, double globalZ, void* userData);
    std::size_t spawnTrees(VulkanBase& renderer, std::size_t count,
                           const awdm::dvec3& areaCenterGlobal, double areaSizeMeters,
                           HeightSampler heightSampler, void* samplerUserData,
                           double minAboveSea, std::uint32_t seed);

    std::size_t objectCount() const noexcept { return objects_.size(); }
    const std::vector<InstancedObject>& objects() const noexcept { return objects_; }
    std::size_t primitiveCount() const noexcept { return primitives_.size(); }

    // Отрисовочный проход кадра: frustum-culling по AABB каждого экземпляра
    // (double-плоскости, тот же Frustum, что и для чанков) + сборка DrawData.
    // Выход дописывается в out (вызывающий передаёт свой буфер-накопитель,
    // аллокаций здесь нет — capacity переиспользуется между кадрами).
    // Возвращает число принятых инстансов (для HUD-статистики).
    struct CullStats {
        std::uint32_t total{0};       // объектов в пуле
        std::uint32_t visible{0};     // прошло culling
        std::uint32_t drawCalls{0};   // групп (mesh, lod), т.е. будущих вызовов
    };
    CullStats collectDraws(const Frustum& frustum, const awdm::dvec3& origin,
                           std::vector<DrawData>& out) const;

    // Модельная матрица экземпляра (перемещение * поворот(Y) * масштаб).
    static glm::mat4 instanceMatrix(const InstancedObject& object,
                                    const awdm::dvec3& origin) noexcept;

private:
    struct Primitive {
        Mesh* mesh{nullptr};  // не владеет: mesh принадлежит VulkanBase
        std::uint32_t triangles{0};
    };

    void ensureSorted() const;

    std::vector<Primitive> primitives_;
    std::vector<InstancedObject> objects_;
    // Буфер-накопитель выхода collectDraws (mutable-семантики избегаем:
    // collectDraws пишет в out вызывающего; этот scratch нужен только для
    // сортировки по примитиву, чтобы drawFrame() слил инстансы в один вызов).
    mutable std::vector<std::pair<std::uint16_t, std::uint32_t>> sortScratch_;
    // Сортировка нужна только после изменения пула объектов; после — переиспользуем.
    mutable bool sortDirty_{true};
};

}  // namespace renderer
