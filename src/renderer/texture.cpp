#include "renderer/texture.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace renderer {
namespace {

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
    throw std::runtime_error("Texture: не найден подходящий тип памяти");
}

// Один буфер команд на загрузку. Ждём завершения через fence: без этого
// vkQueueSubmit может вернуться раньше, чем GPU закончит копирование, и
// освобождение staging-буфера было бы гонкой с GPU.
VkCommandBuffer beginOneShot(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS) {
        throw std::runtime_error("Texture: не удалось выделить command buffer");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Texture: не удалось начать command buffer");
    }
    return cmd;
}

void endOneShot(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Texture: не удалось закончить command buffer");
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        throw std::runtime_error("Texture: не удалось создать fence");
    }
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        vkDestroyFence(device, fence, nullptr);
        throw std::runtime_error("Texture: vkQueueSubmit не удался");
    }
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

}  // namespace

Texture::~Texture() { destroy(); }

void Texture::createImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width,
                          uint32_t height) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("Texture: vkCreateImage не удался");
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_, &requirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory_) != VK_SUCCESS) {
        throw std::runtime_error("Texture: не удалось выделить память изображения");
    }
    if (vkBindImageMemory(device, image_, imageMemory_, 0) != VK_SUCCESS) {
        throw std::runtime_error("Texture: vkBindImageMemory не удался");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView_) != VK_SUCCESS) {
        throw std::runtime_error("Texture: vkCreateImageView не удался");
    }
}

void Texture::init(VkDevice device, VkPhysicalDevice physicalDevice,
                   VkCommandPool commandPool, VkQueue queue, uint32_t width,
                   uint32_t height, const void* pixels, size_t pixelBytes) {
    destroy();
    device_ = device;
    physicalDevice_ = physicalDevice;
    width_ = width;
    height_ = height;
    createImage(device, physicalDevice, width, height);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // CLAMP_TO_EDGE, а не REPEAT: за краем карты показываем крайнюю точку, а
    // не зеркалим материки — зеркалирование читалось бы как «ещё одна суша».
    // U — долгота, и на сфере u=0 и u=1 это ОДИН меридиан. С CLAMP_TO_EDGE
    // билинейная фильтрация усредняла бы последний пиксель с первым, то есть
    // рисовала бы шов шириной в один тексель поверх шва данных. REPEAT даёт
    // корректную фильтрацию через край.
    //
    // V — широта, и periodic там нельзя: полюса не периодичны, полюс — это
    // одна точка, а не петля. Поэтому V остаётся CLAMP_TO_EDGE.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Texture: vkCreateSampler не удался");
    }

    upload(commandPool, queue, pixels, pixelBytes);
}

void Texture::upload(VkCommandPool commandPool, VkQueue queue, const void* pixels,
                     size_t pixelBytes) {
    const VkDeviceSize size = pixelBytes;
    if (stagingBuffer_ == VK_NULL_HANDLE || stagingSize_ != size) {
        destroyStaging();
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("Texture: staging-буфер не создан");
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, stagingBuffer_, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            physicalDevice_, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMemory_) != VK_SUCCESS) {
            throw std::runtime_error("Texture: память staging-буфера не выделена");
        }
        if (vkBindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0) != VK_SUCCESS) {
            throw std::runtime_error("Texture: staging-буфер не привязан к памяти");
        }
        stagingSize_ = size;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, stagingMemory_, 0, size, 0, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("Texture: vkMapMemory не удался");
    }
    std::memcpy(mapped, pixels, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, stagingMemory_);

    VkCommandBuffer cmd = beginOneShot(device_, commandPool);
    VkImageMemoryBarrier toTransfer = barrier(image_, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_ACCESS_MEMORY_WRITE_BIT,
                                              VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    VkImageMemoryBarrier toRead = barrier(image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);
    endOneShot(device_, queue, commandPool, cmd);
}

void Texture::update(VkCommandPool commandPool, VkQueue queue, const void* pixels,
                     size_t pixelBytes) {
    if (image_ == VK_NULL_HANDLE || pixelBytes != static_cast<size_t>(stagingSize_)) {
        return;
    }
    upload(commandPool, queue, pixels, pixelBytes);
}

void Texture::destroyStaging() {
    if (stagingBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, stagingBuffer_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
    }
    if (stagingMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, stagingMemory_, nullptr);
        stagingMemory_ = VK_NULL_HANDLE;
    }
    stagingSize_ = 0;
}

void Texture::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    destroyStaging();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (imageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, imageView_, nullptr);
        imageView_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (imageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, imageMemory_, nullptr);
        imageMemory_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    width_ = 0;
    height_ = 0;
}

VkImageMemoryBarrier Texture::barrier(VkImage image, VkImageLayout oldLayout,
                                      VkImageLayout newLayout, VkAccessFlags srcAccess,
                                      VkAccessFlags dstAccess) {
    VkImageMemoryBarrier result{};
    result.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    result.oldLayout = oldLayout;
    result.newLayout = newLayout;
    result.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    result.image = image;
    result.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    result.subresourceRange.levelCount = 1;
    result.subresourceRange.layerCount = 1;
    result.srcAccessMask = srcAccess;
    result.dstAccessMask = dstAccess;
    return result;
}

}  // namespace renderer
