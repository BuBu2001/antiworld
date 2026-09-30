#include "renderer/instanced_renderer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

#include "core/logger.h"
#include "renderer/vegetation_geometry.h"
#include "renderer/vulkan_base.h"

namespace renderer {

namespace {

// Значение в диапазоне [-1, 1] для дешёвой «псевдо-высоты» тестовой сетки.
float hashNoise(std::uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return static_cast<float>(static_cast<std::int64_t>(h) % 2048) / 1024.0f - 1.0f;
}

// Процедурная «палка-дерево»: тонкий вертикальный куб (ствол) + куб-крона
// сверху. Одна mesh, один набор вершин — все 10 000 экземпляров рисуются ею.
// Координаты центрированы по XZ, основание на Y=0, поэтому AABB инстанса
// строится от позиции с известным радиусом/высотой.
ModelData makeStickTreeGeometry() {
    ModelData model;
    const auto addBox = [&model](glm::vec3 center, glm::vec3 half) {
        const std::size_t base = model.vertices.size();
        // 8 углов коробка.
        for (int corner = 0; corner < 8; ++corner) {
            Vertex v{};
            v.position[0] = center.x + ((corner & 1) ? half.x : -half.x);
            v.position[1] = center.y + ((corner & 2) ? half.y : -half.y);
            v.position[2] = center.z + ((corner & 4) ? half.z : -half.z);
            v.normal[0] = 0.0f;
            v.normal[1] = 1.0f;  // усреднённая «верхняя» нормаль — прототип
            v.normal[2] = 0.0f;
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
            v.color[0] = 0.30f;  // тёмно-зелёная хвоя/кора тестового дерева
            v.color[1] = 0.55f;
            v.color[2] = 0.25f;
            v.snowBias = 0.2f;
            v.water = 0.0f;
            model.vertices.push_back(v);
        }
        // 6 граней по два треугольника (обход CCW снаружи).
        static const std::uint32_t faceIndices[36] = {
            0, 2, 1, 1, 2, 3,  // -Z? (front/back)
            4, 5, 6, 6, 5, 7,
            0, 1, 4, 4, 1, 5,
            2, 6, 3, 3, 6, 7,
            0, 4, 2, 2, 4, 6,
            1, 3, 5, 5, 3, 7,
        };
        for (const std::uint32_t index : faceIndices) {
            model.indices.push_back(static_cast<std::uint32_t>(base + index));
        }
    };

    // Ствол: 0.15 м в половину, 2.4 м высотой, основание в нуле.
    addBox({0.0f, 1.2f, 0.0f}, {0.15f, 1.2f, 0.15f});
    // Крона: куб 1.6 м, центр на 3.2 м.
    addBox({0.0f, 3.2f, 0.0f}, {0.8f, 0.9f, 0.8f});
    return model;
}

}  // namespace

std::uint16_t InstancedRenderer::addPrimitive(VulkanBase& renderer,
                                              const ModelData& geometry) {
    if (geometry.vertices.empty() || geometry.indices.empty()) {
        throw std::invalid_argument("InstancedRenderer: пустая геометрия примитива");
    }
    Mesh* mesh = renderer.createMesh(geometry);
    Primitive primitive;
    primitive.mesh = mesh;
    primitive.triangles = static_cast<std::uint32_t>(geometry.indices.size() / 3);
    // Габариты берём из реальных вершин. Считать их «на глаз» по высоте вида
    // нельзя: размах кроны и высота ствола независимы, и ошибка в 1.5 м
    // означает либо обрезанную крону, либо вдвое больше лишней работы в
    // culling'е на каждом кадре.
    const vegetation::Bounds box = vegetation::boundsOf(geometry);
    primitive.boundCenter = 0.5f * (box.min + box.max);
    const glm::vec3 half = 0.5f * (box.max - box.min);
    // Сфера вокруг AABB: half-длина диагонали. Для сплюснутой кроны сфера
    // заметно больше нужного AABB, поэтому для culling'а держим и сам AABB.
    primitive.boundRadius = glm::length(half);
    primitives_.push_back(primitive);
    return static_cast<std::uint16_t>(primitives_.size() - 1);
}

void InstancedRenderer::addObjectNoReserve(const InstancedObject& object) {
    // Порциями по 4096: рассев добавляет десятки тысяч объектов, и точная
    // верхняя оценка потребовала бы второй выборки по всей площади.
    if (objects_.size() == objects_.capacity()) {
        objects_.reserve(objects_.size() + std::max<std::size_t>(4096, objects_.size() / 2));
    }
    objects_.push_back(object);
    sortDirty_ = true;
}

void InstancedRenderer::clearObjects() {
    objects_.clear();
    objectCapacityHint_ = 0;
    // Сортировка по примитиву стала неверной: индексы указывают на старые
    // объекты. ensureSorted() пересоберёт её, но флаг нужно сбросить явно —
    // иначе collectDraws() на пустом пуле вернёт пустой результат без
    // ошибки, а первый же новый объект отрисуется не в своей группе.
    sortDirty_ = true;
    sortScratch_.clear();
}

float InstancedRenderer::primitiveRadius(std::uint16_t primitive) const noexcept {
    if (primitive >= primitives_.size()) return 1.0f;
    return primitives_[primitive].boundRadius;
}

void InstancedRenderer::destroy(VulkanBase& renderer) {
    for (Primitive& primitive : primitives_) {
        if (primitive.mesh != nullptr) {
            renderer.destroyMesh(primitive.mesh);
            primitive.mesh = nullptr;
        }
    }
    primitives_.clear();
    objects_.clear();
    objects_.shrink_to_fit();
    sortScratch_.clear();
    sortScratch_.shrink_to_fit();
}

glm::mat4 InstancedRenderer::instanceMatrix(const InstancedObject& object,
                                            const awdm::dvec3& origin) noexcept {
    // Глобальная double-позиция -> локальные float: разность считается в
    // double и сужается один раз, здесь. Дальше T * R_y(yaw) * S(scale).
    const glm::dvec3 local = awdm::toLocal(object.globalPosition, origin);
    glm::mat4 model = glm::translate(glm::mat4{1.0f},
                                     glm::vec3(static_cast<float>(local.x),
                                                static_cast<float>(local.y),
                                                static_cast<float>(local.z)));
    model = glm::rotate(model, object.yawRadians, glm::vec3{0.0f, 1.0f, 0.0f});
    return glm::scale(model, glm::vec3{object.scale});
}

std::size_t InstancedRenderer::spawnTrees(VulkanBase& renderer, std::size_t count,
                                          const awdm::dvec3& areaCenterGlobal,
                                          double areaSizeMeters,
                                          HeightSampler heightSampler,
                                          void* samplerUserData, double minAboveSea,
                                          std::uint32_t seed) {
    if (primitives_.empty()) {
        // Примитив «дерево-палка» регистрируется лениво: одна загрузка mesh
        // на все деревья, повторные вызовы её пропускают.
        addPrimitive(renderer, makeStickTreeGeometry());
    }
    const std::uint16_t treePrimitive = 0;
    // Габаритный радиус юнит-дерева берём у примитива: он посчитан из
    // геометрии в addPrimitive(). Константа «2.6 м» здесь означала сферу
    // вокруг земли, а дерево высотой 4.1 м с основанием в Y=0 в неё не
    // помещалось — верхушка кроны выпадала из culling'а у самой камеры, и
    // деревья исчезали ровно тогда, когда игрок подходил к ним вплотную.
    const float kUnitRadius = primitiveRadius(treePrimitive);

    // Сетка размещения: ceil(sqrt(count)) x то же, шаг = размер области / N.
    const auto side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const double step = areaSizeMeters / static_cast<double>(side);
    const double halfArea = areaSizeMeters * 0.5;

    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> jitter{-0.45f, 0.45f};
    std::uniform_real_distribution<float> yawDist{0.0f, 2.0f * 3.14159265f};
    std::uniform_real_distribution<float> scaleDist{0.8f, 1.6f};

    objects_.reserve(objects_.size() + count);
    std::size_t placed = 0;
    for (std::size_t iz = 0; iz < side && placed < count; ++iz) {
        for (std::size_t ix = 0; ix < side && placed < count; ++ix) {
            const double gx = areaCenterGlobal.x - halfArea + (ix + 0.5 + jitter(rng)) * step;
            const double gz = areaCenterGlobal.z - halfArea + (iz + 0.5 + jitter(rng)) * step;
            const float h = heightSampler ? heightSampler(gx, gz, samplerUserData)
                                          : static_cast<float>(areaCenterGlobal.y);
            if (!(std::isfinite(h) && h > minAboveSea)) continue;  // вода/обрыв — мимо

            InstancedObject object;
            // Позиция — ГЛОБАЛЬНАЯ (gx/gz уже глобальные: центр области плюс
            // смещение в сетке). Локальные координаты относительно floating
            // origin вычисляются в instanceMatrix() в момент отрисовки, поэтому
            // сдвиг origin не требует пересоздания пула.
            object.globalPosition = glm::dvec3{gx, static_cast<double>(h), gz};
            object.yawRadians = yawDist(rng);
            object.scale = scaleDist(rng);
            object.radius = kUnitRadius * object.scale;
            object.primitive = treePrimitive;
            objects_.push_back(object);
            ++placed;
        }
    }

    core::Logger::info("InstancedRenderer: расставлено деревьев " + std::to_string(placed) +
                       " из запрошенных " + std::to_string(count) + " (примитивов: " +
                       std::to_string(primitives_.size()) + ", треугольников в примитиве: " +
                       std::to_string(primitives_[treePrimitive].triangles) + ")");
    return placed;
}

void InstancedRenderer::ensureSorted() const {
    if (!sortDirty_ && sortScratch_.size() == objects_.size()) return;
    sortScratch_.clear();
    sortScratch_.reserve(objects_.size());
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        sortScratch_.emplace_back(objects_[i].primitive, static_cast<std::uint32_t>(i));
    }
    // stable_sort: инстансы одного примитива идут подряд, порядок внутри группы
    // сохраняется (детерминированный кадр при прочих равных).
    std::stable_sort(sortScratch_.begin(), sortScratch_.end(),
                     [](const std::pair<std::uint16_t, std::uint32_t>& a,
                        const std::pair<std::uint16_t, std::uint32_t>& b) {
                         return a.first < b.first;
                     });
    sortDirty_ = false;
}

InstancedRenderer::CullStats InstancedRenderer::collectDraws(
    const Frustum& frustum, const awdm::dvec3& origin, std::vector<DrawData>& out) const {
    CullStats stats;
    stats.total = static_cast<std::uint32_t>(objects_.size());
    if (objects_.empty() || primitives_.empty()) return stats;

    // Сортируем индексы объектов по примитиву (один раз — stable после
    // создания пула; дальше переиспользуем порядок). Это нужно, чтобы
    // drawFrame() слил ВСЕ видимые деревья одного mesh в одну группу и один
    // vkCmdDrawIndexed с instanceCount=N вместо тысяч вызовов.
    ensureSorted();

    const std::size_t startOffset = out.size();
    out.reserve(startOffset + objects_.size());
    for (const std::pair<std::uint16_t, std::uint32_t>& entry : sortScratch_) {
        const InstancedObject& object = objects_[entry.second];
        const Primitive& primitive = primitives_[entry.first];
        if (primitive.mesh == nullptr) continue;
        // AABB в ЛОКАЛЬНЫХ координатах — ровно в тех же, что и плоскости
        // frustum (view-матрица камеры строится по position() = global - origin).
        // Считаем разность в double и только её сужаем: иначе на больших
        // расстояниях AABB «съезжает» на метры.
        const awdm::dvec3 local = awdm::toLocal(object.globalPosition, origin);
        // Габариты берём из ГЕОМЕТРИИ примитива, а не из object.radius,
        // выставленного при рассеве: примитив — источник истины, и
        // object.radius может разойтись с ним после смены геометрии.
        const double s = static_cast<double>(object.scale);
        // Центр габаритов повёрнут тем же yaw, что и инстанс: при повороте
        // вокруг Y смещение (cx, cy, cz) идёт в (cx·cos+cz·sin, cy,
        // -cx·sin+cz·cos). Поворот нужен, потому что крона смещена от оси
        // ствола, и без него AABB уезжал бы на ширину кроны.
        const float c = std::cos(object.yawRadians);
        const float sn = std::sin(object.yawRadians);
        const awdm::dvec3 center{local.x + s * static_cast<double>(primitive.boundCenter.x * c +
                                                                   primitive.boundCenter.z * sn),
                                 local.y + s * static_cast<double>(primitive.boundCenter.y),
                                 local.z + s * static_cast<double>(-primitive.boundCenter.x * sn +
                                                                    primitive.boundCenter.z * c)};
        const double r = static_cast<double>(primitive.boundRadius) * s;
        const AABB box{awdm::dvec3{center.x - r, center.y - r, center.z - r},
                       awdm::dvec3{center.x + r, center.y + r, center.z + r}};
        if (!frustum.intersectsAABB(box)) continue;
        // tint (малое отклонение от единицы) несёт климат точки: сухой склон
        // желтее, влажная тайга синее. У рельефа и прочих объектов, tint не
        // задающих, он остаётся единичным — см. InstancedObject::colorTint.
        out.push_back({primitive.mesh, instanceMatrix(object, origin), object.colorTint, /*lod=*/0});
        ++stats.visible;
    }

    // Число будущих draw calls = число разных примитивов среди принятых
    // (группы подряд идут: сортировка гарантирует смежность одного mesh).
    for (std::size_t i = startOffset; i < out.size(); ++i) {
        const Mesh* mesh = out[i].mesh;
        if (i == startOffset || out[i - 1].mesh != mesh) ++stats.drawCalls;
    }
    return stats;
}

}  // namespace renderer
