#include "ecs/world.h"

#include "core/camera.h"
#include "renderer/vulkan_base.h"

namespace ecs {

World::World(core::Camera& camera, renderer::VulkanBase& renderer)
    : camera_(camera), renderer_(renderer) {}

entt::entity World::createEntity() {
    return registry_.create();
}

void World::update(float deltaTime) {
    movementSystem_.update(registry_, deltaTime);
}

void World::setEnvironment(const renderer::FrameEnvironment& environment) {
    environment_ = environment;
}

void World::render() {
    renderSystem_.collect(registry_, renderData_);
    const glm::mat4 viewProjection = camera_.projection() * camera_.view();
    renderer_.drawFrame(viewProjection, renderData_, environment_);
}

}
