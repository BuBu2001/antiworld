#include "character/animation.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace character {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

// Ось сгиба локтя/колена в локальных осях кости сегмента, направленного
// вдоль -Y: локтевой сустав гнётся вокруг локальной X (для руки — назад к
// спине при supination, поэтому знак подбирается позой), колено — вперёд.
void applyFingerCurl(Pose& pose, const HandBones& hand, float curl) {
    for (const FingerBones& finger : hand.fingers) {
        // Сгиб MCP/PIP/DIP идёт вокруг локальной +Z кисти (ладонь смотрит
        // в -Z, пальцы сгибаются в сторону ладони).
        pose.bend(finger.prox, 0.0f, 0.0f, curl);
        pose.bend(finger.mid, 0.0f, 0.0f, curl * 0.8f);
        pose.bend(finger.tip, 0.0f, 0.0f, curl * 0.6f);
    }
    // Большой палец сгибается сильнее к указательному (противопоставление).
    pose.bend(hand.fingers[kThumb].prox, 0.0f, 0.35f * curl, 0.4f * curl);
    pose.bend(hand.fingers[kThumb].mid, 0.0f, 0.0f, 0.7f * curl);
    pose.bend(hand.fingers[kThumb].tip, 0.0f, 0.0f, 0.5f * curl);
}

}  // namespace

Pose AnimationGenerator::idle(float time, float breathingRate) {
    Pose pose;
    const float breath = std::sin(time * 2.0f * kPi * breathingRate);
    const float sway = std::sin(time * 0.6f);

    // Дыхание: грудь поднимается, голова чуть кивает в такт.
    pose.bend(Bone::Spine, 0.01f * breath);
    pose.bend(Bone::Chest, 0.025f * breath);
    pose.bend(Bone::Neck, -0.015f * breath);
    pose.bend(Bone::Head, 0.01f * breath + 0.02f * sway);
    pose.bend(Bone::Root, 0.0f, 0.0f, 0.006f * sway);

    // Руки в покое слегка согнуты и отведены от корпуса, кисти расслаблены.
    const HandBones hands[2] = {handLeft(), handRight()};
    for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? 1.0f : -1.0f;
        const HandBones& hand = hands[side];
        pose.bend(hand.shoulder, 0.0f, 0.0f, sx * 0.04f);
        pose.bend(hand.upperArm, 0.0f, 0.0f, sx * 0.05f);
        // Локоть: сгиб назад (локальная -Z для руки вниз = локоть внутрь).
        pose.bend(hand.forearm, 0.0f, 0.0f, -0.18f - 0.02f * breath);
        pose.bend(hand.hand, 0.0f, 0.0f, -0.05f);
        applyFingerCurl(pose, hand, 0.12f + 0.03f * std::sin(time * 0.9f + side));
    }

    // Ноги почти прямые, вес чуть смещён на одну ногу.
    pose.bend(Bone::UpperLegL, 0.0f, 0.0f, 0.02f * sway);
    pose.bend(Bone::UpperLegR, 0.0f, 0.0f, -0.02f * sway);
    pose.bend(Bone::LowerLegL, 0.0f, 0.0f, -0.03f);
    pose.bend(Bone::LowerLegR, 0.0f, 0.0f, -0.02f);
    return pose;
}

Pose AnimationGenerator::walk(float time, float speed) {
    Pose pose;
    const float normalized = clampf(std::abs(speed) / 6.0f, 0.0f, 1.0f);
    // Темп шага растёт со скоростью: ~1.6 Гц при ходьбе, ~3 Гц при беге.
    const float stepFrequency = 1.1f + 2.2f * normalized;
    const float phase = time * 2.0f * kPi * stepFrequency;
    const float swing = std::sin(phase);
    const float swingAlt = std::sin(phase + kPi);  // противофаза другой ноги
    const float amp = 0.35f + 0.45f * normalized;  // амплитуда маха бедра

    // Покачивание таза и наклон корпуса вперёд при ускорении.
    pose.bend(Bone::Root, 0.0f, 0.06f * swing * (0.5f + normalized), 0.0f);
    pose.bend(Bone::Hips, 0.0f, 0.0f, 0.05f * swing);
    pose.bend(Bone::Spine, 0.04f + 0.08f * normalized);
    pose.bend(Bone::Chest, 0.03f + 0.06f * normalized);
    // Голова стабилизирует горизонт: компенсирует часть наклона корпуса.
    pose.bend(Bone::Neck, -0.05f - 0.08f * normalized);
    pose.bend(Bone::Head, -0.02f * normalized, -0.04f * swing);

    // === Ноги: бедро качается в сагиттальной плоскости (локальная X для
    // сегмента вниз), колено сгибается только в фазе переноса. ===
    struct LegBones { Bone upper, lower, foot, toes; };
    const LegBones legs[2] = {{Bone::UpperLegL, Bone::LowerLegL, Bone::FootL, Bone::ToesL},
                              {Bone::UpperLegR, Bone::LowerLegR, Bone::FootR, Bone::ToesR}};
    const float phases[2] = {swing, swingAlt};
    for (int side = 0; side < 2; ++side) {
        const LegBones& leg = legs[side];
        const float s = phases[side];
        // Мах бедра: положительный X — колено идёт вперёд (нога назад)?
        // Для сегмента вдоль -Y поворот вокруг +X на угол a отклоняет
        // направление сегмента (-Y) к -Z (вперёд лицом): значит +a = нога
        // вперёд.
        pose.bend(leg.upper, 0.0f, 0.0f, 0.0f);
        pose[leg.upper] = glm::angleAxis(-amp * s, glm::vec3{1.0f, 0.0f, 0.0f});
        // Колено сгибается, когда нога на выносе (s < 0 => мах назад):
        // подтягиваем пятку к ягодице в передней половине цикла.
        const float lift = std::max(0.0f, -s);
        const float kneeBend = (0.25f + 0.9f * normalized) * lift + 0.05f;
        // Поворот голени вокруг той же оси X в ту же сторону (сгиб назад).
        pose[leg.lower] = glm::angleAxis(kneeBend, glm::vec3{1.0f, 0.0f, 0.0f});
        // Голеностоп: тыльное сгибание при опоре, подошвенное при отталкивании.
        const float ankle = 0.15f * std::sin(phase + side * kPi + 1.2f) - 0.1f * lift;
        pose[leg.foot] = glm::angleAxis(ankle, glm::vec3{1.0f, 0.0f, 0.0f}) *
                         pose[leg.foot];
        pose.bend(leg.toes, 0.0f, 0.0f, -0.2f * std::max(0.0f, s));
    }

    // === Руки: плечо качается в противофазу одноимённой ноге, локоть согнут. ===
    const HandBones arms[2] = {handLeft(), handRight()};
    const float armPhases[2] = {swingAlt, swing};  // левая рука с правой ногой
    for (int side = 0; side < 2; ++side) {
        const HandBones& arm = arms[side];
        const float s = armPhases[side];
        const float reach = 0.30f + 0.5f * normalized;
        // Плечо: мах вперёд-назад вокруг X.
        pose[arm.upperArm] = glm::angleAxis(reach * s, glm::vec3{1.0f, 0.0f, 0.0f});
        // Небольшое отведение от корпуса при быстром беге.
        pose[arm.upperArm] *= glm::angleAxis(0.06f * normalized, glm::vec3{0.0f, 0.0f, 1.0f});
        // Локоть: согнут ~90° при беге, ~20° при ходьбе; раскачивается.
        const float elbow = -(0.35f + 1.1f * normalized + 0.15f * s);
        pose[arm.forearm] = glm::angleAxis(elbow, glm::vec3{0.0f, 0.0f, 1.0f});
        pose[arm.hand] = glm::angleAxis(0.1f * s, glm::vec3{0.0f, 0.0f, 1.0f});
        applyFingerCurl(pose, arm, 0.35f + 0.25f * normalized);
    }
    return pose;
}

Pose AnimationGenerator::fall(float verticalVelocity) {
    Pose pose;
    const float panic = clampf(std::abs(verticalVelocity) / 20.0f, 0.0f, 1.0f);

    // Корпус отклоняется назад, голова вскидывается вверх (ищем опору взглядом).
    pose.bend(Bone::Spine, -0.15f);
    pose.bend(Bone::Chest, -0.2f * panic);
    pose.bend(Bone::Neck, 0.25f);
    pose.bend(Bone::Head, 0.2f);

    const HandBones hands[2] = {handLeft(), handRight()};
    for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? 1.0f : -1.0f;
        const HandBones& hand = hands[side];
        // Руки всплывают вверх-в стороны («парашют»).
        pose[hand.upperArm] = glm::angleAxis(-1.6f - 0.5f * panic, glm::vec3{1.0f, 0.0f, 0.0f}) *
                              glm::angleAxis(sx * 0.5f, glm::vec3{0.0f, 0.0f, 1.0f});
        pose[hand.forearm] = glm::angleAxis(-0.9f, glm::vec3{0.0f, 0.0f, 1.0f});
        applyFingerCurl(pose, hand, 0.7f);  // растопыренные напряжённые пальцы
    }

    // Ноги поджимаются: бедро вперёд, колено согнуто (поза группировки).
    pose[Bone::UpperLegL] = glm::angleAxis(-0.5f, glm::vec3{1.0f, 0.0f, 0.0f});
    pose[Bone::UpperLegR] = glm::angleAxis(-0.35f, glm::vec3{1.0f, 0.0f, 0.0f});
    pose[Bone::LowerLegL] = glm::angleAxis(0.9f, glm::vec3{1.0f, 0.0f, 0.0f});
    pose[Bone::LowerLegR] = glm::angleAxis(0.6f, glm::vec3{1.0f, 0.0f, 0.0f});
    pose[Bone::FootL] = glm::angleAxis(0.3f, glm::vec3{1.0f, 0.0f, 0.0f});
    pose[Bone::FootR] = glm::angleAxis(0.3f, glm::vec3{1.0f, 0.0f, 0.0f});
    return pose;
}

void AnimationGenerator::blend(const Pose& a, const Pose& b, float w, Pose& out) {
    const float t = clampf(w, 0.0f, 1.0f);
    for (int i = 0; i < Skeleton::kBoneCount; ++i) {
        out.localRotations[i] = glm::slerp(a.localRotations[i], b.localRotations[i], t);
    }
}

}  // namespace character
