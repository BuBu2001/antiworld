#include "renderer/command_buffers.h"

#include <array>
#include <stdexcept>

#include "core/logger.h"
#include "renderer/mesh.h"
#include "renderer/pipeline.h"
#include "renderer/render_pass.h"

namespace renderer {

namespace {

// Проверка результата вызова Vulkan; при ошибке — исключение.
void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error (") + std::to_string(result) +
                                 ") в " + expr + " [" + file + ":" + std::to_string(line) +
                                 "]");
    }
}

}  // namespace

#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

CommandBuffers::~CommandBuffers() {
    destroy();
}

void CommandBuffers::init(VkDevice device, uint32_t graphicsQueueFamily,
                          uint32_t framesInFlight) {
    device_ = device;

    // RESET_COMMAND_BUFFER_BIT — разрешаем перезапись буфера между кадрами.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;

    VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));

    commandBuffers_.resize(framesInFlight);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()));
    core::Logger::info("CommandBuffers: созданы командные буферы (" +
                       std::to_string(commandBuffers_.size()) + " шт.)");
}

void CommandBuffers::record(VkCommandBuffer commandBuffer, const RenderPass& renderPass,
                            size_t framebufferIndex, VkExtent2D extent,
                            GraphicsPipeline& pipeline,
                            std::span<const MeshDraw> meshDraws, VkClearColorValue clearColor,
                            VkDescriptorSet descriptorSet, VkBuffer instanceBuffer,
                            bool fullscreen, GraphicsPipeline* skyPipeline) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    // Статистика кадра: сколько реальных draw-вызовов попало в командный буфер
    // ПОСЛЕ отсечения (см. GraphicsPipeline::lastDrawCalls / HUD).
    pipeline.resetDrawCallCounter();

    // Begin render pass: экран очищается цветом и глубиной, затем рисуется mesh.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass.handle();
    renderPassInfo.framebuffer = renderPass.framebuffer(framebufferIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = clearColor;
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    // Динамический viewport/scissor на весь прикреплённый буфер кадра.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // UBO с MVP-матрицами камеры (set 0, binding 0) общий для всех mesh кадра.
    if (descriptorSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout(), 0, 1, &descriptorSet, 0, nullptr);
    }

    // По одному instanced-вызову на группу (mesh, LOD): вершины и индексы —
    // из своей mesh (для чанков это отдельный индексный буфер уровня LOD),
    // матрицы объектов — из общего instance-буфера со смещением на начало
    // группы. Именно здесь отсечённые CPU-отсечением чанки НЕ порождают
    // вызовов: их просто нет в meshDraws.
    if (fullscreen) {
        // Три вершины, один инстанс, без вершинного и индексного буферов.
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        pipeline.countDrawCall();
    }

    for (const MeshDraw& draw : meshDraws) {
        if (draw.mesh == nullptr || !draw.mesh->initialized() || draw.instanceCount == 0 ||
            instanceBuffer == VK_NULL_HANDLE) {
            continue;
        }

        const std::array<VkBuffer, 2> vertexBufferHandles = {draw.mesh->vertexBuffer(),
                                                             instanceBuffer};
        // Смещение(instance-матрицы) кратно sizeof(glm::mat4) — это же длина
        // экземпляра в пайплайне, поэтому вершины читаются с правильного места.
        const std::array<VkDeviceSize, 2> offsets = {
            0, static_cast<VkDeviceSize>(draw.firstInstance) * sizeof(glm::mat4)};
        vkCmdBindVertexBuffers(commandBuffer, 0,
                               static_cast<uint32_t>(vertexBufferHandles.size()),
                               vertexBufferHandles.data(), offsets.data());
        vkCmdBindIndexBuffer(commandBuffer, draw.mesh->indexBuffer(), 0,
                             draw.mesh->indexType());

        // Инстанс-буфер уже содержит по матрице на каждый экземпляр группы,
        // поэтому используется классический indexed instanced draw:
        // instanceCount экземпляров за один вызов (10 000 «деревьев»
        // InstancedRenderer'а — это ровно ОДНА такая строка/один вызов).
        //
        // firstInstance здесь 0 СОЗНАТЕЛЬНО: смещение на матрицы группы задаёт
        // pOffsets[1] в vkCmdBindVertexBuffers выше (firstInstance * sizeof(mat4)),
        // а binding 1 объявлен с inputRate = VK_VERTEX_INPUT_RATE_INSTANCE, так
        // что атрибуты 6..9 читаются от этого смещения. Если продублировать
        // смещение ещё и в firstInstance draw'а, группа N прочитает матрицы
        // с 2N, а последняя группа уйдёт за конец буфера.
        vkCmdDrawIndexed(commandBuffer, draw.mesh->indexCount(), draw.instanceCount, 0, 0,
                         0);
        pipeline.countDrawCall();
    }

    // Небо — после всей геометрии, последним draw'ом кадра. Оно само
    // отсекается по глубине (EQUAL против очистки 1.0), так что перекрывать
    // ландшафт не может; рисовать его раньше бессмысленно — небо стёрлось бы
    // первым же треугольником чанка.
    if (skyPipeline != nullptr) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          skyPipeline->handle());
        // Тот же набор дескрипторов, что у сцены: UBO кадра один на оба
        // пайплайна, и перепривязывать его не нужно.
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        skyPipeline->countDrawCall();
    }

    vkCmdEndRenderPass(commandBuffer);

    VK_CHECK(vkEndCommandBuffer(commandBuffer));
}
void CommandBuffers::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    // Command buffers освобождаются вместе с pool.
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    commandBuffers_.clear();
}

}  // namespace renderer