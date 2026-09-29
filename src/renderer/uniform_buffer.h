#pragma once

// Vulkan-дескрипторы и uniform-буферы для UBO (камера, MVP).
// Управляет тремя группами ресурсов:
//   1) descriptor set layout (binding 0 — VkUniformBuffer, vertex stage) —
//      используется при создании пайплайна (GraphicsPipeline);
//   2) descriptor pool + по одному descriptor set'у на кадр в полёте;
//   3) host-visible uniform-буферы (по одному на кадр) для фактических данных.
//
// UniformBufferObject — данные кадра (MVP-матрицы), совпадающие с UBO
// в triangle.vert. Каждый кадр VulkanBase::drawFrame() обновляет UBO из
// core::Camera (камера считает view/projection, модель — единичная) через
// UniformBuffer<UniformBufferObject>::update(frameIndex, uvbo).
//
// Header-only (inline), как и core::Camera/Input. Владение Vulkan-ресурсами —
// копирование/перемещение запрещены.

#include <GLFW/glfw3.h>

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>


namespace renderer {

// Содержимое UBO (выравнивание std140 — glm::mat4 по 16 байт, кратно 16).
// Тот же layout, что и в triangle.vert (layout(binding = 0) uniform UBO).
//
// Данные кадра делятся на две части:
//   * viewProjection — матрица камеры, нужна только вершинному шейдеру;
//   * sunDirection/environment — освещение и климат, нужны обоим шейдерам:
//     солнце считается в фрагментном, а environment.x (frost) управляет
//     снегом на альбедо вершины.
//
// Поля упакованы в vec4 не ради экономии, а ради выравнивания: std140
// выравнивает каждый скалярный член до 16 байт, и четыре float в vec4 занимают
// ровно столько же места, сколько четыре отдельных float, но layout совпадает
// с GLSL гарантированно.
struct UniformBufferObject {
    glm::mat4 viewProjection{1.0f};
    // xyz — единичный вектор НА солнце, w — интенсивность солнца (0 ночью).
    glm::vec4 sunDirection{0.0f, 1.0f, 0.0f, 1.0f};
    // x — frost (сезонная морозность 0..1), y — ambient, z — seaLevel
    // (уровень моря в мировых координатах; при hasWater = 0 не используется),
    // w — время суток 0..1 (зарезервировано под сумеречность).
    glm::vec4 environment{0.0f, 0.2f, 0.0f, 0.5f};
    // x — есть ли в мире океан (1/0), yzw — зарезервированы. Отдельный vec4:
    // расширение environment сломало бы совпадение layout с GLSL без правки
    // обоих шейдеров сразу, а здесь добавляется ровно один std140-блок.
    glm::vec4 waterFlags{0.0f, 0.0f, 0.0f, 0.0f};

    // Обратная viewProjection. Нужна полноэкранному проходу неба: у него нет
    // геометрии, поэтому направление взгляда приходится восстанавливать из
    // экранных координат обратным преобразованием, а не брать из вершины.
    // Единственный честный способ — отдать матрицу, обратную той, что уже
    // есть: пересчитывать проекцию на CPU нельзя (у камеры она своя).
    glm::mat4 inverseViewProjection{1.0f};
    // xyz — мировая позиция камеры (глаз), w — время суток 0..1.
    // Позиция камеры нужна небу для параллакса облаков: без неё слой
    // облаков «приклеен» к экрану и не сдвигается при ходьбе.
    glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 0.5f};
    // x — время в секундах (анимация ветра и облаков), y — сила ветра 0..1,
    // z — высота/накал осадков 0..1, w — зарезервировано.
    glm::vec4 wind{0.0f, 0.5f, 0.5f, 0.0f};
    // xyz — сдвиг floating origin в мировых координатах. Рендер работает в
    // локальных координатах относительно него, поэтому шейдер, считающий
    // что-то в мировых координатах (облака), должен прибавить этот сдвиг.
    glm::vec4 worldOrigin{0.0f, 0.0f, 0.0f, 0.0f};
};

// Свет и климат кадра: то, что меняется со временем года и суток, но не
// зависит от отдельного объекта. Заполняется из world::Climate (см.
// world::Terrain::environment) и уходит в UBO, откуда читают оба шейдера.
// Renderer при этом ничего не знает про климат — только про четыре числа,
// поэтому world::Climate может жить в модуле world, не создавая зависимости
// renderer -> world.
//
// Объявлено рядом с UniformBufferObject, а не в vulkan_base.h, чтобы
// ecs::World мог принимать его, не включая весь VulkanBase.
struct FrameEnvironment {
    // Единичный вектор НА солнце (в небо), а не на его источник света.
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
    // Яркость прямого света; 0 ночью.
    float sunIntensity{1.0f};
    // Заполняющий свет, чтобы тени не были чёрными.
    float ambient{0.2f};
    // Сезонная морозность 0..1: чем больше, тем ниже порог снега на альбедо
    // вершины (см. Vertex::snowBias и triangle.frag).
    float frost{0.0f};
    // Уровень моря в мировых координатах (Y). Всё, что ниже — вода. Приходит
    // из карты высот (world::Heightmap::seaLevel) — это факт ГЕОГРАФИИ мира,
    // а не визуальная настройка: шейдер рисует водную гладь ровно на этой
    // высоте, и она обязана совпадать с границей биомов и с физикой.
    float seaLevel{0.0f};
    // Есть ли в мире океан вовсе (выключенная география -> 0: шейдер не
    // рисует воду поверх «просто низин»).
    float hasWater{0.0f};
    // Мировая позиция камеры. Renderer сам её не знает — заполняет вызывающий
    // из core::Camera, как и солнце: так renderer остаётся независимым от
    // мира, а небу нужны и eye, и обратная view-матрица.
    glm::vec3 cameraPosition{0.0f, 0.0f, 0.0f};
    // Время суток 0..1: 0 — полночь, 0.5 — полдень. Не берётся из солнца
    // численно, потому что высота солнца зависит от широты и сезона, а
    // сумеречность нужна как отдельная величина (для цвета неба у горизонта).
    float dayTime{0.5f};
    // Сила ветра 0..1. Трава и кроны качаются с периодом, который зависит от
    // неё, поэтому ветер должен быть параметром кадра, а не константой
    // шейдера.
    float windStrength{0.45f};
    // Время в секундах с начала запуска. Шейдеры не имеют своего таймера:
    // анимация облаков и травы должна идти от одного источника, иначе они
    // разойдутся по фазе. Renderer сам его не знает — кладёт вызывающий.
    float timeSeconds{0.0f};
    // Покрытость неба облаками 0..1: 0 — ясно, 1 — сплошная туча. Влияет и
    // на сам небосвод, и на прямой свет на земле (тени от облаков), поэтому
    // это параметр кадра, а не константа шейдера.
    float cloudCover{0.42f};
    // Сдвиг floating origin в мировых координатах. Рендер работает в
    // локальных координатах относительно него, но небу нужны мировые:
    // проекция точки на слой облаков по локальным координатам уехала бы на
    // десятки километров.
    glm::vec3 worldOrigin{0.0f};
};

// Управляет UBO-дескрипторами и буферами (см. комментарий в начале файла).
class UniformBuffer {
public:
    UniformBuffer() = default;
    ~UniformBuffer();

    UniformBuffer(const UniformBuffer&) = delete;
    UniformBuffer& operator=(const UniformBuffer&) = delete;

    // Создаёт descriptor set layout (binding 0, uniform buffer, обе стадии),
    // descriptor pool и framesInFlight наборов данных; в каждом — свой UBO.
    void init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t framesInFlight);

    // Копирует данные кадра (матрицы MVP из камеры) в UBO для frameIndex.
    void update(uint32_t frameIndex, const UniformBufferObject& data);

    // Уничтожает буферы/память/descriptor pool/layout; повторный вызов безопасен.
    void destroy();

    // Хэндлы для привязки: layout — в GraphicsPipeline::init,
    // descriptor set — в CommandBuffers::record (descriptorSet(index)).
    VkDescriptorSetLayout layout() const { return descriptorSetLayout_; }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const {
        return descriptorSets_[frameIndex];
    }

private:
    // Ищет тип памяти, подходящий для host-visible буферов камеры.
    uint32_t findMemoryType(uint32_t typeFilter,
                           VkMemoryPropertyFlags properties) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;

    // По одному буферу/памяти/descriptor set'у на каждый кадр в полёте.
    std::vector<VkBuffer> buffers_;
    std::vector<VkDeviceMemory> buffersMemory_;
    std::vector<VkDescriptorSet> descriptorSets_;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
};

}  // namespace renderer
