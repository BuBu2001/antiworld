#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include "renderer/frustum_culling.h"

namespace renderer {
class Mesh;
}

namespace ecs {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 toMatrix() const {
        glm::mat4 model = glm::translate(glm::mat4{1.0f}, position);
        model = glm::rotate(model, rotation.x, glm::vec3{1.0f, 0.0f, 0.0f});
        model = glm::rotate(model, rotation.y, glm::vec3{0.0f, 1.0f, 0.0f});
        model = glm::rotate(model, rotation.z, glm::vec3{0.0f, 0.0f, 1.0f});
        return glm::scale(model, scale);
    }
};

struct MeshRenderer {
    const renderer::Mesh* handle{nullptr};
};

// Габариты сущности для отсечения по пирамиде видимости.
//
// Только для крупных статических объектов — прежде всего чанков рельефа.
// Смысл компонента: чанк, ушедший за камеру или вбок, не должен попадать в
// командный буфер. Без него RenderSystem отправляет GPU все загруженные
// чанки целиком, и на 142 чанках это ~1.16 млн треугольников рельефа в
// кадре, из которых видна меньше половины.
//
// Координаты локальные (относительно floating origin) и в double — тот же
// фрейм, в котором main.cpp строит Frustum для растительности. Глобальные
// координаты на 10 км в float дают шаг ~1 мм, и на границе экрана чанк
// мигал бы visible/hidden.
//
// ОТСУТСТВИЕ компонента означает «не отсекать»: так ведут себя мелкие
// сущности (агенты), которые дешевле отправить, чем проверить, и у которых
// габаритов всё равно нет.
struct Bounds {
    renderer::AABB local{};
};

struct Velocity {
    glm::vec3 value{0.0f};
};

struct RigidBody {
    JPH::BodyID handle;
};

struct Agent {};

// Игрок. Скорости и радиусы проверки опоры хранятся в компоненте, чтобы
// контроллер не тащил настройки в глобальные переменные.
struct Player {
    float walkSpeed{3.5f};      // м/с, обычный шаг
    float runSpeed{8.0f};       // м/с, с зажатым Shift
    float jumpSpeed{6.0f};      // м/с, начальная вертикальная
    float radius{0.35f};        // радиус капсулы
    float height{1.8f};         // полная высота капсулы
    float groundProbe{0.25f};   // на сколько ниже низа капсулы бьём луч
    float slopeLimitCos{0.7f};  // косинус максимального наклона для опоры
    bool grounded{false};
    bool running{false};
};

}
