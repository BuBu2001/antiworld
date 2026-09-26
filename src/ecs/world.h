#pragma once

#include <vector>

#include <entt/entt.hpp>

#include "ecs/systems.h"
#include "renderer/mesh.h"
#include "renderer/uniform_buffer.h"

namespace core {
class Camera;
}

namespace renderer {
class VulkanBase;
}

namespace ecs {

class World {
public:
    World(core::Camera& camera, renderer::VulkanBase& renderer);

    entt::registry& registry() noexcept { return registry_; }
    const entt::registry& registry() const noexcept { return registry_; }

    entt::entity createEntity();
    void update(float deltaTime);
    void render();

    // Свет и климат кадра для шейдера. Заполняется из world::Climate каждый
    // кадр (см. world::Terrain::environment) и просто перекладывается в
    // renderer::drawFrame, поэтому renderer не зависит от климата.
    void setEnvironment(const renderer::FrameEnvironment& environment);

private:
    entt::registry registry_;
    MovementSystem movementSystem_;
    RenderSystem renderSystem_;
    core::Camera& camera_;
    renderer::VulkanBase& renderer_;
    renderer::FrameEnvironment environment_;
    std::vector<renderer::DrawData> renderData_;
};

}
