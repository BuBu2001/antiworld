#pragma once

#include <cstddef>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace physics {

class PhysicsWorld {
public:
    using BodyHandle = JPH::BodyID;

    explicit PhysicsWorld(
        std::size_t maxBodies = 4096,
        std::size_t maxBodyPairs = 16384,
        std::size_t maxContactConstraints = 8192
    );
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    BodyHandle createStaticBox(
        const glm::vec3& halfExtents,
        const glm::vec3& position,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
    );

    BodyHandle createStaticPlane(
        const glm::vec3& position,
        float halfExtent = 30.0f
    );

    BodyHandle createDynamicBox(
        const glm::vec3& halfExtents,
        const glm::vec3& position,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
    );

    // Капсула-игрок: halfHeight — половина ВЫСОТЫ ЦИЛИНДРА (без двух
    // полусферий), radius — радиус полусфер. Полная высота =
    // 2 * (halfHeight + radius). Форма выбрана capsule, а не box/sphere,
    // потому что она не «застревает» на стыках треугольников рельефа.
    BodyHandle createDynamicCapsule(
        float halfHeight,
        float radius,
        const glm::vec3& position,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
    );

    // Луч вниз для проверки опоры под персонажем. Возвращает true, если луч
    // что-то задел; в outDistance — расстояние до точки попадания, в
    // outNormal — нормаль поверхности в этой точке (нужна, чтобы отличать
    // «стоим на земле» от «висим на стене»).
    bool raycastDown(
        const glm::vec3& from,
        const glm::vec3& to,
        float& outDistance,
        glm::vec3& outNormal
    ) const;

    // Статическое тело с произвольной формой Jolt: высотное поле
    // (TerrainGenerator::createPhysicsBody), mesh и тому подобное. Форму
    // достаточно передать по ссылке — Jolt держит собственную ссылку на неё
    // в теле, поэтому вызывающий не обязан управлять её временем жизни.
    BodyHandle createStaticShape(
        const JPH::Shape& shape,
        const glm::vec3& position,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
    );

    void removeBody(BodyHandle handle);
    bool isBodyValid(BodyHandle handle) const noexcept;

    void setBodyTransform(
        BodyHandle handle,
        const glm::vec3& position,
        const glm::quat& rotation
    );

    glm::vec3 bodyPosition(BodyHandle handle) const;
    glm::quat bodyRotation(BodyHandle handle) const;
    glm::vec3 bodyLinearVelocity(BodyHandle handle) const;

    void setBodyLinearVelocity(
        BodyHandle handle,
        const glm::vec3& velocity
    );

    void step(float deltaTime);

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    // Тело игрока: луч проверки опоры не должен цеплять его снизу, иначе
    // «стою на земле» было бы истинно всегда. Ставится в createDynamicCapsule.
    BodyHandle playerBody_{};
};

}
