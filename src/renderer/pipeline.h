#pragma once

#include <vulkan/vulkan.h>

namespace renderer {

// Какой набор шейдеров и какой режим ввода/глубины у пайплайна. Раньше это был
// булев fullscreen, но появился третий случай — небо, — который отличается от
// карты тем, что depth test у него ВКЛЮЧЁН (сравнение EQUAL против очистки
// 1.0), а запись выключена. Булевым флагом это уже не выражается.
enum class PipelineMode {
    // Обычная сцена: вершины из vertex buffer, шаг sizeof(Vertex),
    // инстанс-матрицы в locations 7..10, depth test/write включены.
    Scene,
    // Полноэкранный треугольник поверх очищенного кадра: вершин нет вообще
    // (gl_VertexIndex), depth отключён. Так рисуется карта мира.
    Fullscreen,
    // Небо: вершин нет, но depth test EQUAL + запись выключена, поэтому
    // шейдер выполняется только там, где сцена не записала глубину.
    // Требует рисования ПОСЛЕ геометрии сцены.
    Sky,
};

class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline();

    // Владение уникальными Vulkan-ресурсами — копирование запрещено.
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    // Загружает вершинный/фрагментный шейдеры и создаёт пайплайн,
    // совместимый с переданным render pass'ом. descriptorSetLayout —
    // layout дескрипторного набора камеры (UBO кадра), хранится в
    // UniformBuffer; передаётся в VkPipelineLayoutCreateInfo.pSetLayouts.
    //
    // mode выбирает пару шейдеров (triangle/map/sky) и режим ввода вершин и
    // глубины — см. PipelineMode. Смешивание (cull/depth write) у всех
    // режимов одинаковое, различается только то, откуда берутся вершины.
    void init(VkDevice device, VkRenderPass renderPass,
              VkDescriptorSetLayout descriptorSetLayout, PipelineMode mode = PipelineMode::Scene);
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