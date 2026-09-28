#pragma once

#include <entt/entity/fwd.hpp>

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

    // yaw — угол обзора камеры вокруг вертикали: движение задаётся
    // относительно взгляда (W = туда, куда смотрим), а не относительно осей
    // мира. eyeOffset — на сколько поднять камеру над центром капсулы.
    void update(
        entt::registry& registry,
        PhysicsWorld& world,
        float deltaTime,
        float yaw
    );

    const State& state() const noexcept { return state_; }

private:
    State state_{};
};

}
