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
    // fullscreen = false — обычная сцена: вершины из vertex buffer, шаг
    // sizeof(Vertex), инстанс-матрицы в locations 7..10, depth test включён.
    //
    // fullscreen = true — режим карты мира: шейдер сам синтезирует три
    // вершины из gl_VertexIndex, поэтому vertex input не нужен вовсе, а
    // depth test/write выключены (карта рисуется поверх очищенного кадра).
    // Так карта не трогает формат вершины сцены (шаг 56 байт) и её
    // locations, о которых договаривались шейдеры terrain/tree/water.
    void init(VkDevice device, VkRenderPass renderPass,
              VkDescriptorSetLayout descriptorSetLayout, bool fullscreen = false);
    // Уничтожает пайплайн и layout; повторный вызов безопасен.
    void destroy();

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

    // Число draw-вызовов, записанных в текущий командный буфер (сбрасывается
    // при vkCmdBeginCommandBuffer). Нужно для HUD: frustum culling обязан
    // реально снижать это число от кадра к кадру. Счётчик ведётся внутри
    // CommandBuffers::record(), которая вызывает инлайновые методы пайплайна.
    uint32_t lastDrawCalls() const noexcept { return drawCalls_; }
    void resetDrawCallCounter() noexcept { drawCalls_ = 0; }
    void countDrawCall() noexcept { ++drawCalls_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t drawCalls_{0};
};

}  // namespace renderer