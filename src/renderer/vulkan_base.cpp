#include "renderer/vulkan_base.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "core/logger.h"
#include "core/window.h"

namespace renderer {

namespace {

// Включение validation layers только в debug-сборке (NDEBUG отключён).
#if defined(NDEBUG)
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

const std::vector<const char*> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};
const std::vector<const char*> kDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

// Тёмно-синий цвет очистки экрана (RGBA, 0.0 - 1.0).
const VkClearColorValue kClearColor{0.07f, 0.08f, 0.24f, 1.0f};
// Очистка под картой мира: почти чёрный синий, чтобы кадр не «мигал» между
// картой и очисткой в местах, где текстура ещё не успела отрисоваться.
const VkClearColorValue kMapClearColor{0.02f, 0.03f, 0.07f, 1.0f};

// Глобальная проверка результата вызова Vulkan; при ошибке бросает исключение.
void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error (") + std::to_string(result) +
                                 ") в " + expr + " [" + file + ":" + std::to_string(line) +
                                 "]");
    }
}

}  // namespace

// Макрос для единообразной проверки: VK_CHECK(vkCreateDevice(...)).
#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

namespace {

// Колбэк validation layers: сообщения попадают в core::Logger.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/) {
    switch (severity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            core::Logger::error(callbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            core::Logger::warn(callbackData->pMessage);
            break;
        default:
            core::Logger::info(callbackData->pMessage);
            break;
    }
    return VK_FALSE;  // не прерываем вызов (VK_TRUE = вызов abort)
}

// Заполнение структур описания debug-messenger (общая для instance и messenger).
void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info) {
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = &debugCallback;
}

// Проверка, что запрошенные validation layers доступны в системе.
void checkValidationLayerSupport() {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : kValidationLayers) {
        const bool found = std::any_of(
            availableLayers.begin(), availableLayers.end(),
            [=](const VkLayerProperties& props) {
                return std::strcmp(props.layerName, layerName) == 0;
            });
        if (!found) {
            throw std::runtime_error(std::string("Validation layer не найден: ") +
                                     layerName);
        }
    }
}

// Поддерживает ли устройство требуемые расширения?
bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& extension : availableExtensions) {
        required.erase(extension.extensionName);
    }
    return required.empty();
}

// Простая оценка устройства: чем больше балл, тем охотнее его выбираем.
int rateDeviceSuitability(VkPhysicalDevice device) {
    int score = 0;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device, &features);

    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 300;  // дискретная видеокарта лучше всего
    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 100;  // встроенное видео — запасной вариант
    }
    if (features.geometryShader) {
        score += 50;
    }
    return score;
}

}  // namespace

VulkanBase::~VulkanBase() {
    cleanup();
}

void VulkanBase::init(core::Window& window) {
    window_ = &window;

    createInstance();
    if (kEnableValidation) {
        createDebugMessenger();
    }
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapChain();
    createImageViews();
    createRenderTargets();
    createSyncObjects();

    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    commandBuffers_.init(device_, *indices.graphics, kMaxFramesInFlight);

    // HUD инициализируется именно здесь, а не в createRenderTargets(): атлас
    // шрифта грузится через одноразовый command buffer, поэтому пул command
    // buffers к этому моменту должен уже существовать. В createRenderTargets
    // он создаётся двумя строками ниже, и vkAllocateCommandBuffers на
    // VK_NULL_HANDLE роняет процесс.
    hudPass_.init(device_, physicalDevice_, commandBuffers_.commandPool(), graphicsQueue_,
                  renderPass_.handle(), kMaxFramesInFlight);

    core::Logger::info("Vulkan: инициализация завершена");
}

void VulkanBase::uploadMesh(const ModelData& model, const Material& material) {
    if (device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Vulkan: нельзя загрузить mesh до инициализации renderer");
    }
    mesh_.init(device_, physicalDevice_, model, material);
    core::Logger::info("Vulkan: mesh загружена (" +
                       std::to_string(model.vertices.size()) + " вершин, " +
                       std::to_string(model.indices.size()) + " индексов)");
}

Mesh* VulkanBase::createMesh(const ModelData& model, const Material& material) {
    if (device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Vulkan: нельзя создать mesh до инициализации renderer");
    }

    auto mesh = std::make_unique<Mesh>();
    mesh->init(device_, physicalDevice_, model, material);
    core::Logger::info("Vulkan: создана mesh (" + std::to_string(model.vertices.size()) +
                       " вершин, " + std::to_string(model.indices.size()) + " индексов)");

    // Адрес Mesh не меняется при росте вектора, поэтому возвращаемый указатель
    // остаётся валидным, пока mesh не удалена через destroyMesh().
    Mesh* handle = mesh.get();
    ownedMeshes_.push_back(std::move(mesh));
    return handle;
}

void VulkanBase::destroyMesh(const Mesh* mesh) {
    const auto it = std::find_if(ownedMeshes_.begin(), ownedMeshes_.end(),
                                 [mesh](const std::unique_ptr<Mesh>& owned) {
                                     return owned.get() == mesh;
                                 });
    if (it == ownedMeshes_.end()) {
        throw std::invalid_argument("Vulkan: destroyMesh — mesh не создана этим renderer");
    }

    // Порядок важен: сначала ждём завершения всех команд, использующих буферы
    // mesh, иначе уничтожение буфера может обогнать GPU.
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    ownedMeshes_.erase(it);
}

bool VulkanBase::ownsMesh(const Mesh* mesh) const noexcept {
    if (mesh == &mesh_) {
        return true;
    }
    return std::any_of(ownedMeshes_.begin(), ownedMeshes_.end(),
                       [mesh](const std::unique_ptr<Mesh>& owned) {
                           return owned.get() == mesh;
                       });
}

void VulkanBase::createRenderTargets() {
    renderPass_.init(device_, swapChainImageFormat_);
    renderPass_.createFramebuffers(device_, swapChainExtent_, swapChainImageViews_,
                                   depthImageView_);
    uniformBuffer_.init(device_, physicalDevice_, kMaxFramesInFlight);
    pipeline_.init(device_, renderPass_.handle(), uniformBuffer_.layout());
    // Небо живёт в том же render pass и читает тот же UBO кадра, поэтому
    // набор дескрипторов отдельный ему не нужен — меняется только пайплайн.
    skyPipeline_.init(device_, renderPass_.handle(), uniformBuffer_.layout(),
                      PipelineMode::Sky);
    // Проход карты пересоздаём вместе с render pass: его пайплайн собран под
    // формат swapchain. Текстура карты при этом сохраняется — от swapchain она
    // не зависит, и перезаливать 32 МБ пикселей при каждом resize незачем.
    worldMapPass_.init(device_, physicalDevice_, renderPass_.handle(), kMaxFramesInFlight);
    if (worldMapTexture_.initialized()) {
        worldMapPass_.setTexture(&worldMapTexture_);
    }
}

void VulkanBase::destroyRenderTargets() {
    worldMapPass_.destroy();
    hudPass_.destroy();
    skyPipeline_.destroy();
    pipeline_.destroy();
    uniformBuffer_.destroy();
    renderPass_.destroy();
}

void VulkanBase::drawFrame(const glm::mat4& viewProjection,
                           std::span<const DrawData> drawData) {
    drawFrame(viewProjection, drawData, FrameEnvironment{});
}

void VulkanBase::drawFrame(const glm::mat4& viewProjection,
                           std::span<const DrawData> drawData,
                           const FrameEnvironment& environment) {
    if (drawData.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Vulkan: слишком много объектов в кадре");
    }

    // Раскладываем объекты кадра по парам (mesh, LOD): подряд идущие объекты
    // с одной mesh и одним уровнем детализации объединяются в группу, на
    // каждую группу — один instanced-вызов. Матрицы групп пишем в
    // instance-буфер в том же порядке, firstInstance указывает на начало
    // матриц группы.
    // Сортировка по (mesh, lod) гарантирует, что все инстансы одной пары идут
    // подряд, независимо от порядка обхода ECS (иначе один mesh дробился бы на
    // множество отдельных vkCmdDrawIndexed).
    //
    // ВАЖНО про culling: отсечение по пирамиде видимости выполняется НА CPU до
    // вызова drawFrame() (см. world::ChunkManager::updateCulling) — в span
    // drawData попадают только чанки, реально пересекающие frustum. Поэтому
    // число групп здесь = числу реальных draw call'ов кадра.
    //
    // Сортируем копию: drawFrame() получает span<const DrawData>, а std::sort
    // требует изменяемых итераторов. Сортировать сам span нельзя, а менять
    // порядок у вызывающего мы не вправе — const-обязательство параметра.
    sortScratch_.assign(drawData.begin(), drawData.end());
    std::sort(sortScratch_.begin(), sortScratch_.end(),
              [](const DrawData& a, const DrawData& b) {
                  // std::less, а не сырой < : указатели на НЕСВЯЗАННЫЕ объекты
                  // (разные Mesh, выделенные в разных местах) сравнивать через
                  // operator< нельзя — результат не определён стандартом.
                  // std::sort требует строгого слабого порядка, а потому
                  // компаратор обязан быть транзитивным на всей области.
                  // std::less гарантирует тотальный порядок для любых
                  // указателей (сначала std::less<> по типу, потом адрес).
                  if (a.mesh != b.mesh) return std::less<const Mesh*>{}(a.mesh, b.mesh);
                  return a.lod < b.lod;
              });

    meshDrawScratch_.clear();
    instanceScratch_.clear();
    meshDrawScratch_.reserve(sortScratch_.size());
    instanceScratch_.reserve(sortScratch_.size());
    for (const DrawData& data : sortScratch_) {
        if (!ownsMesh(data.mesh)) {
            throw std::runtime_error("Vulkan: mesh handle не принадлежит renderer");
        }

        if (!meshDrawScratch_.empty() && meshDrawScratch_.back().mesh == data.mesh &&
            meshDrawScratch_.back().lod == data.lod) {
            ++meshDrawScratch_.back().instanceCount;
        } else {
            meshDrawScratch_.push_back({data.mesh,
                                        static_cast<uint32_t>(instanceScratch_.size()), 1,
                                        data.lod});
        }
        instanceScratch_.push_back(InstanceData{data.model, glm::vec4(data.tint, 1.0f)});
    }
    const std::span<const MeshDraw> meshDraws{meshDrawScratch_};

    const FrameAcquire acquired = acquireFrame();
    if (acquired.skip) {
        return;
    }
    const uint32_t imageIndex = acquired.imageIndex;

    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    uint32_t instanceCount = static_cast<uint32_t>(instanceScratch_.size());
    if (instanceCount != 0) {
        const VkDeviceSize instanceDataSize =
            static_cast<VkDeviceSize>(instanceScratch_.size() * sizeof(InstanceData));
        Buffer& buffer = instanceBuffers_[currentFrame_];
        if (buffer.capacity() < instanceDataSize) {
            buffer.init(device_, physicalDevice_, instanceScratch_.data(), instanceDataSize,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        } else {
            buffer.update(instanceScratch_.data(), instanceDataSize);
        }
        instanceBuffer = buffer.handle();
    }

    VkCommandBuffer commandBuffer = commandBuffers_.commandBuffer(currentFrame_);
    vkResetCommandBuffer(commandBuffer, 0);
    UniformBufferObject frameData{};
    frameData.viewProjection = viewProjection;
    // Солнце нормализуем здесь: в UBO и в шейдер уходит единичный вектор,
    // чтобы длину не пришлось учитывать в каждом шейдере отдельно.
    frameData.sunDirection =
        glm::vec4{glm::normalize(environment.sunDirection), environment.sunIntensity};
    // x — frost, y — ambient, z — seaLevel (уровень моря из географии мира),
    // w — время суток 0..1. Раньше здесь стоял фиксированный полдень, из-за
    // чего сумеречное небо и ночные звёзды были недостижимы: небо появлялось
    // всегда при свете, будто съёмка велась в полдень.
    frameData.environment = glm::vec4{environment.frost, environment.ambient,
                                      environment.seaLevel, environment.dayTime};
    // x — есть ли в мире океан: без флага шейдер не рисует воду даже если
    // какая-то вершина случайно оказалась ниже уровня моря (география выкл.).
    frameData.waterFlags = glm::vec4{environment.hasWater, 0.0f, 0.0f, 0.0f};
    // Обратная viewProjection для неба: у него нет вершин, поэтому луч
    // взгляда восстанавливается только так. Обратную матрицу считаем здесь
    // же — на GPU инверсия 4x4 ничего не стоит, а на CPU пришлось бы тянуть
    // инверсию в FrameEnvironment и рисковать рассинхроном.
    frameData.inverseViewProjection = glm::inverse(viewProjection);
    // Позиция камеры нужна небу для параллакса облаков: без неё слой
    // «приклеен» к камере и не сдвигается при ходьбе.
    frameData.cameraPosition =
        glm::vec4{environment.cameraPosition, environment.dayTime};
    // x — время в секундах (анимация ветра и облаков), y — сила ветра,
    // z — покрытость неба облаками. Пока облака не заданы настройкой мира,
    // берётся значение по умолчанию — примерно половина неба в кучевой
    // облачности, как в средней полосе.
    frameData.wind = glm::vec4{environment.timeSeconds, environment.windStrength,
                               environment.cloudCover, 0.0f};
    // Сдвиг floating origin: рендер считает в локальных координатах, а небу
    // нужны мировые (проекция на слой облаков).
    frameData.worldOrigin = glm::vec4{environment.worldOrigin, 0.0f};
    uniformBuffer_.update(currentFrame_, frameData);
    // HUD обновляем здесь же: UBO кадра в полёте уже наш, и scissor должен быть
    // посчитан до record(), который его записывает.
    if (hudVisible_ && hudPass_.ready()) {
        hudPass_.update(currentFrame_, swapChainExtent_.width, swapChainExtent_.height, hudText_,
                        hudGlyphScale_);
    }
    commandBuffers_.record(commandBuffer, renderPass_, imageIndex, swapChainExtent_,
                           pipeline_, meshDraws, kClearColor,
                           uniformBuffer_.descriptorSet(currentFrame_), instanceBuffer,
                           /*fullscreen=*/false, &skyPipeline_, hudOverlay());
    // Статистика для HUD: фактическое число draw-вызовов кадра (уже после
    // frustum culling на стороне вызывающего — см. CullingStats).
    lastDrawCalls_ = pipeline_.lastDrawCalls();

    submitAndPresent(commandBuffer, imageIndex);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

VulkanBase::FrameAcquire VulkanBase::acquireFrame() {
    FrameAcquire result{};

    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    const VkResult acquireResult = vkAcquireNextImageKHR(
        device_, swapChain_, std::numeric_limits<uint64_t>::max(),
        imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &result.imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        // ВНИМАНИЕ: семафор imageAvailable_[currentFrame_] был передан в acquire
        // и мог быть сигнален даже при ошибке. Пересоздаём синхронизационные
        // примитивы, чтобы на следующем кадре не переиспользовать «висячий»
        // сигнальный семафор (иначе vkQueueSubmit будет ждать никогда не
        // сбрасываемый сигнал либо использовать уже знавший семафор).
        // Перед удалением убеждаемся, что GPU завершил все операции.
        vkDeviceWaitIdle(device_);
        recreateSyncPrimitives();
        recreateSwapChain();
        result.skip = true;
        return result;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Vulkan: не удалось захватить изображение swapchain");
    }

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
    return result;
}

void VulkanBase::submitAndPresent(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    const std::array<VkSemaphore, 1> waitSemaphores = {
        imageAvailableSemaphores_[currentFrame_]};
    const std::array<VkPipelineStageFlags, 1> waitStages = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT};
    const std::array<VkSemaphore, 1> signalSemaphores = {
        renderFinishedSemaphores_[currentFrame_]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.data();

    VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]));

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    presentInfo.pWaitSemaphores = signalSemaphores.data();
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapChain_;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        window_->framebufferResized()) {
        window_->resetFramebufferResized();
        recreateSwapChain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: vkQueuePresentKHR завершился ошибкой");
    }
}

void VulkanBase::createWorldMap(const void* pixels, uint32_t width, uint32_t height,
                                size_t pixelBytes) {
    worldMapTexture_.init(device_, physicalDevice_, commandBuffers_.commandPool(),
                          graphicsQueue_, width, height, pixels, pixelBytes);
    worldMapPass_.setTexture(&worldMapTexture_);
    core::Logger::info("Vulkan: текстура карты мира загружена (" +
                       std::to_string(width) + "x" + std::to_string(height) + ")");
}

void VulkanBase::setHudText(std::string_view text, float glyphScale) {
    hudText_ = std::string(text);
    hudGlyphScale_ = glyphScale;
}

void VulkanBase::drawWorldMap(const MapUniformObject& uniform) {
    if (!worldMapPass_.ready()) {
        return;
    }
    const FrameAcquire acquired = acquireFrame();
    if (acquired.skip) {
        return;
    }
    const uint32_t imageIndex = acquired.imageIndex;

    VkCommandBuffer commandBuffer = commandBuffers_.commandBuffer(currentFrame_);
    vkResetCommandBuffer(commandBuffer, 0);
    // Аспект берём из swapchain здесь, а не в main: окно может измениться
    // между кадрами, и единственное достоверное место знать текущий extent —
    // сам renderer.
    MapUniformObject withAspect = uniform;
    const float aspect = swapChainExtent_.height > 0
                             ? static_cast<float>(swapChainExtent_.width) /
                                   static_cast<float>(swapChainExtent_.height)
                             : 1.0f;
    withAspect.params.x = aspect;
    worldMapPass_.update(currentFrame_, withAspect);

    // Список объектов пустой: карта рисуется одним полноэкранным треугольником
    // в шейдере, 3D-сцена в этом кадре не выводится. Цвет очистки — тёмно-синий,
    // чтобы полосы за пределами карты (их не бывает при CLAMP_TO_EDGE) не мигали.
    const std::span<const MeshDraw> noDraws{};
    commandBuffers_.record(commandBuffer, renderPass_, imageIndex, swapChainExtent_,
                           worldMapPass_.pipeline(), noDraws, kMapClearColor,
                           worldMapPass_.descriptorSet(currentFrame_), VK_NULL_HANDLE,
                           /*fullscreen=*/true);

    lastDrawCalls_ = worldMapPass_.pipeline().lastDrawCalls();
    submitAndPresent(commandBuffer, imageIndex);
    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

void VulkanBase::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "AntiWorld";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "AntiWorld";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    // Расширения, необходимые GLFW для создания поверхности.
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char*> extensions(glfwExtensions,
                                        glfwExtensions + glfwExtensionCount);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // Для debug-сборки включаем validation layers и debug-расширение.
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (kEnableValidation) {
        checkValidationLayerSupport();
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();

        // Тот же messenger подключается и к самому vkCreateInstance,
        // чтобы ловить ошибки ещё на этапе создания инстанса.
        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = &debugCreateInfo;
    }

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));
    core::Logger::info("Vulkan: instance создан");
}

void VulkanBase::createDebugMessenger() {
    // Функции расширения VK_EXT_debug_utils получаем через proc-адреса,
    // чтобы не полагаться на их экспорт из загрузчика.
    const auto createDebugUtilsMessengerEXT =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    const auto destroyDebugUtilsMessengerEXT =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));

    if (createDebugUtilsMessengerEXT == nullptr || destroyDebugUtilsMessengerEXT == nullptr) {
        throw std::runtime_error(
            "Vulkan: расширение VK_EXT_debug_utils недоступно (нет proc-адресов)");
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);
    VK_CHECK(createDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_));
    core::Logger::info("Vulkan: debug messenger создан");
}

void VulkanBase::createSurface() {
    // GLFW создаёт поверхность под текущую платформу (Wayland/X11).
    VK_CHECK(glfwCreateWindowSurface(instance_, window_->handle(), nullptr, &surface_));
    core::Logger::info("Vulkan: поверхность окна создана");
}

void VulkanBase::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("Vulkan: не найдено устройств с поддержкой Vulkan");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    // Ищем подходящее устройство с максимальной оценкой.
    int bestScore = -1;
    for (VkPhysicalDevice device : devices) {
        const QueueFamilyIndices indices = findQueueFamilies(device);
        const SwapChainSupportDetails details = querySwapChainSupport(device);

        if (!indices.isComplete() || !checkDeviceExtensionSupport(device) ||
            details.formats.empty() || details.presentModes.empty()) {
            continue;  // устройство не удовлетворяет нашим требованиям
        }

        const int score = rateDeviceSuitability(device);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = device;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Vulkan: нет подходящего физического устройства");
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    core::Logger::info(std::string("Vulkan: выбрано устройство \"") +
                       properties.deviceName + "\"");
}

void VulkanBase::createLogicalDevice() {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    // Одна очередь (или две, если семейства разные) с приоритетом 1.0.
    const std::set<uint32_t> uniqueQueueFamilies = {*indices.graphics, *indices.present};
    const float queuePriority = 1.0f;

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueQueueFamilies.size());
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();

    // Валидация на уровне устройства (безвредна и для старых драйверов).
    if (kEnableValidation) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
    }

    VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));
    vkGetDeviceQueue(device_, *indices.graphics, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, *indices.present, 0, &presentQueue_);
    core::Logger::info("Vulkan: логическое устройство создано");
}

void VulkanBase::createSwapChain() {
    const SwapChainSupportDetails details = querySwapChainSupport(physicalDevice_);

    const VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(details.formats);
    const VkPresentModeKHR presentMode = chooseSwapPresentMode(details.presentModes);
    const VkExtent2D extent = chooseSwapExtent(details.capabilities);

    uint32_t imageCount = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 &&
        imageCount > details.capabilities.maxImageCount) {
        imageCount = details.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    // Очистка через render pass, без сэмплов — обычное изображение.
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    const std::array<uint32_t, 2> queueFamilyIndices = {*indices.graphics,
                                                        *indices.present};
    if (*indices.graphics != *indices.present) {
        // Очереди разные — изображением владеют несколько семейств.
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;
    }

    createInfo.preTransform = details.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;  // при пересоздании будет старый

    VK_CHECK(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_));

    // Получаем изображения созданного swapchain.
    uint32_t swapChainImageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapChain_, &swapChainImageCount, nullptr);
    swapChainImages_.resize(swapChainImageCount);
    vkGetSwapchainImagesKHR(device_, swapChain_, &swapChainImageCount,
                            swapChainImages_.data());

    swapChainImageFormat_ = surfaceFormat.format;
    swapChainExtent_ = extent;
    core::Logger::info("Vulkan: swapchain создан (" + std::to_string(swapChainImageCount) +
                       " изображений " + std::to_string(extent.width) + "x" +
                       std::to_string(extent.height) + ")");
}

void VulkanBase::createImageViews() {
    swapChainImageViews_.resize(swapChainImages_.size());
    for (size_t i = 0; i < swapChainImages_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapChainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapChainImageFormat_;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(device_, &createInfo, nullptr,
                                   &swapChainImageViews_[i]));
    }

    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.format = VK_FORMAT_D32_SFLOAT;
    depthImageInfo.extent = {swapChainExtent_.width, swapChainExtent_.height, 1};
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &depthImageInfo, nullptr, &depthImage_));

    VkMemoryRequirements depthRequirements{};
    vkGetImageMemoryRequirements(device_, depthImage_, &depthRequirements);
    VkMemoryAllocateInfo depthAllocationInfo{};
    depthAllocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAllocationInfo.allocationSize = depthRequirements.size;
    depthAllocationInfo.memoryTypeIndex =
        findMemoryType(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &depthAllocationInfo, nullptr, &depthImageMemory_));
    VK_CHECK(vkBindImageMemory(device_, depthImage_, depthImageMemory_, 0));

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = depthImage_;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &depthViewInfo, nullptr, &depthImageView_));

    core::Logger::info("Vulkan: созданы view изображений swapchain и depth image");
}

void VulkanBase::createSyncObjects() {
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // первый vkWaitForFences не блокирует

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]));
        VK_CHECK(
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]));
        VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]));
    }
    core::Logger::info("Vulkan: созданы объекты синхронизации");
}

// Пересоздание примитивов синхронизации. Вызывается после vkDeviceWaitIdle,
// когда предыдущие семафоры/fence гарантированно не используются GPU.
void VulkanBase::recreateSyncPrimitives() {
    for (size_t i = 0; i < inFlightFences_.size(); ++i) {
        if (inFlightFences_[i] != VK_NULL_HANDLE) {
            vkDestroyFence(device_, inFlightFences_[i], nullptr);
        }
        if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        }
        if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        }
    }
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    createSyncObjects();
}

QueueFamilyIndices VulkanBase::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                             queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = i;  // подходящее семейство для графики
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport == VK_TRUE) {
            indices.present = i;  // семейство для презентации на поверхность
        }

        if (indices.isComplete()) {
            break;
        }
    }
    return indices;
}

SwapChainSupportDetails VulkanBase::querySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount,
                                             details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                                  details.presentModes.data());
    }
    return details;
}

uint32_t VulkanBase::findMemoryType(uint32_t typeFilter,
                                    VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Vulkan: не найден подходящий тип памяти");
}

VkSurfaceFormatKHR VulkanBase::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    // Предпочтительный формат — sRGB с 8 битами на канал.
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];  // запасной вариант — первый доступный
}

VkPresentModeKHR VulkanBase::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& presentModes) {
    // Mailbox — низкая задержка и без вертикальной синхронизации (triple buffering).
    for (VkPresentModeKHR mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;  // FIFO есть везде, это надёжный fallback
}

VkExtent2D VulkanBase::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;  // размер уже задан поверхностью
    }

    // Иначе берём размер буфера кадра окна и ограничиваем допустимым диапазоном.
    VkExtent2D actualExtent{static_cast<uint32_t>(window_->framebufferWidth()),
                            static_cast<uint32_t>(window_->framebufferHeight())};
    actualExtent.width =
        std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                   capabilities.maxImageExtent.width);
    actualExtent.height =
        std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                   capabilities.maxImageExtent.height);
    return actualExtent;
}

void VulkanBase::cleanupSwapChain() {
    if (depthImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthImageView_, nullptr);
        depthImageView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthImageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depthImageMemory_, nullptr);
        depthImageMemory_ = VK_NULL_HANDLE;
    }

    for (VkImageView imageView : swapChainImageViews_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    swapChainImageViews_.clear();

    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
}

void VulkanBase::recreateSwapChain() {
    // При сворачивании окна буфер кадра 0x0 — ждём восстановления размера.
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_->handle(), &width, &height);
    while (width == 0 || height == 0) {
        // Окно может быть закрыто пользователем, пока оно свёрнуто: без
        // проверки shouldClose() здесь игра намертво зависала в glfwWaitEvents.
        if (window_->shouldClose()) {
            return;
        }
        glfwWaitEvents();
        glfwGetFramebufferSize(window_->handle(), &width, &height);
    }

    vkDeviceWaitIdle(device_);  // завершаем все текущие операции

    // Пайплайн/render pass/framebuffer'ы зависят от swapchain — пересоздаём.
    destroyRenderTargets();
    cleanupSwapChain();
    createSwapChain();
    createImageViews();
    createRenderTargets();
    core::Logger::info("Vulkan: swapchain пересоздан при изменении размера окна");
}

void VulkanBase::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    // Освобождение всего в обратном порядке создания.
    for (size_t i = 0; i < inFlightFences_.size(); ++i) {
        vkDestroyFence(device_, inFlightFences_[i], nullptr);
        vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
    }
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();

    // Модульные объекты рендера освобождаются в порядке, обратном созданию.
    commandBuffers_.destroy();
    for (Buffer& buffer : instanceBuffers_) {
        buffer.destroy();
    }
    // Буферы mesh должны умереть до уничтожения VkDevice, а entity, ссылающиеся
    // на mesh, — до этого (см. world::Terrain::~Terrain).
    ownedMeshes_.clear();
    mesh_.destroy();
    destroyRenderTargets();

    cleanupSwapChain();

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (debugMessenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        const auto destroyDebugUtilsMessengerEXT =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyDebugUtilsMessengerEXT != nullptr) {
            destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    core::Logger::info("Vulkan: ресурсы освобождены");
}

}  // namespace renderer