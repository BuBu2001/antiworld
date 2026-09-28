#pragma once

#include <entt/entity/fwd.hpp>

#include <glm/glm.hpp>

namespace ecs {
struct Player;
struct RigidBody;
struct Transform;
}

namespace physics {

class PhysicsWorld;

// Управление персонажем на физическом теле: ходьба, бег, прыжок, проверка
// опоры лучом вниз. Кадр: вызывается ДО PhysicsSystem::step(), чтобы
// заданная скорость применилась на этом же шаге симуляции.
//
// Гравитация принадлежит Jolt: контроллер НИКОГДА не пишет velocity.y —
// только удерживает/задаёт горизонталь и отдельно, при отрыве, стартовую
// скорость прыжка. Иначе гравитация не успевала бы накапливаться.
class PlayerController {
public:
    // Состояние, о котором интересно знать рендеру и HUD.
    struct State {
        bool grounded{false};
        bool running{false};
        float horizontalSpeed{0.0f};
    };

    // forward/right — ГОРИЗОНТАЛЬНЫЕ направления взгляда камеры (camera.front()
    // и camera.right(), спроецированные на плоскость XZ). Раньше контроллер
    // принимал yaw и сам собирал базис, но перепутал компоненты X и Z: W шёл
    // в (sin yaw, 0, cos yaw) вместо (cos yaw, 0, sin yaw), то есть на любой
    // сторону света. Теперь базис берётся у камеры напрямую — соглашение о
    // соглашении yaw и forward уже не может разойтись.
    //
    // относительно взгляда (W = туда, куда смотрим), а не относительно осей
    // мира. eyeOffset — на сколько поднять камеру над центром капсулы.
    void update(
        entt::registry& registry,
        PhysicsWorld& world,
        float deltaTime,
        const glm::vec3& forward,
        const glm::vec3& right
    );

    const State& state() const noexcept { return state_; }

private:
    State state_{};
};

}
