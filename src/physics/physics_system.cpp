#include "physics/physics_system.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "ecs/components.h"

namespace physics {
namespace {

// Порядок преобразований должен быть строго обратимым: раньше прямое
// преобразование использовало композицию X*Y*Z (эквивалент ZYX по GLM-терминам),
// а обратное читало углы как из матрицы Tait-Bryan YZX — из-за рассогласования
// ориентация вращающихся тел дрейфовала каждый кадр (Euler->Quat->Euler).
// Теперь оба направления используют один и тот же порядок: R = Rz * Ry * Rx
// («сначала тангаж, затем рыскание, затем крен»), что соответствует
// glm::eulerAngles() (XYZ-разложение) для обратного преобразования.
glm::quat rotationToQuaternion(const glm::vec3& rotation) {
    glm::quat result = glm::angleAxis(rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    result = glm::angleAxis(rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)) * result;
    result = glm::angleAxis(rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) * result;
    return result;
}

glm::vec3 quaternionToRotation(const glm::quat& rotation) {
    // glm::eulerAngles выполняет XYZ-разложение, точно обратное композиции
    // Rz*Ry*Rx выше, поэтому round-trip Euler->Quat->Euler стабилен.
    return glm::eulerAngles(rotation);
}

bool isClose(const glm::vec3& first, const glm::vec3& second) {
    return glm::dot(first - second, first - second) <= 1.0e-10f;
}

bool isClose(const glm::quat& first, const glm::quat& second) {
    return std::abs(glm::dot(first, second)) >= 1.0f - 1.0e-5f;
}

}

void PhysicsSystem::update(entt::registry& registry, float deltaTime) {
    syncTransforms(registry);
    world_.step(deltaTime);
    syncPhysics(registry);
}

void PhysicsSystem::syncTransforms(entt::registry& registry) {
    auto view = registry.view<ecs::Transform, ecs::RigidBody>();
    for (const entt::entity entity : view) {
        const auto& transform = registry.get<ecs::Transform>(entity);
        const auto& rigidBody = registry.get<ecs::RigidBody>(entity);
        if (!world_.isBodyValid(rigidBody.handle)) {
            continue;
        }

        const glm::quat rotation = rotationToQuaternion(transform.rotation);
        if (!isClose(world_.bodyPosition(rigidBody.handle), transform.position) ||
            !isClose(world_.bodyRotation(rigidBody.handle), rotation)) {
            world_.setBodyTransform(rigidBody.handle, transform.position, rotation);
        }

        if (auto* velocity = registry.try_get<ecs::Velocity>(entity)) {
            if (!isClose(world_.bodyLinearVelocity(rigidBody.handle), velocity->value)) {
                world_.setBodyLinearVelocity(rigidBody.handle, velocity->value);
            }
        }
    }
}

void PhysicsSystem::syncPhysics(entt::registry& registry) {
    auto view = registry.view<ecs::Transform, ecs::RigidBody>();
    for (const entt::entity entity : view) {
        auto& transform = registry.get<ecs::Transform>(entity);
        const auto& rigidBody = registry.get<ecs::RigidBody>(entity);
        if (!world_.isBodyValid(rigidBody.handle)) {
            continue;
        }

        transform.position = world_.bodyPosition(rigidBody.handle);
        transform.rotation = quaternionToRotation(world_.bodyRotation(rigidBody.handle));
        if (auto* velocity = registry.try_get<ecs::Velocity>(entity)) {
            velocity->value = world_.bodyLinearVelocity(rigidBody.handle);
        }
    }
}

}
