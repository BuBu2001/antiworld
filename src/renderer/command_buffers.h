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
    // fullscreen = true: вместо обхода meshDraws записывается ОДИН
    // vkCmdDraw(3, 1, 0, 0) — полноэкранный треугольник, вершины которого
    // шейдер синтезирует из gl_VertexIndex. Так карта мира рисуется тем же
    // вызовом record(), то есть внутри уже открытого render pass: рисовать
    // ПОСЛЕ record() нельзя, он закрывает pass.
    //
    // skyPipeline (не nullptr) — пайплайн неба, который рисуется ПОСЛЕ всей
    // геометрии, тем же вызовом и внутри того же pass. Его depthCompare =
    // EQUAL против очистки 1.0 сам отсекает пиксели, занятые ландшафтом,
    // поэтому порядок «сначала сцена, потом небо» не нужен — нужен лишь
    // порядок записей, чтобы небо перекрывало там, где сцена не записала
    // глубину, и не перекрывало ничего лишнего.
    void record(VkCommandBuffer commandBuffer, const RenderPass& renderPass,
                size_t framebufferIndex, VkExtent2D extent,
                GraphicsPipeline& pipeline, std::span<const MeshDraw> meshDraws,
                VkClearColorValue clearColor, VkDescriptorSet descriptorSet,
                VkBuffer instanceBuffer, bool fullscreen = false,
                GraphicsPipeline* skyPipeline = nullptr);

    VkCommandBuffer commandBuffer(uint32_t index) const { return commandBuffers_[index]; }
    // Пул для одноразовых command buffer'ов (загрузка текстур). Основной пул
    // и так существует на время жизни renderer'а, поэтому отдельный создавать
    // ради staging-буферов не нужно.
    VkCommandPool commandPool() const { return commandPool_; }
    uint32_t count() const { return static_cast<uint32_t>(commandBuffers_.size()); }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
};

}  // namespace renderer