#pragma once

#include <vulkan/vulkan.h>

namespace renderer {

// Графический пайплайн: вершинный ввод (position + normal + UV + color +
// snowBias), шейдеры, rasterization, мультисэмплинг, color blend и
// depth/stencil.
class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline();

    // Владение уникальными Vulkan-ресурсами — копирование запрещено.
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    // Загружает вершинный/фрагментный шейдеры и создаёт пайплайн,
    // совместимый с переданным render pass'ом. descriptorSetLayout —
    // layout дескрипторного набора камеры (UBO MVP), хранится в
    // UniformBuffer; передаётся в VkPipelineLayoutCreateInfo.pSetLayouts.
    void init(VkDevice device, VkRenderPass renderPass,
              VkDescriptorSetLayout descriptorSetLayout);
    // Уничтожает пайплайн и layout; повторный вызов безопасен.
    void destroy();

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace renderer