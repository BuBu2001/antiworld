#pragma once

#include <array>
#include <cstdint>

namespace character {

// Схема скелета процедурного персонажа (см. skeleton.h). Все размеры — в
// метрах, рост по умолчанию 1.8 м; пропорции приближены к классическому
// канону фигуры «в 8 голов»: голова ~0.24 м, плечевой пояс чуть шире таза,
// кончики пальцев в позе смирно достают середины бедра.
//
// Иерархия костей:
//   root (таз, центр масс)
//   ├── hips → spine → chest → neck → head
//   │                                  └── jaw
//   ├── chest → shoulder(L/R) → upperArm(L/R) → forearm(L/R) → hand(L/R)
//   │           → фаланги 5 пальцев (index/middle/ring/little — 3 секции,
//   │             thumb — запястная + 2 секции)
//   └── hips → upperLeg(L/R) → lowerLeg(L/R) → foot(L/R) → toes(L/R)
enum class Bone : std::uint8_t {
    Root,
    Hips, Spine, Chest, Neck, Head, Jaw,
    ShoulderL, UpperArmL, ForearmL, HandL,
    IndexProxL, IndexMidL, IndexTipL,
    MiddleProxL, MiddleMidL, MiddleTipL,
    RingProxL, RingMidL, RingTipL,
    LittleProxL, LittleMidL, LittleTipL,
    ThumbMetaL, ThumbProxL, ThumbTipL,
    ShoulderR, UpperArmR, ForearmR, HandR,
    IndexProxR, IndexMidR, IndexTipR,
    MiddleProxR, MiddleMidR, MiddleTipR,
    RingProxR, RingMidR, RingTipR,
    LittleProxR, LittleMidR, LittleTipR,
    ThumbMetaR, ThumbProxR, ThumbTipR,
    UpperLegL, LowerLegL, FootL, ToesL,
    UpperLegR, LowerLegR, FootR, ToesR,
    Count
};

inline constexpr int boneIndex(Bone bone) {
    return static_cast<int>(bone);
}

// Пальцы кисти: четыре длинных (3 фаланги) и большой палец (плюсна +
// проксимальная + дистальная фаланги — три костные секции для
// единообразия анимации с остальными пальцами).
struct FingerBones {
    Bone prox{};
    Bone mid{};
    Bone tip{};
};

struct HandBones {
    Bone shoulder{};
    Bone upperArm{};
    Bone forearm{};
    Bone hand{};
    std::array<FingerBones, 5> fingers{};  // index, middle, ring, little, thumb
};

inline constexpr int kIndex = 0, kMiddle = 1, kRing = 2, kLittle = 3, kThumb = 4;

constexpr HandBones handLeft() {
    return HandBones{
        Bone::ShoulderL, Bone::UpperArmL, Bone::ForearmL, Bone::HandL,
        {{Bone::IndexProxL, Bone::IndexMidL, Bone::IndexTipL},
         {Bone::MiddleProxL, Bone::MiddleMidL, Bone::MiddleTipL},
         {Bone::RingProxL, Bone::RingMidL, Bone::RingTipL},
         {Bone::LittleProxL, Bone::LittleMidL, Bone::LittleTipL},
         {Bone::ThumbMetaL, Bone::ThumbProxL, Bone::ThumbTipL}}};
}

constexpr HandBones handRight() {
    return HandBones{
        Bone::ShoulderR, Bone::UpperArmR, Bone::ForearmR, Bone::HandR,
        {{Bone::IndexProxR, Bone::IndexMidR, Bone::IndexTipR},
         {Bone::MiddleProxR, Bone::MiddleMidR, Bone::MiddleTipR},
         {Bone::RingProxR, Bone::RingMidR, Bone::RingTipR},
         {Bone::LittleProxR, Bone::LittleMidR, Bone::LittleTipR},
         {Bone::ThumbMetaR, Bone::ThumbProxR, Bone::ThumbTipR}}};
}

// Линейные размеры тела (половинные длины сегментов и радиусы), метры.
struct BodyDimensions {
    float height = 1.8f;            // полный рост; масштаб всех ниже

    // Позвоночник / корпус.
    float pelvisHalfHeight = 0.075f;
    float abdomenHalfHeight = 0.065f;
    float chestHalfHeight = 0.095f;
    float neckHalfHeight = 0.050f;
    float torsoDepth = 0.115f;      // полутолщина корпуса (вперёд-назад)
    float chestWidth = 0.185f;      // полуширина грудной клетки
    float waistWidth = 0.135f;
    float pelvisWidth = 0.155f;

    // Голова: овал черепа + лицо, челюсть — отдельная кость.
    float headRadius = 0.098f;
    float headElongation = 1.18f;   // вытянутость вверх
    float jawHalfHeight = 0.030f;

    // Плечевой пояс и руки.
    float clavicleLength = 0.155f;  // от груди до сустава плеча
    float upperArmHalfLength = 0.160f;
    float forearmHalfLength = 0.135f;
    float handHalfLength = 0.092f;  // от запястья до основания пальцев
    float armRadius = 0.045f;       // плечо
    float forearmRadius = 0.036f;
    float wristRadius = 0.026f;
    float palmThickness = 0.016f;   // полутолщина ладони

    // Пальцы: длина фаланг уменьшается от проксимальной к дистальной.
    float fingerProxLen = 0.040f;
    float fingerMidLen = 0.022f;
    float fingerTipLen = 0.018f;
    float fingerRadius = 0.0085f;
    float thumbProxLen = 0.034f;
    float thumbTipLen = 0.024f;
    float thumbRadius = 0.011f;

    // Ноги.
    float thighHalfLength = 0.225f;
    float shinHalfLength = 0.215f;
    float footLength = 0.155f;      // от голеностопа до основания пальцев
    float ankleToGround = 0.075f;   // высота стопы над землёй
    float thighRadius = 0.070f;
    float calfRadius = 0.052f;
    float ankleRadius = 0.030f;
    float legSpacing = 0.095f;      // полуразстояние бёдер

    // Масштаб всех размеров под заданный рост.
    void scaleTo(float newHeight) {
        const float factor = newHeight / height;
        height = newHeight;
        auto apply = [factor](auto& value) { value *= factor; };
        apply(pelvisHalfHeight); apply(abdomenHalfHeight); apply(chestHalfHeight);
        apply(neckHalfHeight); apply(torsoDepth); apply(chestWidth);
        apply(waistWidth); apply(pelvisWidth);
        apply(headRadius); apply(jawHalfHeight);
        apply(clavicleLength); apply(upperArmHalfLength); apply(forearmHalfLength);
        apply(handHalfLength); apply(armRadius); apply(forearmRadius);
        apply(wristRadius); apply(palmThickness);
        apply(fingerProxLen); apply(fingerMidLen); apply(fingerTipLen);
        apply(fingerRadius); apply(thumbProxLen); apply(thumbTipLen); apply(thumbRadius);
        apply(thighHalfLength); apply(shinHalfLength); apply(footLength);
        apply(ankleToGround); apply(thighRadius); apply(calfRadius);
        apply(ankleRadius); apply(legSpacing);
    }
};

// Цвета тканей для вершинной раскраски (skin — тело, clothes — одежда).
struct Palette {
    std::array<float, 3> skin{0.80f, 0.62f, 0.52f};
    std::array<float, 3> lips{0.66f, 0.40f, 0.36f};
    std::array<float, 3> nails{0.88f, 0.78f, 0.74f};
    std::array<float, 3> shirt{0.28f, 0.36f, 0.52f};
    std::array<float, 3> pants{0.20f, 0.20f, 0.24f};
    std::array<float, 3> shoes{0.10f, 0.09f, 0.09f};
};

}  // namespace character
