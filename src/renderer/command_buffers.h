#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "renderer/mesh.h"

namespace renderer {

class GraphicsPipeline;
class RenderPass;

// Набор командных буферов (по одному на кадр в полёте) с записью отрисовки
// mesh: begin render pass (очистка) -> bind pipeline -> bind buffers -> draw -> end render pass.
class CommandBuffers {
public:
    CommandBuffers() = default;
    ~CommandBuffers();

    // Владение уникальными Vulkan-ресурсами — копирование запрещено.
    CommandBuffers(const CommandBuffers&) = delete;
    CommandBuffers& operator=(const CommandBuffers&) = delete;

    // Создаёт command pool и выделяет framesInFlight командных буферов.
    void init(VkDevice device, uint32_t graphicsQueueFamily, uint32_t framesInFlight);
    // Уничтожает command pool (вместе с буферами); повторный вызов безопасен.
    void destroy();

    // Записывает команды кадра в commandBuffer: очистку экрана и по одному
    // indexed draw на каждую группу meshDraws. Все группы читают матрицы
    // объектов из общего instance-буфера, каждая — со своим смещением.
    // pipeline намеренно НЕ const: record() ведёт в нём счётчик draw-вызовов
    // (GraphicsPipeline::resetDrawCallCounter / countDrawCall), который
    // вызывающий читает после возврата.
    void record(VkCommandBuffer commandBuffer, const RenderPass& renderPass,
                size_t framebufferIndex, VkExtent2D extent,
                GraphicsPipeline& pipeline, std::span<const MeshDraw> meshDraws,
                VkClearColorValue clearColor, VkDescriptorSet descriptorSet,
                VkBuffer instanceBuffer);

    VkCommandBuffer commandBuffer(uint32_t index) const { return commandBuffers_[index]; }
    uint32_t count() const { return static_cast<uint32_t>(commandBuffers_.size()); }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
};

}  // namespace renderer