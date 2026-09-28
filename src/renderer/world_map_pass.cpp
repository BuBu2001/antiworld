#include "renderer/world_map_pass.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

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

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("WorldMapPass: не найден тип памяти");
}

}  // namespace

#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

WorldMapPass::~WorldMapPass() { destroy(); }

void WorldMapPass::init(VkDevice device, VkPhysicalDevice physicalDevice,
                        VkRenderPass renderPass, uint32_t framesInFlight) {
    destroy();
    device_ = device;
    physicalDevice_ = physicalDevice;

    // set 0: binding 0 — combined image sampler (карта), binding 1 — UBO маркера.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_));

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = framesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_));

    // UBO на каждый кадр в полёте.
    frames_.resize(framesInFlight);
    const VkDeviceSize uboSize = sizeof(MapUniformObject);
    for (Frame& frame : frames_) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = uboSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &frame.buffer));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, frame.buffer, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &frame.memory));
        VK_CHECK(vkBindBufferMemory(device, frame.buffer, frame.memory, 0));
    }

    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, setLayout_);
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = pool_;
    setAlloc.descriptorSetCount = framesInFlight;
    setAlloc.pSetLayouts = layouts.data();
    descriptorSets_.resize(framesInFlight);
    VK_CHECK(vkAllocateDescriptorSets(device, &setAlloc, descriptorSets_.data()));

    pipeline_.init(device, renderPass, setLayout_, /*fullscreen=*/true);
    core::Logger::info("Vulkan: проход карты мира создан");
}

void WorldMapPass::setTexture(const Texture* texture) {
    texture_ = texture;
    if (texture_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < descriptorSets_.size(); ++i) {
        VkDescriptorImageInfo imageInfo = texture_->descriptor();
        VkDescriptorBufferInfo bufferInfo{frames_[i].buffer, 0, sizeof(MapUniformObject)};

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }
}

void WorldMapPass::update(uint32_t frameIndex, const MapUniformObject& data) {
    if (frameIndex >= frames_.size()) {
        return;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, frames_[frameIndex].memory, 0, sizeof(MapUniformObject), 0,
                    &mapped) != VK_SUCCESS) {
        return;
    }
    std::memcpy(mapped, &data, sizeof(MapUniformObject));
    vkUnmapMemory(device_, frames_[frameIndex].memory);
}

void WorldMapPass::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    pipeline_.destroy();
    for (Frame& frame : frames_) {
        if (frame.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.buffer, nullptr);
        }
        if (frame.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.memory, nullptr);
        }
    }
    frames_.clear();
    descriptorSets_.clear();
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    texture_ = nullptr;
}

}  // namespace renderer
