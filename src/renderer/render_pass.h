#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace renderer {

// VkRenderPass с color- и depth-аттачментами и набором VkFramebuffer по одному
// на изображение swapchain.
class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass();

    // Владение уникальными Vulkan-ресурсами — копирование запрещено.
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    // Создаёт render pass под формат изображений swapchain.
    void init(VkDevice device, VkFormat swapChainFormat);
    // Создаёт framebuffer для каждого view изображения swapchain.
    void createFramebuffers(VkDevice device, VkExtent2D extent,
                            const std::vector<VkImageView>& colorImageViews,
                            VkImageView depthImageView);

    // Уничтожает framebuffer'ы и render pass; повторный вызов безопасен.
    void destroy();

    VkRenderPass handle() const { return renderPass_; }
    VkFramebuffer framebuffer(size_t index) const { return framebuffers_[index]; }

private:
    void destroyFramebuffers();

    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
};

}  // namespace renderer