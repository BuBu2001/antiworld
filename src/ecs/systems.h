#pragma once

#include <vector>

#include <entt/entt.hpp>

#include "renderer/mesh.h"

namespace ecs {

class MovementSystem {
public:
    void update(entt::registry& registry, float deltaTime);
};

class RenderSystem {
public:
    void collect(entt::registry& registry,
                 std::vector<renderer::DrawData>& output) const;
};

}
