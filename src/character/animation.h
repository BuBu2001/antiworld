#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "character/body_parts.h"
#include "character/skeleton.h"

namespace character {

// Поза персонажа: локальный поворот каждой кости относительно bind-pose.
// Единичный кватернион — «смирно» (bind-pose). Повороты композитятся поверх
// локальной ориентации кости в FK-цепочке (Skeleton::animatedMatrix).
struct Pose {
    std::array<glm::quat, Skeleton::kBoneCount> localRotations{};

    Pose() {
        for (auto& rotation : localRotations) rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }

    void reset() { *this = Pose{}; }

    glm::quat& operator[](Bone bone) { return localRotations[boneIndex(bone)]; }
    const glm::quat& operator[](Bone bone) const { return localRotations[boneIndex(bone)]; }

    // Поворот кости вокруг её локальной оси (X — отведение/приведение,
    // Y — ротация вдоль сегмента, Z — сгиб в суставе для конечностей вниз).
    void bend(Bone bone, float angleX, float angleY = 0.0f, float angleZ = 0.0f) {
        glm::quat rotation = glm::angleAxis(angleX, glm::vec3{1.0f, 0.0f, 0.0f});
        rotation *= glm::angleAxis(angleY, glm::vec3{0.0f, 1.0f, 0.0f});
        rotation *= glm::angleAxis(angleZ, glm::vec3{0.0f, 0.0f, 1.0f});
        localRotations[boneIndex(bone)] = rotation;
    }
};

// Генератор процедурных анимаций. Все фазовые функции возвращают позу для
// момента времени t (секунды); частота шага и амплитуды зависят от скорости
// движения, поэтому бег отличается от ходьбы не только темпом.
class AnimationGenerator {
public:
    // Поза стоя: лёгкий естественный полусогнут рук и головы, дыхание.
    static Pose idle(float time, float breathingRate = 0.25f);

    // Ходьба/бег:походку с опорной перекатывающейся стопой, противофазной
    // работой рук, покачиванием таза и наклоном корпуса. speed — м/с.
    static Pose walk(float time, float speed);

    // Свободное падение: руки вверх-в стороны («самолёт»), ноги согнуты.
    static Pose fall(float verticalVelocity);

    // Смешение поз по весу w (линейная интерполяция кватерниоров через slerp).
    static void blend(const Pose& a, const Pose& b, float w, Pose& out);
};

}  // namespace character
