#include "renderer/instanced_renderer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

#include "core/logger.h"
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
    primitives_.push_back({mesh, static_cast<std::uint32_t>(geometry.indices.size() / 3)});
    return static_cast<std::uint16_t>(primitives_.size() - 1);
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
    // Локальные float-координаты: глобальная позиция минус floating origin
    // (см. core/floating_origin.h). Позиция объекта уже хранится локально,
    // поэтому здесь только T * R_y(yaw) * S(scale).
    glm::mat4 model = glm::translate(glm::mat4{1.0f}, object.position);
    model = glm::rotate(model, object.yawRadians, glm::vec3{0.0f, 1.0f, 0.0f});
    return glm::scale(model, glm::vec3{object.scale});
    (void)origin;  // позиция уже локальная; параметр оставлен для API-ясности
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
    // Bounding radius юнит-дерева (ствол+крона): по высоте ~4.1 м, по ширине
    // ~1.13 м -> консервативно берём описывающую сферу вокруг центра формы.
    constexpr float kUnitRadius = 2.6f;

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
            // Храним ЛОКАЛЬНУЮ позицию (floating origin вычитается при спавне;
            // при сдвиге origin игрок пересоздаёт пул — для теста это ок, и
            // именно так же поступают ECS-компоненты чанков).
            object.position = glm::vec3{static_cast<float>(gx - areaCenterGlobal.x),
                                        h,
                                        static_cast<float>(gz - areaCenterGlobal.z)};
            // Смещаем к центру области в ЛОКАЛЬНЫХ координатах относительно
            // переданного areaCenter (он же и есть временной origin пула).
            object.position += awdm::toLocal(areaCenterGlobal, areaCenterGlobal);
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
        // Консервативный AABB: сфера радиуса radius вокруг позиции (в double,
        // глобальные координаты — точность как у чанкового culling).
        const awdm::dvec3 global = awdm::toGlobal(
            awdm::dvec3{static_cast<double>(object.position.x),
                        static_cast<double>(object.position.y),
                        static_cast<double>(object.position.z)},
            origin);
        const double r = static_cast<double>(object.radius);
        const AABB box{awdm::dvec3{global.x - r, global.y - r, global.z - r},
                       awdm::dvec3{global.x + r, global.y + r, global.z + r}};
        if (!frustum.intersectsAABB(box)) continue;
        const Primitive& primitive = primitives_[entry.first];
        if (primitive.mesh == nullptr) continue;
        out.push_back({primitive.mesh, instanceMatrix(object, origin), /*lod=*/0});
        ++stats.visible;    }

    // Число будущих draw calls = число разных примитивов среди принятых
    // (группы подряд идут: сортировка гарантирует смежность одного mesh).
    for (std::size_t i = startOffset; i < out.size(); ++i) {
        const Mesh* mesh = out[i].mesh;
        if (i == startOffset || out[i - 1].mesh != mesh) ++stats.drawCalls;
    }
    return stats;
}

}  // namespace renderer
