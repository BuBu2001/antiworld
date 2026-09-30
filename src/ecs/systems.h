#pragma once

#include <vector>

#include <entt/entt.hpp>

#include "renderer/frustum_culling.h"
#include "renderer/mesh.h"

namespace ecs {

class MovementSystem {
public:
    void update(entt::registry& registry, float deltaTime);
};

class RenderSystem {
public:
    // Итог отсечения за кадр — для логов. submitted считает всё, что реально
    // ушло в drawFrame, culled — то, что отсеяно по пирамиде видимости.
    struct CullStats {
        std::size_t submitted{0};
        std::size_t culled{0};
        std::size_t tested{0};  // сколько сущностей вообще имели Bounds
    };

    // Собирает список отрисовки. Сущности с компонентом Bounds отсекаются по
    // frustum; без него проходят всегда. frustum уже построен вызывающим в том
    // же локальном фрейме, что и Transform, — пересчитывать здесь нечего.
    void collect(entt::registry& registry, const renderer::Frustum& frustum,
                 std::vector<renderer::DrawData>& output, CullStats* stats = nullptr) const;
};

}
