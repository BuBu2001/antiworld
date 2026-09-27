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

void World::render(std::span<const renderer::DrawData> extraDraws) {
    renderSystem_.collect(registry_, renderData_);
    // Инстансные объекты (деревья и т.п.) идут в том же списке: drawFrame()
    // сгруппирует DrawData по mesh и сделает один вызов на mesh с
    // instanceCount = N, поэтому тысячи деревьев стоят ровно один draw call.
    // renderData_ не перевыделяем между кадрами — он и так переиспользуется.
    renderData_.insert(renderData_.end(), extraDraws.begin(), extraDraws.end());

    const glm::mat4 viewProjection = camera_.projection() * camera_.view();
    renderer_.drawFrame(viewProjection, renderData_, environment_);
}

}
