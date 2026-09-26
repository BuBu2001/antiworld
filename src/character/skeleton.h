#pragma once

#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "character/body_parts.h"

namespace character {

// Кость скелета в bind-pose: положение сустава в локальных координатах
// родителя, ориентация сегмента (локальная -Y направлена вдоль сегмента от
// сустава к дочерней кости) и индекс родителя (root — сам себе родитель).
struct BoneDef {
    Bone bone{Bone::Root};
    Bone parent{Bone::Root};
    glm::vec3 localPosition{0.0f};
    glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

class Skeleton {
public:
    static constexpr int kBoneCount = boneIndex(Bone::Count);

    // Строит скелет по размерам тела. Все позиции считаются из размеров,
    // поэтому персонаж любого роста сохраняет пропорции.
    explicit Skeleton(const BodyDimensions& dimensions = {});

    const std::array<BoneDef, kBoneCount>& bones() const { return bones_; }
    const BoneDef& bone(Bone bone) const { return bones_[boneIndex(bone)]; }
    const BodyDimensions& dimensions() const { return dimensions_; }

    // Мировая матрица bind-pose i-й кости (T * R * L цепочкой от root).
    const glm::mat4& bindMatrix(Bone bone) const {
        return bindMatrices_[boneIndex(bone)];
    }

    // Текущая мировая матрица той же кости после FK-поворотов поз.
    glm::mat4 animatedMatrix(Bone bone, const std::array<glm::quat, kBoneCount>& pose) const;

private:
    void build();

    BodyDimensions dimensions_;
    std::array<BoneDef, kBoneCount> bones_{};
    std::array<glm::mat4, kBoneCount> bindMatrices_{};
};

}  // namespace character
