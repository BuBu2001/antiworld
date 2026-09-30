#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "renderer/model_loader.h"
#include "renderer/vertex_buffer.h"

namespace renderer {

class Mesh;

// Уровень детализации объекта кадра (см. renderer::LodMesh / lod_manager.h).
// 0 — полный mesh; для объектов без LOD-наборов всегда 0.
inline constexpr int kMaxLodLevels = 3;

struct DrawData {
    DrawData() = default;
    DrawData(const Mesh* meshHandle, glm::mat4 modelMatrix)
        : mesh(meshHandle), model(modelMatrix) {}
    DrawData(const Mesh* meshHandle, glm::mat4 modelMatrix, std::uint8_t level)
        : mesh(meshHandle), model(modelMatrix), lod(level) {}
    // Полная форма: матрица, tint, LOD. Порядок полей в struct именно такой,
    // поэтому braced-init перечисляет их по имени, а не по позиции.
    DrawData(const Mesh* meshHandle, glm::mat4 modelMatrix, glm::vec3 tintValue,
             std::uint8_t level)
        : mesh(meshHandle), model(modelMatrix), tint(tintValue), lod(level) {}

    const Mesh* mesh{nullptr};
    glm::mat4 model{1.0f};
    // Множитель альбедо конкретного инстанса (см. InstanceData::tint). Нужен
    // растительности: вид и вариант задают форму, а климат и хеш точки — оттенок
    // (сухой склон желтее, затенённая тайга синее). Значение по умолчанию —
    // единица, то есть «не красить», поэтому рельеф и все прочие объекты,
    // tint не задающие, выглядят как раньше.
    glm::vec3 tint{1.0f, 1.0f, 1.0f};
    // Выбранный на CPU уровень LOD (см. world::ChunkManager::updateCulling).
    std::uint8_t lod{0};
};

// Одна запись instance-буфера: матрица модели + множитель альбедо.
//
// Раньше инстанс был ровно glm::mat4, поэтому binding имел stride 64 байта.
// Добавлять tint «рядом с матрицей в том же буфере» — правильное решение: один
// binding вместо второго, а шаг атрибута остаётся вычисляемым компилятором
// (offsetof), поэтому добавление поля не сдвинет геометрию в памяти.
struct InstanceData {
    glm::mat4 model{1.0f};
    // w не используется, но vec4 удобнее для Vulkan-формата R32G32B32A32_SFLOAT
    // и оставляет место для, например, индивидуальной густоты снега.
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// ЕДИНСТВЕННОЕ место, где задан шаг инстанса. Его используют ТРИ независимых
// места: stride в vertex input пайплайна,sizeof(InstanceData) при заполнении
// буфера в VulkanBase::drawFrame и смещение firstInstance в
// CommandBuffers::record. Раньше шаг был продублирован литералом, и когда в
// InstanceData добавили tint, два из трёх мест остались с 64 байтами — world
// собирался, запускался и рисовал «кучу артефактов» без единого
// предупреждения. Объявление-константа делает рассинхрон невозможным: чтобы
// сломать шаг, придётся сломать компиляцию.
inline constexpr VkDeviceSize kInstanceStride = sizeof(InstanceData);
static_assert(kInstanceStride == 80, "шаг инстанса изменился — проверь атрибуты в pipeline.cpp");

// Группа объектов кадра, которые рисуются одной mesh одним instanced-вызовом:
// матрицы лежат в instance-буфере подряд, начиная с firstInstance.
// Формируется в VulkanBase::drawFrame() из DrawData, consumed в
// CommandBuffers::record().
//
// Группировка идёт по паре (mesh, lod): один и тот же чанк на разных LOD —
// разные индексные буферы, поэтому вызовы разные; но тысячи инстансов ОДНОГО
// mesh+LOD (растительность) по-прежнему сливаются в один draw call.
struct MeshDraw {
    const Mesh* mesh{nullptr};
    uint32_t firstInstance{0};
    uint32_t instanceCount{0};
    std::uint8_t lod{0};
};

struct Material {
    glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void init(VkDevice device, VkPhysicalDevice physicalDevice,
              const std::vector<Vertex>& vertices,
              const std::vector<uint32_t>& indices,
              const Material& material = Material{});
    void init(VkDevice device, VkPhysicalDevice physicalDevice,
              const ModelData& model, const Material& material = Material{});

    void destroy();

    VkBuffer vertexBuffer() const { return vertexBuffer_.handle(); }
    VkBuffer indexBuffer() const { return indexBuffer_.handle(); }
    uint32_t indexCount() const { return indexBuffer_.indexCount(); }
    VkIndexType indexType() const { return indexBuffer_.indexType(); }
    bool initialized() const {
        return vertexBuffer_.handle() != VK_NULL_HANDLE && indexBuffer_.handle() != VK_NULL_HANDLE;
    }

    const Material& material() const { return material_; }
    void setMaterial(const Material& material) { material_ = material; }

private:
    VertexBuffer vertexBuffer_;
    IndexBuffer indexBuffer_;
    Material material_;
};

}
