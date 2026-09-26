#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "renderer/model_loader.h"
#include "renderer/vertex_buffer.h"

namespace renderer {

class Mesh;

struct DrawData {
    DrawData() = default;
    DrawData(const Mesh* meshHandle, glm::mat4 modelMatrix)
        : mesh(meshHandle), model(modelMatrix) {}

    const Mesh* mesh{nullptr};
    glm::mat4 model{1.0f};
};

// Группа объектов кадра, которые рисуются одной mesh одним instanced-вызовом:
// матрицы лежат в instance-буфере подряд, начиная с firstInstance.
// Формируется в VulkanBase::drawFrame() из DrawData, consumed в
// CommandBuffers::record().
struct MeshDraw {
    const Mesh* mesh{nullptr};
    uint32_t firstInstance{0};
    uint32_t instanceCount{0};
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
