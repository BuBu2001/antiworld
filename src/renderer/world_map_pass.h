#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "renderer/pipeline.h"
#include "renderer/texture.h"

namespace renderer {

// Содержимое UBO карты (std140: один vec4 = 16 байт, выравнивание совпадает
// с GLSL). Соответствует блоку MapUniforms в shaders/map.frag.
struct MapUniformObject {
    // xy — UV игрока на карте, z — радиус маркера в UV, w — 1, если игрок есть
    // (0 — маркер не рисуем, например до появления капсулы).
    glm::vec4 player{0.0f, 0.0f, 0.004f, 0.0f};
};

// Проход карты мира: пайплайн полноэкранного треугольника, descriptor set с
// текстурой карты и UBO, по одному набору на каждый кадр в полёте (как у
// UniformBuffer сцены) — иначе два кадра в полёте писали бы в один UBO.
class WorldMapPass {
public:
    WorldMapPass() = default;
    ~WorldMapPass();

    WorldMapPass(const WorldMapPass&) = delete;
    WorldMapPass& operator=(const WorldMapPass&) = delete;

    // Создаёт пайплайн, layout, pool и per-frame наборы. renderPass нужен
    // пайплайну (совместимость с форматом swapchain).
    void init(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass,
              uint32_t framesInFlight);

    // Привязывает готовую текстуру карты ко всем наборам дескрипторов.
    void setTexture(const Texture* texture);

    // Обновляет UBO кадра (маркер игрока).
    void update(uint32_t frameIndex, const MapUniformObject& data);

    void destroy();

    GraphicsPipeline& pipeline() noexcept { return pipeline_; }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const {
        return descriptorSets_[frameIndex];
    }
    bool ready() const noexcept { return !descriptorSets_.empty() && texture_ != nullptr; }

private:
    struct Frame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    GraphicsPipeline pipeline_;
    const Texture* texture_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
    std::vector<Frame> frames_;
};

}  // namespace renderer
