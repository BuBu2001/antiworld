#pragma once

#include <array>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "renderer/vertex.h"

namespace renderer {

class Buffer {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    void init(VkDevice device, VkPhysicalDevice physicalDevice, const void* data,
              VkDeviceSize size, VkBufferUsageFlags usage);
    void update(const void* data, VkDeviceSize size);
    void destroy();

    VkBuffer handle() const { return buffer_; }
    VkDeviceSize capacity() const { return capacity_; }

protected:
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize capacity_ = 0;
};

class VertexBuffer final : public Buffer {
public:
    VertexBuffer() = default;
    ~VertexBuffer() = default;

    void init(VkDevice device, VkPhysicalDevice physicalDevice, const Vertex* vertices,
              uint32_t count);
    uint32_t vertexCount() const { return vertexCount_; }

private:
    uint32_t vertexCount_ = 0;
};

class IndexBuffer final : public Buffer {
public:
    IndexBuffer() = default;
    ~IndexBuffer() = default;

    void init(VkDevice device, VkPhysicalDevice physicalDevice, const uint32_t* indices,
              uint32_t count);
    uint32_t indexCount() const { return indexCount_; }
    VkIndexType indexType() const { return VK_INDEX_TYPE_UINT32; }

private:
    uint32_t indexCount_ = 0;
};

const std::array<Vertex, 3>& triangleVertices();

}
