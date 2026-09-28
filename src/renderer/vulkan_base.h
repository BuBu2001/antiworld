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
#include "renderer/frustum_culling.h"
#include "renderer/mesh.h"
#include "renderer/pipeline.h"
#include "renderer/render_pass.h"
#include "renderer/uniform_buffer.h"
#include "renderer/world_map_pass.h"

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

    // === Карта мира ===
    //
    // Загружает готовую картинку мира в текстуру и поднимает проход карты.
    // pixels — RGBA8, width*height*4 байт, tightly packed. Вызывается один
    // раз, когда world::WorldMap закончила генерацию в фоне.
    void createWorldMap(const void* pixels, uint32_t width, uint32_t height,
                        size_t pixelBytes);
    // Рисует кадр карты: полноэкранный треугольник с текстурой мира и
    // маркером игрока. Список объектов пустой — 3D-кадр в этом вызове не рисуется.
    void drawWorldMap(const MapUniformObject& uniform);
    // Готова ли карта к отрисовке (текстура загружена, проход создан).
    bool worldMapReady() const noexcept { return worldMapPass_.ready(); }

    // Освобождает все Vulkan-ресурсы в правильном порядке.
    void cleanup();

    // === Кадровая статистика (для HUD: доказательство работы culling/LOD) ===
    // Число реальных draw-вызовов, записанных в командный буфер последнего
    // отрисованного кадра (после frustum culling на CPU — см. CullingStats).
    std::uint32_t lastFrameDrawCalls() const noexcept { return lastDrawCalls_; }
    // Кандидатов было (до отсечения) — из последнего drawFrame().
    std::uint32_t lastFrameCandidates() const noexcept {
        return static_cast<std::uint32_t>(sortScratch_.size());
    }

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
    void recreateSyncPrimitives();

    // Общая для всех кадров (сцена и карта) синхронизация: ожидание fence,
    // захват изображения swapchain, отправка и презентация. Вынесено отдельно,
    // потому что drawFrame и drawWorldMap обязаны вести себя ОДИНАКОВО: своя
    // копия этого кода в двух местах — верный способ однажды забыть fence или
    // обработку OUT_OF_DATE в одном из путей.
    struct FrameAcquire {
        uint32_t imageIndex = 0;
        bool skip = false;  // swapchain устарел — кадр не рисуем
    };
    FrameAcquire acquireFrame();
    void submitAndPresent(VkCommandBuffer commandBuffer, uint32_t imageIndex);

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
    // Карта мира: текстура (не зависит от swapchain и переживает resize) и
    // проход (пайплайн зависит от render pass, поэтому пересоздаётся вместе с ним).
    Texture worldMapTexture_;
    WorldMapPass worldMapPass_;
    Mesh mesh_;
    // Прочие mesh (террейн и т.п.), созданные через createMesh.
    std::vector<std::unique_ptr<Mesh>> ownedMeshes_;
    std::array<Buffer, kMaxFramesInFlight> instanceBuffers_;
    UniformBuffer uniformBuffer_;
    CommandBuffers commandBuffers_;

    // Рабочая копия кадра для группировки по mesh в drawFrame(). drawFrame()
    // принимает span<const DrawData>, сортировать его на месте нельзя, а
    // переставлять элементы у вызывающего за спиной не вежливо. Буфер
    // переиспользуется между кадрами, чтобы не аллоцировать в горячем пути.
    std::vector<DrawData> sortScratch_;
    // Рабочие буферы групп (mesh, LOD) и матриц — тоже переиспользуются.
    std::vector<MeshDraw> meshDrawScratch_;
    std::vector<glm::mat4> modelMatrixScratch_;

    // Draw-вызовы последнего записанного командного буфера (статистика HUD).
    std::uint32_t lastDrawCalls_{0};

    // Синхронизация: по набору на каждый кадр в полёте.
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;

    core::Window* window_ = nullptr;  // не владеет окном, только ссылается
};

}  // namespace renderer