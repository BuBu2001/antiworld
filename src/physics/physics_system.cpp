#include "physics/physics_system.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "ecs/components.h"

namespace physics {
namespace {

glm::quat rotationToQuaternion(const glm::vec3& rotation) {
    glm::quat result = glm::angleAxis(rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    result *= glm::angleAxis(rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    result *= glm::angleAxis(rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    return result;
}

glm::vec3 quaternionToRotation(const glm::quat& rotation) {
    const glm::mat3 matrix = glm::mat3_cast(rotation);
    const float sinY = std::clamp(matrix[2][0], -1.0f, 1.0f);
    const float y = std::asin(sinY);
    if (std::abs(sinY) < 0.999999f) {
        return {
            std::atan2(-matrix[2][1], matrix[2][2]),
            y,
            std::atan2(-matrix[1][0], matrix[0][0])
        };
    }
    return {0.0f, y, std::atan2(matrix[0][1], matrix[1][1])};
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
