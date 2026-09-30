#pragma once

#include <span>
#include <vector>

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

    // Рисует кадр: ECS-объекты (чанки/агенты) + внешние инстансные draw call'ы.
    //
    // extraDraws нужен для InstancedRenderer (деревья, камни, трава): там тысячи
    // одинаковых объектов, у которых нет и не может быть по ECS-сущности на
    // каждый — одна сущность означала бы один draw call. instanced-объекты
    // приходят готовыми span'ами (по одному на mesh) и просто дописываются к
    // общему списку; VulkanBase::drawFrame() сам объединит все DrawData одного
    // mesh в один vkCmdDrawIndexed с instanceCount=N.
    //
    // Эти DrawData живут только внутри InstancedRenderer (scratch-буфер), так
    // что копировать их в renderData_ смысла нет — render() лишь снимает
    // указатели на время вызова drawFrame().
    void render(const renderer::Frustum& frustum,
                std::span<const renderer::DrawData> extraDraws = {});

    // Отсечение чанков за последний кадр. submitted — сколько чанков ушло в
    // GPU, culled — сколько отсеяно пирамидой видимости. Нужно для лога:
    // без него не видно, работает ли отсечение вообще.
    const ecs::RenderSystem::CullStats& chunkCullStats() const noexcept {
        return chunkCull_;
    }
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
    RenderSystem::CullStats chunkCull_{};
};

}
