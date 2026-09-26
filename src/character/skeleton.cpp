#include "character/skeleton.h"

#include <glm/gtc/matrix_transform.hpp>

namespace character {
namespace {

// Локальная ориентация сегмента: -Y кости направлена вдоль сегмента.
glm::quat align(const glm::vec3& direction) {
    return glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f), glm::normalize(direction));
}

}  // namespace

Skeleton::Skeleton(const BodyDimensions& dimensions) : dimensions_(dimensions) {
    build();
}

void Skeleton::build() {
    const BodyDimensions& d = dimensions_;

    auto set = [this](Bone bone, Bone parent, const glm::vec3& position, const glm::quat& rotation) {
        BoneDef& def = bones_[boneIndex(bone)];
        def.bone = bone;
        def.parent = parent;
        def.localPosition = position;
        def.localRotation = rotation;
    };

    // === Осевой скелет: root в центре бёдер (высота — от земли) ===
    const float hipY = d.thighHalfLength * 2.0f + d.shinHalfLength * 2.0f + d.ankleToGround;
    set(Bone::Root, Bone::Root, {0.0f, hipY, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    set(Bone::Hips, Bone::Root, {0.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    set(Bone::Spine, Bone::Hips, {0.0f, -d.pelvisHalfHeight, 0.0f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    set(Bone::Chest, Bone::Spine, {0.0f, -d.abdomenHalfHeight, 0.0f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    set(Bone::Neck, Bone::Chest, {0.0f, -d.chestHalfHeight, 0.0f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    set(Bone::Head, Bone::Neck, {0.0f, -d.neckHalfHeight, 0.0f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    // Челюсть шарнирно сидит чуть ниже/сзади центра головы.
    set(Bone::Jaw, Bone::Head, {0.0f, -d.headRadius * 0.45f, -d.headRadius * 0.25f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});

    // === Руки: плечо свисает вниз (локальная -Y сегмента = мировая -Y) ===
    const glm::quat armDown = align(glm::vec3(0.0f, -1.0f, 0.0f));
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? 1.0f : -1.0f;  // левая сторона — +X
        const HandBones hand = (side == 0) ? handLeft() : handRight();

        set(hand.shoulder, Bone::Chest,
            {sx * (d.chestWidth - 0.02f), -d.chestHalfHeight + 0.035f, 0.0f}, armDown);
        set(hand.upperArm, hand.shoulder,
            {sx * d.clavicleLength, 0.0f, 0.0f}, armDown);
        set(hand.forearm, hand.upperArm,
            {0.0f, -d.upperArmHalfLength * 2.0f, 0.0f}, armDown);
        set(hand.hand, hand.forearm,
            {0.0f, -d.forearmHalfLength * 2.0f, 0.0f}, armDown);

        // Ладонь смотрит вперёд (в анатомической позе ладони развёрнуты к
        // лицу): локальная -Z кисти направлена в мировую +Z.
        const glm::quat palmForward = glm::rotation(glm::vec3(0.0f, 0.0f, -1.0f),
                                                    glm::vec3(0.0f, 0.0f, 1.0f)) *
                                      armDown;
        const Bone wrist = hand.hand;

        // Четыре длинных пальца: ряд костяшек поперёк ладони, лёгкий веер.
        const float knuckleY = -(d.handHalfLength - 0.012f);
        const float spread = 0.017f;  // межпальцевой промежуток
        const int fingerOrder[4] = {kIndex, kMiddle, kRing, kLittle};
        const float fingerScale[4] = {0.94f, 1.0f, 0.93f, 0.78f};
        for (int f = 0; f < 4; ++f) {
            const FingerBones& fb = hand.fingers[fingerOrder[f]];
            const float offset = (static_cast<float>(f) - 1.5f) * spread;
            const float scale = fingerScale[f];
            // Небольшое отклонение крайних пальцев наружу (веер).
            const glm::vec3 dir = glm::normalize(
                glm::vec3{sx * offset * 0.6f, -1.0f, -0.05f * static_cast<float>(f)});
            set(fb.prox, wrist, {sx * offset, knuckleY, d.palmThickness * 0.6f},
                glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f), dir) * palmForward);
            set(fb.mid, fb.prox, {0.0f, -d.fingerProxLen * scale, 0.0f}, armDown);
            set(fb.tip, fb.mid, {0.0f, -d.fingerMidLen * scale, 0.0f}, armDown);
        }

        // Большой палец: пястная кость торчит из края ладони вперёд-наружу,
        // остальные две секции идут вдоль указательного (противопоставление).
        const FingerBones& thumb = hand.fingers[kThumb];
        set(thumb.prox /* metacarpal */, wrist,
            {sx * (d.wristRadius + 0.008f), -0.030f, d.palmThickness + 0.006f},
            glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f),
                          glm::normalize(glm::vec3{sx * 0.75f, -0.45f, 0.65f})) * palmForward);
        set(thumb.mid /* proximal phalanx */, thumb.prox,
            {0.0f, -d.thumbProxLen * 0.75f, 0.0f}, armDown);
        set(thumb.tip /* distal phalanx */, thumb.mid,
            {0.0f, -d.thumbProxLen * 0.55f, 0.0f}, armDown);
    }

    // === Ноги: бедро вниз, колено — низ бедра, стопа повёрнута вперёд ===
    const glm::quat legDown = align(glm::vec3(0.0f, -1.0f, 0.0f));
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? 1.0f : -1.0f;
        const Bone upperLeg = (side == 0) ? Bone::UpperLegL : Bone::UpperLegR;
        const Bone lowerLeg = (side == 0) ? Bone::LowerLegL : Bone::LowerLegR;
        const Bone foot = (side == 0) ? Bone::FootL : Bone::FootR;
        const Bone toes = (side == 0) ? Bone::ToesL : Bone::ToesR;

        set(upperLeg, Bone::Hips, {sx * d.legSpacing, -d.pelvisHalfHeight * 0.4f, 0.0f}, legDown);
        set(lowerLeg, upperLeg, {0.0f, -d.thighHalfLength * 2.0f, 0.0f}, legDown);
        // Стопа: локальная -Y голеностопа направлена вперёд (+Z) — носок.
        set(foot, lowerLeg, {0.0f, -d.shinHalfLength * 2.0f, 0.0f},
            align(glm::vec3(0.0f, 0.0f, -1.0f)));
        set(toes, foot, {0.0f, -d.footLength, 0.0f},
            glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    }

    // === Мировые матрицы bind-pose (обход сверху вниз: индексы костей
    // уже упорядочены иерархией — родитель всегда имеет меньший индекс) ===
    bindMatrices_[boneIndex(Bone::Root)] =
        glm::translate(glm::mat4(1.0f), bones_[boneIndex(Bone::Root)].localPosition);
    for (int i = 1; i < kBoneCount; ++i) {
        const BoneDef& def = bones_[i];
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), def.localPosition) *
            glm::mat4_cast(def.localRotation);
        bindMatrices_[i] = bindMatrices_[boneIndex(def.parent)] * local;
    }
}

glm::mat4 Skeleton::animatedMatrix(
    Bone bone, const std::array<glm::quat, kBoneCount>& pose) const {
    // Собираем цепочку родителей от кости до root и проходим сверху вниз.
    std::array<int, kBoneCount> chain{};
    int length = 0;
    for (int current = boneIndex(bone);; current = boneIndex(bones_[current].parent)) {
        chain[length++] = current;
        if (current == boneIndex(Bone::Root)) break;
    }

    glm::mat4 matrix(1.0f);
    for (int i = length - 1; i >= 0; --i) {
        const BoneDef& def = bones_[chain[i]];
        matrix = matrix * glm::translate(glm::mat4(1.0f), def.localPosition) *
                 glm::mat4_cast(def.localRotation * pose[chain[i]]);
    }
    return matrix;
}

}  // namespace character
