#include "ecs/systems.h"

#include "ecs/components.h"

namespace ecs {

void MovementSystem::update(entt::registry& registry, float deltaTime) {
    auto view = registry.view<Transform, Velocity>();
    for (const entt::entity entity : view) {
        if (registry.all_of<RigidBody>(entity)) {
            continue;
        }
        auto& transform = registry.get<Transform>(entity);
        const auto& velocity = registry.get<Velocity>(entity);
        transform.position += velocity.value * deltaTime;
    }
}

void RenderSystem::collect(entt::registry& registry, const renderer::Frustum& frustum,
                           std::vector<renderer::DrawData>& output, CullStats* stats) const {
    output.clear();
    CullStats local;
    CullStats& counters = stats != nullptr ? *stats : local;

    auto view = registry.view<const Transform, const MeshRenderer>();
    for (const entt::entity entity : view) {
        const auto& transform = registry.get<const Transform>(entity);
        const auto& meshRenderer = registry.get<const MeshRenderer>(entity);
        if (meshRenderer.handle == nullptr) {
            continue;
        }
        // Отсечение по пирамиде видимости. Без Bounds сущность проходит
        // всегда: чанки ради этого габариты и держат, а агентов (десятки
        // штук) дешевле отправить, чем проверять.
        if (const auto* bounds = registry.try_get<const Bounds>(entity);
            bounds != nullptr) {
            ++counters.tested;
            if (!frustum.intersectsAABB(bounds->local)) {
                ++counters.culled;
                continue;
            }
        }
        output.push_back({meshRenderer.handle, transform.toMatrix()});
        ++counters.submitted;
    }
}

}
