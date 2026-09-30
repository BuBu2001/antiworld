#include "renderer/hud_pass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

#include "core/logger.h"
#include "renderer/hud_font.h"

namespace renderer {
namespace {

void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error (") + std::to_string(result) +
                                 ") в " + expr + " [" + file + ":" + std::to_string(line) +
                                 "]");
    }
}

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
    throw std::runtime_error("HudPass: не найден тип памяти");
}

// Растягивает атлас R8 в RGBA8: Texture умеет только VK_FORMAT_R8G8B8A8_UNORM,
// а свой формат не принимает. Атлас 96x32 — это 12 КБ, разница не имеет
// значения, зато переиспользование Texture не требует его переделывать.
std::vector<uint8_t> toRgba(const HudFontAtlas& atlas) {
    std::vector<uint8_t> rgba(static_cast<std::size_t>(atlas.width) * atlas.height * 4, 0);
    for (std::size_t i = 0; i < atlas.pixels.size(); ++i) {
        rgba[i * 4 + 0] = atlas.pixels[i];
        rgba[i * 4 + 1] = atlas.pixels[i];
        rgba[i * 4 + 2] = atlas.pixels[i];
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

}  // namespace

#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

HudPass::~HudPass() { destroy(); }

void HudPass::init(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
                   VkQueue queue, VkRenderPass renderPass, uint32_t framesInFlight) {
    destroy();
    device_ = device;
    physicalDevice_ = physicalDevice;

    // Атлас шрифта: детерминированный, грузится один раз при создании.
    const HudFontAtlas atlas = buildHudFontAtlas();
    const std::vector<uint8_t> rgba = toRgba(atlas);
    atlasTexture_.init(device, physicalDevice, commandPool, queue, atlas.width, atlas.height,
                       rgba.data(), rgba.size());

    // set 0: binding 0 — атлас шрифта, binding 1 — UBO строки.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_));

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = framesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_));

    // Свой UBO на каждый кадр в полёте: два кадра в полёте писали бы в одну
    // память, и текст на экране «дёргался» бы на длину кадра.
    frames_.resize(framesInFlight);
    const VkDeviceSize uboSize = sizeof(HudUniformObject);
    for (Frame& frame : frames_) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = uboSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &frame.buffer));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, frame.buffer, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &frame.memory));
        VK_CHECK(vkBindBufferMemory(device, frame.buffer, frame.memory, 0));
    }

    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, setLayout_);
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = pool_;
    setAlloc.descriptorSetCount = framesInFlight;
    setAlloc.pSetLayouts = layouts.data();
    descriptorSets_.resize(framesInFlight);
    VK_CHECK(vkAllocateDescriptorSets(device, &setAlloc, descriptorSets_.data()));

    for (std::size_t i = 0; i < descriptorSets_.size(); ++i) {
        VkDescriptorImageInfo imageInfo = atlasTexture_.descriptor();
        VkDescriptorBufferInfo bufferInfo{frames_[i].buffer, 0, sizeof(HudUniformObject)};

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }

    pipeline_.init(device, renderPass, setLayout_, PipelineMode::Hud);
    core::Logger::info("Vulkan: HUD (счётчик FPS) создан, атлас шрифта " +
                       std::to_string(atlas.width) + "x" + std::to_string(atlas.height));
}

void HudPass::packText(const std::string_view& text, glm::uvec4 (&dst)[16]) {
    for (glm::uvec4& word : dst) {
        word = glm::uvec4(0u);
    }
    const std::size_t count = std::min<std::size_t>(text.size(), kHudMaxChars);
    for (std::size_t i = 0; i < count; ++i) {
        // Атлас покрывает 32..95, поэтому приводим к верхнему регистру: иначе
        // строчные буквы выводились бы пустыми ячейками без всякого предупреждения.
        char c = text[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        const uint32_t code = static_cast<uint32_t>(static_cast<unsigned char>(c));
        dst[i / 4][i % 4] = (code < 32u || code > 95u) ? 32u : code;
    }
}

void HudPass::update(uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight,
                     std::string_view text, float glyphScale) {
    if (frameIndex >= frames_.size()) {
        return;
    }
    HudUniformObject data{};
    data.viewport.x = static_cast<float>(windowWidth);
    data.viewport.y = static_cast<float>(windowHeight);
    data.viewport.z = glyphScale;
    data.viewport.w = static_cast<float>(std::min<std::size_t>(text.size(), kHudMaxChars));
    packText(text, data.text);

    void* mapped = nullptr;
    if (vkMapMemory(device_, frames_[frameIndex].memory, 0, sizeof(HudUniformObject), 0,
                    &mapped) != VK_SUCCESS) {
        return;
    }
    std::memcpy(mapped, &data, sizeof(HudUniformObject));
    vkUnmapMemory(device_, frames_[frameIndex].memory);

    // Прямоугольник растеризации повторяет панель из hud.frag: ряд ячеек плюс
    // отступ вокруг текста. Ширина/высота в texel'ях, умноженные на масштаб.
    const float chars = data.viewport.w;
    const float scale = glyphScale;
    const auto w = static_cast<int32_t>(std::lround(chars * kHudCellW * scale + 8.0f * scale));
    const auto h = static_cast<int32_t>(std::lround(kHudCellH * scale + 6.0f * scale));
    scissor_.offset = {static_cast<int32_t>(std::lround(data.origin.x)),
                       static_cast<int32_t>(std::lround(data.origin.y))};
    scissor_.extent = {std::max(w, 1), std::max(h, 1)};
}

void HudPass::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    pipeline_.destroy();
    for (Frame& frame : frames_) {
        if (frame.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.buffer, nullptr);
        }
        if (frame.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.memory, nullptr);
        }
    }
    frames_.clear();
    descriptorSets_.clear();
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    atlasTexture_.destroy();
    device_ = VK_NULL_HANDLE;
    scissor_ = {{0, 0}, {0, 0}};
}

}  // namespace renderer
