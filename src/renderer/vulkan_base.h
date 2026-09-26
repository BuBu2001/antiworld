#pragma once

// GLFW_INCLUDE_VULKAN подключает Vulkan API после GLFW, чтобы были доступны
// функции работы с поверхностями (VkSurfaceKHR, glfwCreateWindowSurface).
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include <glm/glm.hpp>

#include <vulkan/vulkan.h>

#include "renderer/command_buffers.h"
#include "renderer/mesh.h"
#include "renderer/pipeline.h"
#include "renderer/render_pass.h"
#include "renderer/uniform_buffer.h"

namespace core {
class Window;
}

namespace renderer {

// Базовый слой Vulkan: instance -> surface -> физическое устройство ->
// логическое устройство -> swapchain -> render pass + pipeline -> синхронизация
// кадров. Render pass, пайплайн, mesh и командные буферы вынесены
// в отдельные классы (RenderPass, GraphicsPipeline, Mesh, CommandBuffers).
// Формат поверхности, режим презентации и размер буфера кадра.
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// Индексы семейств очередей, требуемые приложению.
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;  // семейство с поддержкой графики
    std::optional<uint32_t> present;   // семейство с поддержкой презентации

    bool isComplete() const {
        return graphics.has_value() && present.has_value();
    }
};

class VulkanBase {
public:
    // Максимальное число кадров, «летящих» через GPU одновременно.
    static constexpr uint32_t kMaxFramesInFlight = 2;

    VulkanBase() = default;
    ~VulkanBase();

    // Окно нельзя копировать (владение уникальными Vulkan-ресурсами).
    VulkanBase(const VulkanBase&) = delete;
    VulkanBase& operator=(const VulkanBase&) = delete;

    // Полная инициализация: createInstance + surface + device + swapchain и т.д.
    void init(core::Window& window);

    void uploadMesh(const ModelData& model, const Material& material = Material{});
    const Mesh* mesh() const { return &mesh_; }

    // Создаёт дополнительную mesh (террейн, декали и т.п.) и возвращает
    // указатель на неё. Владелец — renderer: он же освобождает mesh в
    // cleanup(), а вызывающий хранит только указатель (например, world::Terrain
    // кладёт его в ecs::MeshRenderer) и может досрочно удалить через
    // destroyMesh(). Адрес Mesh не меняется при добавлении других mesh.
    Mesh* createMesh(const ModelData& model, const Material& material = Material{});
    // Удаляет ранее созданную mesh и освобождает её буферы. Для встроенной
    // mesh (uploadMesh) и неизвестных указателей — исключение.
    void destroyMesh(const Mesh* mesh);

    // Отрисовывает один кадр: захват изображения из swapchain, очистка экрана
    // цветом и отрисовка всех переданных объектов (drawData) в render pass,
    // отправка на презентацию. Объекты группируются по mesh: на каждую mesh
    // приходится один instanced-вызов.
    void drawFrame(const glm::mat4& viewProjection, std::span<const DrawData> drawData);

    // То же, но с освещением и климатом кадра. environment кладётся в UBO
    // вместе с матрицей камеры, поэтому шейдеры видят их согласованно.
    // Версия без environment рисует солнце в зените и без снега.
    void drawFrame(const glm::mat4& viewProjection, std::span<const DrawData> drawData,
                   const FrameEnvironment& environment);

    // Освобождает все Vulkan-ресурсы в правильном порядке.
    void cleanup();

private:
    void createInstance();
    void createDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();

    // Render pass + framebuffer'ы + пайплайн зависят от swapchain
    // (формат/размер), поэтому создаются/уничтожаются вместе с ним.
    void createRenderTargets();
    void destroyRenderTargets();

    void createSyncObjects();

    // Пересоздание swapchain при изменении размера окна.
    void cleanupSwapChain();
    void recreateSwapChain();

    // Проверяет, что mesh создана этим renderer (встроенная или через
    // createMesh): иначе в drawFrame ушёл бы указатель на чужой объект.
    bool ownsMesh(const Mesh* mesh) const noexcept;

    // Поиск требуемых семейств очередей конкретного физического устройства.
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    // Опрос возможностей поверхности: capabilities, форматы, present modes.
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const;

    // Вспомогательные функции выбора параметров swapchain.
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    static VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& formats);
    static VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& presentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    // --- Дескрипторы Vulkan (создаются и освобождаются в init/cleanup) ---
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;  // только в debug
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;  // не требует destroy
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    VkFormat swapChainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapChainExtent_{};
    std::vector<VkImage> swapChainImages_;
    std::vector<VkImageView> swapChainImageViews_;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;

    // Модульные объекты рендера.
    RenderPass renderPass_;
    GraphicsPipeline pipeline_;
    Mesh mesh_;
    // Прочие mesh (террейн и т.п.), созданные через createMesh.
    std::vector<std::unique_ptr<Mesh>> ownedMeshes_;
    std::array<Buffer, kMaxFramesInFlight> instanceBuffers_;
    UniformBuffer uniformBuffer_;
    CommandBuffers commandBuffers_;

    // Синхронизация: по набору на каждый кадр в полёте.
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;

    core::Window* window_ = nullptr;  // не владеет окном, только ссылается
};

}  // namespace renderer