#include "renderer/uniform_buffer.h"

#include <cstring>
#include <stdexcept>

namespace renderer {

// Деструктор освобождает Vulkan-ресурсы, если владелец не вызвал destroy().
UniformBuffer::~UniformBuffer() {
    destroy();
}

// Тип памяти под UBO: HOST_VISIBLE (обновляем с CPU каждый кадр) + HOST_COHERENT
// (копия сразу видна GPU без явного flush).
uint32_t UniformBuffer::findMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("uniform buffer: не найден подходящий тип памяти");
}

void UniformBuffer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                         uint32_t framesInFlight) {
    device_ = device;
    physicalDevice_ = physicalDevice;

    // 1) Descriptor set layout: binding 0 — uniform buffer. Доступен и вершинному,
    //    и фрагментному шейдерам: матрицу камеры читает вершинный, а солнце,
    //    ambient и frost — фрагментный.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                    &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("uniform buffer: не удалось создать descriptor set layout");
    }

    // 2) Descriptor pool: по одному descriptor set'у на каждый кадр в полёте.
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = framesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = framesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("uniform buffer: не удалось создать descriptor pool");
    }

    // 3) По одному uniform-буферу + descriptor set'у на каждый кадр в полёте.
    const VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    buffers_.resize(framesInFlight, VK_NULL_HANDLE);
    buffersMemory_.resize(framesInFlight, VK_NULL_HANDLE);
    descriptorSets_.resize(framesInFlight, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < framesInFlight; ++i) {
        // Буфер (host-visible — пишем с CPU каждый кадр).
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("uniform buffer: не удалось создать буфер");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device_, buffers_[i], &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &buffersMemory_[i]) != VK_SUCCESS) {
            throw std::runtime_error("uniform buffer: не удалось выделить память");
        }
        vkBindBufferMemory(device_, buffers_[i], buffersMemory_[i], 0);

        // Descriptor set.
        VkDescriptorSetAllocateInfo allocSetInfo{};
        allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocSetInfo.descriptorPool = descriptorPool_;
        allocSetInfo.descriptorSetCount = 1;
        allocSetInfo.pSetLayouts = &descriptorSetLayout_;

        if (vkAllocateDescriptorSets(device_, &allocSetInfo, &descriptorSets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("uniform buffer: не удалось выделить descriptor set");
        }

        // Связываем буфер с дескриптором (descriptor write).
        VkDescriptorBufferInfo bufferDescriptorInfo{};
        bufferDescriptorInfo.buffer = buffers_[i];
        bufferDescriptorInfo.offset = 0;
        bufferDescriptorInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets_[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferDescriptorInfo;

        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
    }
}

void UniformBuffer::update(uint32_t frameIndex, const UniformBufferObject& data) {
    // Отображаем память UBO кадра (host-visible + coherent — копия видна GPU
    // сразу, без явного flush).
    void* ptr = nullptr;
    vkMapMemory(device_, buffersMemory_[frameIndex], 0, sizeof(UniformBufferObject), 0, &ptr);
    std::memcpy(ptr, &data, sizeof(UniformBufferObject));
    vkUnmapMemory(device_, buffersMemory_[frameIndex]);
}

void UniformBuffer::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    for (auto buffer : buffers_) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer, nullptr);
        }
    }
    buffers_.clear();

    for (auto memory : buffersMemory_) {
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory, nullptr);
        }
    }
    buffersMemory_.clear();

    descriptorSets_.clear();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }

    device_ = VK_NULL_HANDLE;
}

}  // namespace renderer
