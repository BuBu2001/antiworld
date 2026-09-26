#include "renderer/vertex_buffer.h"

#include <cstring>
#include <stdexcept>

#include "core/logger.h"

namespace renderer {

namespace {

void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error (") + std::to_string(result) +
                                 ") в " + expr + " [" + file + ":" + std::to_string(line) +
                                 "]");
    }
}

}  // namespace

#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

const std::array<Vertex, 3>& triangleVertices() {
    static const std::array<Vertex, 3> vertices = {{
        {{-0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}},
    }};
    return vertices;
}

Buffer::~Buffer() {
    destroy();
}

void Buffer::init(VkDevice device, VkPhysicalDevice physicalDevice, const void* data,
                  VkDeviceSize size, VkBufferUsageFlags usage) {
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("Buffer: Vulkan device не инициализирован");
    }
    if (size == 0 || data == nullptr) {
        throw std::runtime_error("Buffer: данные буфера пусты");
    }

    destroy();
    device_ = device;
    physicalDevice_ = physicalDevice;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex =
        findMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VK_CHECK(vkAllocateMemory(device_, &allocationInfo, nullptr, &memory_));
    VK_CHECK(vkBindBufferMemory(device_, buffer_, memory_, 0));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, memory_, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, memory_);
    capacity_ = size;
}

void Buffer::update(const void* data, VkDeviceSize size) {
    if (data == nullptr || size == 0 || size > capacity_) {
        throw std::runtime_error("Buffer: размер обновления превышает capacity");
    }

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, memory_, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, memory_);
}

void Buffer::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    capacity_ = 0;
}

uint32_t Buffer::findMemoryType(uint32_t typeFilter,
                                VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Buffer: не найден подходящий тип памяти");
}

void VertexBuffer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                        const Vertex* vertices, uint32_t count) {
    if (count == 0 || vertices == nullptr) {
        throw std::runtime_error("VertexBuffer: данные вершин пусты");
    }
    vertexCount_ = 0;
    Buffer::init(device, physicalDevice, vertices,
                 static_cast<VkDeviceSize>(count) * sizeof(Vertex),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertexCount_ = count;
    core::Logger::info("VertexBuffer: создан буфер на " + std::to_string(count) + " вершин");
}

void IndexBuffer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                       const uint32_t* indices, uint32_t count) {
    if (count == 0 || indices == nullptr) {
        throw std::runtime_error("IndexBuffer: данные индексов пусты");
    }
    indexCount_ = 0;
    Buffer::init(device, physicalDevice, indices,
                 static_cast<VkDeviceSize>(count) * sizeof(uint32_t),
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    indexCount_ = count;
    core::Logger::info("IndexBuffer: создан буфер на " + std::to_string(count) + " индексов");
}

}
