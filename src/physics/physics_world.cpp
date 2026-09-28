#include "physics/physics_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "core/logger.h"

namespace {

constexpr JPH::ObjectLayer kStaticObjectLayer = 0;
constexpr JPH::ObjectLayer kMovingObjectLayer = 1;
constexpr JPH::uint kNumBroadPhaseLayers = 2;
constexpr JPH::BroadPhaseLayer kStaticBroadPhaseLayer{0};
constexpr JPH::BroadPhaseLayer kMovingBroadPhaseLayer{1};

JPH::Vec3 toJoltVec3(const glm::vec3& value) {
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 toJoltPosition(const glm::vec3& value) {
    return JPH::RVec3(value.x, value.y, value.z);
}

JPH::Quat toJoltRotation(const glm::quat& value) {
    const float lengthSquared = glm::dot(value, value);
    if (lengthSquared <= std::numeric_limits<float>::epsilon()) {
        return JPH::Quat::sIdentity();
    }
    return JPH::Quat(value.x, value.y, value.z, value.w) / std::sqrt(lengthSquared);
}

glm::vec3 toGlmVec3(const JPH::Vec3& value) {
    return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
}

glm::quat toGlmQuat(const JPH::Quat& value) {
    return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
}

bool isFinite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isFinite(const glm::quat& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}

class JoltRuntime {
public:
    JoltRuntime() {
        std::lock_guard<std::mutex> lock(mutex());
        if (users() == 0) {
            JPH::RegisterDefaultAllocator();
            if (JPH::Factory::sInstance == nullptr) {
                ownsFactory() = true;
                JPH::Factory::sInstance = new JPH::Factory();
            } else {
                ownsFactory() = false;
            }
            JPH::RegisterTypes();
        }
        ++users();
    }

    ~JoltRuntime() {
        std::lock_guard<std::mutex> lock(mutex());
        --users();
        if (users() == 0) {
            JPH::UnregisterTypes();
            if (ownsFactory()) {
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
            ownsFactory() = false;
        }
    }

    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }

    static std::size_t& users() {
        static std::size_t instance = 0;
        return instance;
    }

    static bool& ownsFactory() {
        static bool instance = false;
        return instance;
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override {
        return kNumBroadPhaseLayers;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (layer == kStaticObjectLayer) {
            return kStaticBroadPhaseLayer;
        }
        if (layer == kMovingObjectLayer) {
            return kMovingBroadPhaseLayer;
        }
        return JPH::BroadPhaseLayer(0);
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        if (layer == kStaticBroadPhaseLayer) {
            return "STATIC";
        }
        if (layer == kMovingBroadPhaseLayer) {
            return "MOVING";
        }
        return "UNKNOWN";
    }
#endif
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer first,
        JPH::ObjectLayer second
    ) const override {
        return (first == kStaticObjectLayer && second == kMovingObjectLayer) ||
               (first == kMovingObjectLayer && second == kStaticObjectLayer) ||
               (first == kMovingObjectLayer && second == kMovingObjectLayer);
    }
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer objectLayer,
        JPH::BroadPhaseLayer broadPhaseLayer
    ) const override {
        if (objectLayer == kStaticObjectLayer) {
            return broadPhaseLayer == kMovingBroadPhaseLayer;
        }
        if (objectLayer == kMovingObjectLayer) {
            return broadPhaseLayer == kStaticBroadPhaseLayer || broadPhaseLayer == kMovingBroadPhaseLayer;
        }
        return false;
    }
};

}

namespace physics {

struct PhysicsWorld::Impl {
    JoltRuntime runtime;
    JPH::TempAllocatorImpl tempAllocator;
    JPH::JobSystemThreadPool jobSystem;
    BroadPhaseLayerInterface broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilter objectLayerPairFilter;
    JPH::PhysicsSystem physicsSystem;
    std::vector<BodyHandle> bodyHandles;

    Impl(
        std::size_t maxBodies,
        std::size_t maxBodyPairs,
        std::size_t maxContactConstraints
    )
        : runtime(),
          tempAllocator(32 * 1024 * 1024),
          jobSystem(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers),
          broadPhaseLayerInterface(),
          objectVsBroadPhaseLayerFilter(),
          objectLayerPairFilter(),
          physicsSystem(),
          bodyHandles() {
        if (maxBodies == 0 || maxBodies > JPH::PhysicsSystem::cMaxBodiesLimit ||
            maxBodyPairs == 0 || maxBodyPairs > JPH::PhysicsSystem::cMaxBodyPairsLimit ||
            maxContactConstraints == 0 ||
            maxContactConstraints > JPH::PhysicsSystem::cMaxContactConstraintsLimit) {
            throw std::invalid_argument("PhysicsWorld: invalid simulation capacity");
        }

        physicsSystem.Init(
            static_cast<JPH::uint>(maxBodies),
            0,
            static_cast<JPH::uint>(maxBodyPairs),
            static_cast<JPH::uint>(maxContactConstraints),
            broadPhaseLayerInterface,
            objectVsBroadPhaseLayerFilter,
            objectLayerPairFilter
        );
        physicsSystem.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }

    ~Impl() {
        JPH::BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();
        for (auto it = bodyHandles.rbegin(); it != bodyHandles.rend(); ++it) {
            if (bodyInterface.IsAdded(*it)) {
                bodyInterface.RemoveBody(*it);
            }
            bodyInterface.DestroyBody(*it);
        }
    }

    BodyHandle createShape(
        const JPH::Shape* shape,
        const glm::vec3& position,
        const glm::quat& rotation,
        JPH::EMotionType motionType,
        JPH::ObjectLayer objectLayer,
        JPH::EActivation activation
    ) {
        JPH::BodyCreationSettings settings(
            shape,
            toJoltPosition(position),
            toJoltRotation(rotation),
            motionType,
            objectLayer
        );
        settings.mFriction = 0.8f;
        settings.mRestitution = 0.0f;
        settings.mLinearDamping = 0.05f;
        settings.mAngularDamping = 0.05f;
        settings.mMotionQuality = JPH::EMotionQuality::LinearCast;

        JPH::BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();
        const BodyHandle handle = bodyInterface.CreateAndAddBody(settings, activation);
        if (handle.IsInvalid()) {
            throw std::runtime_error("PhysicsWorld: unable to create body");
        }
        bodyHandles.push_back(handle);
        return handle;
    }
};

PhysicsWorld::PhysicsWorld(
    std::size_t maxBodies,
    std::size_t maxBodyPairs,
    std::size_t maxContactConstraints
) : impl_(std::make_unique<Impl>(maxBodies, maxBodyPairs, maxContactConstraints)) {}

PhysicsWorld::~PhysicsWorld() = default;

PhysicsWorld::BodyHandle PhysicsWorld::createStaticBox(
    const glm::vec3& halfExtents,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    if (!isFinite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f ||
        halfExtents.z <= 0.0f || !isFinite(position) || !isFinite(rotation)) {
        throw std::invalid_argument("PhysicsWorld: invalid static box");
    }

    JPH::Ref<JPH::Shape> shape = new JPH::BoxShape(toJoltVec3(halfExtents));
    return impl_->createShape(
        shape.GetPtr(),
        position,
        rotation,
        JPH::EMotionType::Static,
        kStaticObjectLayer,
        JPH::EActivation::DontActivate
    );
}

PhysicsWorld::BodyHandle PhysicsWorld::createStaticPlane(
    const glm::vec3& position,
    float halfExtent
) {
    if (!isFinite(position) || !std::isfinite(halfExtent) || halfExtent <= 0.0f) {
        throw std::invalid_argument("PhysicsWorld: invalid static plane");
    }

    const JPH::Plane plane = JPH::Plane::sFromPointAndNormal(
        JPH::Vec3::sZero(),
        JPH::Vec3(0.0f, 1.0f, 0.0f)
    );
    JPH::Ref<JPH::Shape> shape = new JPH::PlaneShape(plane, nullptr, halfExtent);
    return impl_->createShape(
        shape.GetPtr(),
        position,
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        JPH::EMotionType::Static,
        kStaticObjectLayer,
        JPH::EActivation::DontActivate
    );
}

PhysicsWorld::BodyHandle PhysicsWorld::createDynamicBox(
    const glm::vec3& halfExtents,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    if (!isFinite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f ||
        halfExtents.z <= 0.0f || !isFinite(position) || !isFinite(rotation)) {
        throw std::invalid_argument("PhysicsWorld: invalid dynamic box");
    }

    JPH::Ref<JPH::Shape> shape = new JPH::BoxShape(toJoltVec3(halfExtents));
    return impl_->createShape(
        shape.GetPtr(),
        position,
        rotation,
        JPH::EMotionType::Dynamic,
        kMovingObjectLayer,
        JPH::EActivation::Activate
    );
}

physics::PhysicsWorld::BodyHandle PhysicsWorld::createDynamicCapsule(
    float halfHeight,
    float radius,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    if (!std::isfinite(halfHeight) || !std::isfinite(radius) || halfHeight < 0.0f ||
        radius <= 0.0f || !isFinite(position) || !isFinite(rotation)) {
        throw std::invalid_argument("PhysicsWorld: invalid dynamic capsule");
    }

    JPH::Ref<JPH::Shape> shape = new JPH::CapsuleShape(halfHeight, radius);
    JPH::BodyCreationSettings settings(
        shape.GetPtr(),
        toJoltPosition(position),
        toJoltRotation(rotation),
        JPH::EMotionType::Dynamic,
        kMovingObjectLayer
    );
    settings.mFriction = 0.2f;   // не «прилипать» к склонам и не скользить
    settings.mRestitution = 0.0f;
    settings.mLinearDamping = 0.0f;  // торможение делает контроллер, не демпфер
    // Только перемещения, без вращений. Иначе капсула на уклоне заваливается
    // набок и персонаж «плюхается» набок, а вертикальность тела ломает и
    // проверку опоры, и рендер.
    settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                            JPH::EAllowedDOFs::TranslationY |
                            JPH::EAllowedDOFs::TranslationZ;
    JPH::BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    const BodyHandle handle =
        bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (handle.IsInvalid()) {
        throw std::runtime_error("PhysicsWorld: unable to create capsule body");
    }
    impl_->bodyHandles.push_back(handle);
    playerBody_ = handle;
    return handle;
}

bool PhysicsWorld::raycastDown(
    const glm::vec3& from,
    const glm::vec3& to,
    float& outDistance,
    glm::vec3& outNormal
) const {
    if (!isFinite(from) || !isFinite(to)) {
        return false;
    }
    JPH::RRayCast ray{toJoltVec3(from), toJoltVec3(to - from)};
    JPH::RayCastResult result{};
    // Луч стартует ВНУТРИ капсулы игрока, поэтому собственное тело надо
    // исключить: иначе он всегда цеплял бы низ капсулы и «опора» была бы
    // всегда истинной — персонаж считал бы, что стоит на земле в воздухе.
    JPH::IgnoreSingleBodyFilter bodyFilter(playerBody_);
    const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(
        impl_->objectVsBroadPhaseLayerFilter, kMovingObjectLayer);
    const JPH::DefaultObjectLayerFilter objectFilter(
        impl_->objectLayerPairFilter, kMovingObjectLayer);
    const bool hit = impl_->physicsSystem.GetNarrowPhaseQuery().CastRay(
        ray, result, broadPhaseFilter, objectFilter, bodyFilter);
    if (!hit || result.mBodyID.IsInvalid()) {
        return false;
    }
    outDistance = result.mFraction * glm::length(to - from);
    // У RayCastResult нет готовой нормали: её считает тело по под-фигуре.
    const JPH::BodyLockRead lock(impl_->physicsSystem.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded()) {
        return false;
    }
    const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(
        result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
    outNormal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
    return true;
}


physics::PhysicsWorld::BodyHandle PhysicsWorld::createStaticShape(
    const JPH::Shape& shape, const glm::vec3& position, const glm::quat& rotation
) {
    if (!isFinite(position) || !isFinite(rotation)) {
        throw std::invalid_argument("PhysicsWorld: invalid static shape transform");
    }

    return impl_->createShape(
        &shape,
        position,
        rotation,
        JPH::EMotionType::Static,
        kStaticObjectLayer,
        JPH::EActivation::DontActivate
    );
}

void PhysicsWorld::removeBody(BodyHandle handle) {
    const auto it = std::find(impl_->bodyHandles.begin(), impl_->bodyHandles.end(), handle);
    if (it == impl_->bodyHandles.end()) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }

    JPH::BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    if (bodyInterface.IsAdded(handle)) {
        bodyInterface.RemoveBody(handle);
    }
    bodyInterface.DestroyBody(handle);
    impl_->bodyHandles.erase(it);
}

bool PhysicsWorld::isBodyValid(BodyHandle handle) const noexcept {
    return !handle.IsInvalid() &&
           std::find(impl_->bodyHandles.begin(), impl_->bodyHandles.end(), handle) !=
               impl_->bodyHandles.end();
}

void PhysicsWorld::setBodyTransform(
    BodyHandle handle,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    if (!isBodyValid(handle)) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }
    if (!isFinite(position) || !isFinite(rotation)) {
        throw std::invalid_argument("PhysicsWorld: invalid body transform");
    }

    impl_->physicsSystem.GetBodyInterface().SetPositionAndRotationWhenChanged(
        handle,
        toJoltPosition(position),
        toJoltRotation(rotation),
        JPH::EActivation::Activate
    );
}

glm::vec3 PhysicsWorld::bodyPosition(BodyHandle handle) const {
    if (!isBodyValid(handle)) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }
    return toGlmVec3(impl_->physicsSystem.GetBodyInterface().GetPosition(handle));
}

glm::quat PhysicsWorld::bodyRotation(BodyHandle handle) const {
    if (!isBodyValid(handle)) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }
    return toGlmQuat(impl_->physicsSystem.GetBodyInterface().GetRotation(handle));
}

glm::vec3 PhysicsWorld::bodyLinearVelocity(BodyHandle handle) const {
    if (!isBodyValid(handle)) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }
    return toGlmVec3(impl_->physicsSystem.GetBodyInterface().GetLinearVelocity(handle));
}

void PhysicsWorld::setBodyLinearVelocity(
    BodyHandle handle,
    const glm::vec3& velocity
) {
    if (!isBodyValid(handle)) {
        throw std::out_of_range("PhysicsWorld: body is not registered");
    }
    if (!isFinite(velocity)) {
        throw std::invalid_argument("PhysicsWorld: invalid body velocity");
    }

    impl_->physicsSystem.GetBodyInterface().SetLinearVelocity(handle, toJoltVec3(velocity));
}

void PhysicsWorld::step(float deltaTime) {
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }

    const float simulationDelta = std::min(deltaTime, 0.25f);
    const int collisionSteps = std::clamp(
        static_cast<int>(std::ceil(simulationDelta * 60.0f)),
        1,
        15
    );
    const JPH::EPhysicsUpdateError error = impl_->physicsSystem.Update(
        simulationDelta,
        collisionSteps,
        &impl_->tempAllocator,
        &impl_->jobSystem
    );
    if (error != JPH::EPhysicsUpdateError::None) {
        // Нехватка контактных слотов — не фатальная ошибка: один «плотный» кадр
        // не должен убивать игру. Предупреждаем не чаще раза в секунду, чтобы
        // не заливать лог; симуляция в этом кадре просто теряет часть контактов.
        static float lastWarningTime = -10.0f;
        static float accumulatedTime = 0.0f;
        accumulatedTime += simulationDelta;
        if (accumulatedTime - lastWarningTime >= 1.0f) {
            lastWarningTime = accumulatedTime;
            core::Logger::warn(
                "PhysicsWorld: Jolt сообщил о нехватке ёмкости симуляции "
                "(контакты/пары тел). Увеличьте maxContactConstraints/maxBodyPairs.");
        }
    }
}

}
