#include "physics/player_controller.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <entt/entity/registry.hpp>

#include <GLFW/glfw3.h>

#include "core/input.h"
#include "ecs/components.h"
#include "physics/physics_world.h"

namespace physics {

void PlayerController::update(
    entt::registry& registry,
    PhysicsWorld& world,
    float deltaTime,
    float yaw
) {
    auto view = registry.view<ecs::RigidBody, ecs::Player>();
    for (const entt::entity entity : view) {
        const auto& rigidBody = registry.get<ecs::RigidBody>(entity);
        auto& player = registry.get<ecs::Player>(entity);
        if (!world.isBodyValid(rigidBody.handle)) {
            continue;
        }

        const glm::vec3 position = world.bodyPosition(rigidBody.handle);
        const glm::vec3 velocity = world.bodyLinearVelocity(rigidBody.handle);

        // --- Проверка опоры: луч вниз из центра капсулы. ---
        // Луч идёт от центра минус «низ капсулы» минус небольшой запас: так мы
        // ловим момент за долю секунды до фактического касания, иначе на
        // скользком рельефе персонаж «отлипал» на кадр и подпрыгивал.
        const float halfHeight = 0.5f * player.height;
        const float probeFrom = position.y - halfHeight + player.groundProbe;
        const float probeTo = position.y - halfHeight - player.groundProbe;
        float hitDistance = 0.0f;
        glm::vec3 hitNormal{0.0f, 1.0f, 0.0f};
        const bool hit = world.raycastDown(
            glm::vec3(position.x, probeFrom, position.z),
            glm::vec3(position.x, probeTo, position.z),
            hitDistance,
            hitNormal
        );
        // Опора есть только если луч попал И поверхность достаточно пологая:
        // иначе персонаж «стоял» бы на отвесной стене.
        const bool grounded =
            hit && (hitNormal.y >= player.slopeLimitCos) &&
            (hitDistance <= 2.0f * player.groundProbe + 0.05f);
        player.grounded = grounded;
        state_.grounded = grounded;

        // --- Желаемое направление в плоскости, относительно взгляда. ---
        glm::vec3 wish{0.0f};
        if (core::Input::isKeyPressed(GLFW_KEY_W)) wish.z += 1.0f;
        if (core::Input::isKeyPressed(GLFW_KEY_S)) wish.z -= 1.0f;
        if (core::Input::isKeyPressed(GLFW_KEY_D)) wish.x += 1.0f;
        if (core::Input::isKeyPressed(GLFW_KEY_A)) wish.x -= 1.0f;
        if (wish != glm::vec3(0.0f)) {
            // Поворот базиса взгляда вокруг Y: forward = (cos yaw, 0, sin yaw).
            const float c = std::cos(yaw);
            const float s = std::sin(yaw);
            const glm::vec3 world3{
                wish.x * c + wish.z * s,
                0.0f,
                -wish.x * s + wish.z * c
            };
            wish = glm::normalize(world3);
        }

        const bool running = core::Input::isKeyPressed(GLFW_KEY_LEFT_SHIFT) ||
                             core::Input::isKeyPressed(GLFW_KEY_RIGHT_SHIFT);
        player.running = running;
        state_.running = running;
        const float targetSpeed = (running ? player.runSpeed : player.walkSpeed) *
                                  (wish == glm::vec3(0.0f) ? 0.0f : 1.0f);

        // --- Горизонталь: разгон к целевой скорости. ---
        // Мгновенная установка скорости (без ускорения) заставляла бы
        // персонажа «скользить» и мгновенно останавливаться, что выглядит
        // как полёт над землёй. Разгон/торможение ограничены ускорением.
        constexpr float kAcceleration = 40.0f;   // м/с^2
        constexpr float kBrake = 55.0f;          // м/с^2, торможение резче
        const glm::vec3 horizontal{velocity.x, 0.0f, velocity.z};
        const glm::vec3 target{wish.x * targetSpeed, 0.0f, wish.z * targetSpeed};
        const float rate =
            (targetSpeed > 0.0f ? kAcceleration : kBrake) * deltaTime;
        const glm::vec3 delta = target - horizontal;
        const float step = std::min(rate, glm::length(delta));
        const glm::vec3 newHorizontal =
            horizontal + (glm::length(delta) > 1.0e-6f
                              ? delta * (step / glm::length(delta))
                              : glm::vec3(0.0f));

        // --- Прыжок: только с опоры, и только вверх. ---
        float newVertical = velocity.y;
        if (grounded && core::Input::isKeyPressed(GLFW_KEY_SPACE)) {
            newVertical = player.jumpSpeed;
            // Прыжок засчитываем сразу: до следующего шага луч ещё достаёт
            // землю, и персонаж «уже стоял» — иначе кадр задержки на ровном
            // месте ощущается как залипание.
            state_.grounded = false;
            player.grounded = false;
        }

        world.setBodyLinearVelocity(
            rigidBody.handle,
            glm::vec3(newHorizontal.x, newVertical, newHorizontal.z)
        );
        state_.horizontalSpeed = glm::length(glm::vec2(newHorizontal.x, newHorizontal.z));
    }
}

}
