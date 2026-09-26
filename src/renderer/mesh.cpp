#include "renderer/mesh.h"

#include <limits>
#include <stdexcept>

namespace renderer {

Mesh::~Mesh() {
    destroy();
}

void Mesh::init(VkDevice device, VkPhysicalDevice physicalDevice,
                const std::vector<Vertex>& vertices,
                const std::vector<uint32_t>& indices, const Material& material) {
    if (vertices.empty()) {
        throw std::runtime_error("Mesh: нельзя создать пустую вершинную сетку");
    }
    if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
        indices.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Mesh: слишком много вершин или индексов");
    }
    if (indices.empty() || indices.size() % 3 != 0) {
        throw std::runtime_error("Mesh: индексы должны быть кратны трём");
    }
    for (const uint32_t index : indices) {
        if (index >= vertices.size()) {
            throw std::runtime_error("Mesh: индекс вершины выходит за границы");
        }
    }

    vertexBuffer_.init(device, physicalDevice, vertices.data(),
                       static_cast<uint32_t>(vertices.size()));
    indexBuffer_.init(device, physicalDevice, indices.data(),
                      static_cast<uint32_t>(indices.size()));
    material_ = material;
}

void Mesh::init(VkDevice device, VkPhysicalDevice physicalDevice, const ModelData& model,
                const Material& material) {
    init(device, physicalDevice, model.vertices, model.indices, material);
}

void Mesh::destroy() {
    indexBuffer_.destroy();
    vertexBuffer_.destroy();
    material_ = Material{};
}

}
