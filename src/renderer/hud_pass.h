#pragma once

#include <cstdint>
#include <string_view>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include "renderer/pipeline.h"
#include "renderer/texture.h"

namespace renderer {

// Содержимое UBO HUD (std140: два vec4 по 16 байт и 16 uvec4 по 16 байт).
// Соответствует блоку HudUniforms в shaders/hud.frag.
struct HudUniformObject {
    // x — ширина окна в пикселях (нужна шейдеру, чтобы перевернуть Y: в Vulkan
    // gl_FragCoord растёт вверх, а HUD прижат к верхней кромке), y — высота,
    // z — размер глифа в экранных пикселях, w — число символов в строке.
    glm::vec4 viewport{0.0f, 0.0f, 3.0f, 0.0f};
    // xy — отступ от левого верхнего угла окна до панели HUD, в пикселях.
    glm::vec4 origin{10.0f, 8.0f, 0.0f, 0.0f};
    // До 64 символов строки, по одному ASCII-коду на uint.
    glm::uvec4 text[16]{};
};

// std140 требует выравнивания 16 байт; при несовпадении размера структура
// молча перестанет соответствовать тому, что ждёт шейдер.
static_assert(sizeof(HudUniformObject) == 288,
              "HudUniformObject должен совпадать с std140-блоком в hud.frag");

// Счётчик FPS в левом верхнем углу.
//
// Отдельный проход, а не часть сцены: он рисуется последним, поверх геометрии
// и неба, с альфа-смешиванием и без depth test. Шрифта в проекте нет (единственный
// ассет — cube.obj), поэтому символы берутся из атласа 96x32, который
// генерируется на CPU в hud_font.cpp.
//
// Ресурсы на каждый кадр в полёте — свой UBO, как у UniformBuffer сцены и
// WorldMapPass: иначе два кадра в полёте писали бы в одну память.
class HudPass {
public:
    HudPass() = default;
    ~HudPass();

    HudPass(const HudPass&) = delete;
    HudPass& operator=(const HudPass&) = delete;

    // Создаёт пайплайн, layout, пул, per-frame UBO и атлас шрифта. renderPass
    // нужен пайплайну (совместимость с форматом swapchain), commandPool и
    // queue — загрузке атласа.
    void init(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
              VkQueue queue, VkRenderPass renderPass, uint32_t framesInFlight);

    // Готовит строку к отрисовке в кадре frameIndex: переводит в верхний
    // регистр (атлас покрывает только 32..95), режет по kHudMaxChars и
    // пересчитывает прямоугольник растеризации под длину строки.
    //
    // ВАЖНО: atlasTexture() надо вызвать до update() — update читает из атласа
    // коды символов, чтобы разложить строку по uvec4.
    void update(uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight,
                std::string_view text, float glyphScale);

    void destroy();

    bool ready() const noexcept { return !descriptorSets_.empty() && atlasTexture_.initialized(); }
    GraphicsPipeline& pipeline() noexcept { return pipeline_; }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const {
        return frameIndex < descriptorSets_.size() ? descriptorSets_[frameIndex] : VK_NULL_HANDLE;
    }

    // Прямоугольник растеризации HUD в координатах фреймбуфера (origin —
    // левый ВЕРХНИЙ угол, как у VkRect2D). Передаётся в vkCmdSetScissor,
    // чтобы шейдер не выполнялся на всех пикселях кадра ради сотни пикселей
    // текста: на карте, которую мы разгрузили ограничением кадра, лишний
    // полноэкранный проход заметен.
    const VkRect2D& scissor() const noexcept { return scissor_; }

private:
    struct Frame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    // Раскладывает строку в uvec4[16] блока UBO.
    static void packText(const std::string_view& text, glm::uvec4 (&dst)[16]);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    GraphicsPipeline pipeline_;
    Texture atlasTexture_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
    std::vector<Frame> frames_;
    VkRect2D scissor_{{0, 0}, {0, 0}};
};

}  // namespace renderer
