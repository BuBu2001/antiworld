#pragma once

#include <entt/entt.hpp>

#include "physics/physics_world.h"

namespace physics {

class PhysicsSystem {
public:
    explicit PhysicsSystem(PhysicsWorld& world) : world_(world) {}

    void update(entt::registry& registry, float deltaTime);
    void step(entt::registry& registry, float deltaTime) {
        update(registry, deltaTime);
    }

private:
    void syncTransforms(entt::registry& registry);
    void syncPhysics(entt::registry& registry);

    PhysicsWorld& world_;
};

}
