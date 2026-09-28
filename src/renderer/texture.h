#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace renderer {

// Текстура Vulkan: VkImage + память + view + sampler.
//
// В движке до этого не было НИ ОДНОЙ текстуры — только буферы. Появилась
// вместе с картой мира: её пиксели нельзя отдать шейдеру иначе, чем через
// combined image sampler.
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Создаёт изображение и samplер, затем заливает его пикселями через
    // промежуточный staging-буфер и переводит в VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    //
    // Формат: VK_FORMAT_R8G8B8A8_UNORM, sampling — линейный, для карты это
    // обязательно (иначе при растяжении на весь экран видны квадраты пикселей).
    // addressMode — CLAMP_TO_EDGE: за пределами карты показываем край, а не
    // зеркалим материки (зеркалирование читалось бы как «ещё одна суша»).
    void init(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
              VkQueue queue, uint32_t width, uint32_t height,
              const void* pixels, size_t pixelBytes);

    // Перезаливает изображение теми же размерами (staging-буфер переиспользуется).
    void update(VkCommandPool commandPool, VkQueue queue, const void* pixels,
                size_t pixelBytes);

    void destroy();

    VkImageView view() const { return imageView_; }
    VkSampler sampler() const { return sampler_; }
    VkDescriptorImageInfo descriptor() const {
        return VkDescriptorImageInfo{sampler_, imageView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool initialized() const { return image_ != VK_NULL_HANDLE; }

private:
    void createImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width,
                     uint32_t height);
    void upload(VkCommandPool commandPool, VkQueue queue, const void* pixels,
                size_t pixelBytes);
    void destroyStaging();
    // Барьер для перевода между layout'ами (загрузка -> чтение шейдером).
    static VkImageMemoryBarrier barrier(VkImage image, VkImageLayout oldLayout,
                                        VkImageLayout newLayout, VkAccessFlags srcAccess,
                                        VkAccessFlags dstAccess);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingSize_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace renderer
