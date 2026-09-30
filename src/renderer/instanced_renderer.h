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

// Один экземпляр инстансного объекта.
//
// Позиция хранится в ГЛОБАЛЬНЫХ координатах (double): это истина о мире, и
// только в collectDraws()/instanceMatrix() она переводится в локальные
// float-координаты относительно текущего floating origin. Хранение сразу
// локальных float означало бы, что при сдвиге origin все объекты «остаются
// позади» и требуют ручного пересчёта — источник тихих артефактов на
// больших дистанциях (float32 на 22 585 км даёт шаг ~2 м).
struct InstancedObject {
    glm::dvec3 globalPosition{0.0};
    float yawRadians{0.0f};
    float scale{1.0f};
    std::uint16_t primitive{0};
    // Габаритный радиус юнита. НЕ заданная константа вида, а извлечённый из
    // реальной геометрии в addPrimitive(): у каждого вида своя высота и
    // размах, и сфера, посчитанная «на глаз», либо обрезает крону (дерево
    // исчезает из кадара у самой камеры), либо стоит втрое дороже нужного.
    float radius{1.0f};
    // Альбедо-множитель экземпляра. У растительности он несут СМЫСЛ: деревья
    // одного вида на севере и на юге отличаются цветом (хвоя темнее, листва
    // желтее в засухе), и без множителя весь мир был бы одного оттенка.
    glm::vec3 colorTint{1.0f};
};

class InstancedRenderer {
public:
    // Примитив, которого нет. Рассев проверяет индекс на это значение перед
    // использованием: молчаливый выход за пределы массива дал бы «растение» из
    // чужого mesh'а, и ошибка проявилась бы через тысячу кадров как «иногда
    // не то дерево», а не как падение.
    static constexpr std::uint16_t kInvalidPrimitive = 0xFFFFu;

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
    //
    // ОСТОРОЖНО: это тестовый рассев, он НЕ смотрит на климат и ставит один
    // вид на всей площади. Боевая растительность — world::VegetationScatter,
    // он выбирает вид по температуре/осадкам и умеет пересобираться при
    // движении игрока. Этот остаётся только как простой пример вызова.
    using HeightSampler = float (*)(double globalX, double globalZ, void* userData);
    std::size_t spawnTrees(VulkanBase& renderer, std::size_t count,
                           const awdm::dvec3& areaCenterGlobal, double areaSizeMeters,
                           HeightSampler heightSampler, void* samplerUserData,
                           double minAboveSea, std::uint32_t seed);

    // === Пул объектов (используется VegetationScatter) ===

    // Добавить готовый объект в пул. Пул — плоский vector, поэтому добавление
    // амортизировано, а вставлять можно только в конец.
    void addObject(const InstancedObject& object) { addObjectNoReserve(object); }

    // Полностью очистить пул объектов, ОСТАВИВ примитивы (mesh'и): смена
    // рассева вокруг игрока не должна перезагружать геометрию на GPU, иначе
    // каждый шаг по миру стоил бы десятки миллисекунд на загрузку mesh'ей.
    //
    // «Удаление» объекта внутри пула — swap-remove (см. removeSwap),
    // произвольный порядок после которого не имеет значения: объекты
    // сортируются по примитиву заново в collectDraws().
    void clearObjects();

    // Габаритный радиус примитива в юнитах. Нужен рассеву, чтобы проставить
    // object.radius при создании инстанса.
    float primitiveRadius(std::uint16_t primitive) const noexcept;

    std::size_t objectCount() const noexcept { return objects_.size(); }
    const std::vector<InstancedObject>& objects() const noexcept { return objects_; }
    std::size_t primitiveCount() const noexcept { return primitives_.size(); }

    // Отрисовочный проход кадра: frustum-culling по AABB каждого экземпляра
    // (double-плоскости, те же ЛОКАЛЬНЫЕ координаты, что и у frustum) + сборка
    // DrawData.
    //
    // ВАЖНО: out ОЧИЩАЕТСЯ ВЫЗЫВАЮЩИМ, collectDraws() дописывает в конец.
    // (Счётчик drawCalls внутри считает только группы, добавленные этим
    // вызовом, отсюда и append-семантика.) Забытый clear() — не мелкая
    // неаккуратность: буфер растёт на ~N элементов КАЖДЫЙ кадр, culling
    // деградирует кадр за кадром, а в drawFrame уходят задублированные
    // инстансы прошлых кадров.
    //
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
        // Габариты ЮНИТА, извлечённые из геометрии при загрузке. Основание
        // вида — Y=0 (то есть в точке земли), а тело растёт вверх, поэтому
        // bounding center по Y НЕ равен нулю: у 12-метровой ели он около
        // 5.5 м. Раньше AABB строился сферой вокруг самой позиции, то есть
        // вокруг земли, и верхушка кроны выпадала из кадра у самой камеры.
        glm::vec3 boundCenter{0.0f};
        float boundRadius{1.0f};
    };

    void ensureSorted() const;
    // Добавление без reserve: рассев зовёт это тысячи раз подряд, и reserve
    // на каждый вызов стоил бы дороже самой вставки.
    void addObjectNoReserve(const InstancedObject& object);

    std::vector<Primitive> primitives_;
    std::vector<InstancedObject> objects_;
    // Сколько объектов пула зарезервировано: addObject() при пустом пуле
    // резервирует порциями, иначе первый же рассев на 100 000 объектов
    // делал бы 17 перевыделений с копированием.
    std::size_t objectCapacityHint_{0};
    // Буфер-накопитель выхода collectDraws (mutable-семантики избегаем:
    // collectDraws пишет в out вызывающего; этот scratch нужен только для
    // сортировки по примитиву, чтобы drawFrame() слил инстансы в один вызов).
    mutable std::vector<std::pair<std::uint16_t, std::uint32_t>> sortScratch_;
    // Сортировка нужна только после изменения пула объектов; после — переиспользуем.
    mutable bool sortDirty_{true};
};

}  // namespace renderer
