#include "character/character_mesh.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_inverse.hpp>

namespace character {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Сторона конечности по кости: левые кости — +X (персонаж лицом к -Z).
float sideOf(Bone bone) {
    switch (bone) {
        case Bone::ShoulderL: case Bone::UpperArmL: case Bone::ForearmL: case Bone::HandL:
        case Bone::IndexProxL: case Bone::MiddleProxL: case Bone::RingProxL:
        case Bone::LittleProxL: case Bone::ThumbMetaL:
        case Bone::UpperLegL: case Bone::LowerLegL: case Bone::FootL: case Bone::ToesL:
            return 1.0f;
        default:
            return -1.0f;
    }
}

// Строитель меша: параметрические примитивы в мировых координатах bind-pose.
// Персонаж стоит в начале координат, лицом к -Z (как камера по умолчанию).
class Builder {
public:
    CharacterMeshGenerator::SkinnedModel model;

    void setBone(Bone bone) { currentBone_ = boneIndex(bone); }
    void setColor(const std::array<float, 3>& rgb) { color_ = rgb; }

    // Трубка между двумя точками с интерполяцией радиуса. Шапки не строим —
    // стыки перекрываются соседними сегментами и суставами-сферами.
    void tube(const glm::vec3& from, const glm::vec3& to, float r0, float r1, int rings) {
        const glm::vec3 axis = to - from;
        const float length = glm::length(axis);
        if (length <= 1e-6f || r0 <= 0.0f || r1 <= 0.0f) return;

        const glm::quat rot = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), axis / length);
        const uint32_t base = static_cast<uint32_t>(model.vertices.size());
        constexpr int kSlices = 12;
        const int steps = std::max(rings, 1);
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const glm::vec3 center = from + axis * t;
            const float radius = glm::mix(r0, r1, t);
            for (int s = 0; s < kSlices; ++s) {
                const float angle = 2.0f * kPi * static_cast<float>(s) / kSlices;
                const glm::vec3 offset =
                    rot * glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
                pushVertex(center + offset, glm::normalize(offset));
            }
        }
        for (int i = 0; i < steps; ++i) {
            for (int s = 0; s < kSlices; ++s) {
                const uint32_t a = base + i * kSlices + s;
                const uint32_t b = base + i * kSlices + (s + 1) % kSlices;
                const uint32_t c = a + kSlices;
                const uint32_t d = b + kSlices;
                model.indices.insert(model.indices.end(), {a, b, c, b, d, c});
            }
        }
    }

    // Сфера/эллипсоид с произвольной ориентацией (голова, суставы, ногти).
    void ellipsoid(const glm::vec3& center, const glm::vec3& radii, const glm::quat& rot,
                   int stacks = 8, int slices = 14) {
        if (radii.x <= 0.0f || radii.y <= 0.0f || radii.z <= 0.0f) return;
        const uint32_t base = static_cast<uint32_t>(model.vertices.size());
        for (int i = 0; i <= stacks; ++i) {
            const float phi = kPi * static_cast<float>(i) / stacks;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);
            for (int j = 0; j < slices; ++j) {
                const float theta = 2.0f * kPi * static_cast<float>(j) / slices;
                const glm::vec3 unit(sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta));
                const glm::vec3 grad(unit.x / (radii.x * radii.x), unit.y / (radii.y * radii.y),
                                     unit.z / (radii.z * radii.z));
                pushVertex(center + rot * (unit * radii), glm::normalize(rot * grad));
            }
        }
        for (int i = 0; i < stacks; ++i) {
            for (int j = 0; j < slices; ++j) {
                const uint32_t nw = base + i * slices + j;
                const uint32_t ne = base + i * slices + (j + 1) % slices;
                const uint32_t sw = nw + slices;
                const uint32_t se = ne + slices;
                if (i != 0) model.indices.insert(model.indices.end(), {nw, sw, ne});
                if (i != stacks - 1) model.indices.insert(model.indices.end(), {ne, sw, se});
            }
        }
    }

    // Скруглённый бокс: 8 углов с нормалью «от центра» (soft shading).
    void roundedBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rot) {
        const glm::vec3 he = halfExtents;
        auto corner = [&](int sx, int sy, int sz) {
            return center + rot * glm::vec3(sx * he.x, sy * he.y, sz * he.z);
        };
        auto normalOf = [&](const glm::vec3& p) {
            const glm::vec3 local = glm::inverse(rot) * (p - center);
            return glm::normalize(rot * glm::vec3(local.x / he.x, local.y / he.y, local.z / he.z));
        };
        const glm::vec3 v[8] = {corner(-1,-1,-1), corner(1,-1,-1), corner(1,-1,1), corner(-1,-1,1),
                                corner(-1,1,-1),  corner(1,1,-1),  corner(1,1,1),  corner(-1,1,1)};
        const uint32_t b = static_cast<uint32_t>(model.vertices.size());
        for (const glm::vec3& p : v) pushVertex(p, normalOf(p));
        const uint32_t faces[6][4] = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
        for (const auto& f : faces) {
            model.indices.insert(model.indices.end(),
                                 {b+f[0], b+f[2], b+f[1], b+f[0], b+f[3], b+f[2]});
        }
    }

private:
    int currentBone_{0};
    std::array<float, 3> color_{0.8f, 0.6f, 0.5f};

    void pushVertex(const glm::vec3& position, const glm::vec3& normal) {
        CharacterMeshGenerator::SkinVertex vertex{};
        vertex.vertex.position = {position.x, position.y, position.z};
        vertex.vertex.normal = {normal.x, normal.y, normal.z};
        vertex.vertex.color = color_;
        vertex.bone0 = currentBone_;
        vertex.weight = 1.0f;
        model.vertices.push_back(vertex);
    }
};

// Локальный репер кости в bind-pose: point(x,y,z) — смещение относительно
// сустава кости (Y вниз вдоль сегмента, X вбок, Z назад/вперёд относительно
// самой кости: локальная -Z у головы и кистей смотрит «в лицо»/«в ладонь»).
struct BoneFrame {
    glm::mat4 world{1.0f};

    static BoneFrame of(const Skeleton& skeleton, Bone bone) {
        BoneFrame frame;
        frame.world = skeleton.bindMatrix(bone);
        return frame;
    }

    glm::vec3 point(float x, float y, float z) const {
        return glm::vec3(world * glm::vec4(x, y, z, 1.0f));
    }
    glm::vec3 direction(float x, float y, float z) const {
        return glm::normalize(glm::vec3(world * glm::vec4(x, y, z, 0.0f)));
    }
    glm::quat rotation() const { return glm::quat_cast(world); }
};

void buildTorso(Builder& b, const Skeleton& sk, const Palette& pal) {
    const BodyDimensions& d = sk.dimensions();
    b.setColor(pal.skin);

    // Таз: широкий скруглённый бокс.
    b.setBone(Bone::Hips);
    {
        const BoneFrame hips = BoneFrame::of(sk, Bone::Hips);
        b.roundedBox(hips.point(0.0f, 0.015f, 0.0f),
                     {d.pelvisWidth, d.pelvisHalfHeight, d.torsoDepth * 0.92f}, hips.rotation());
    }

    // Живот: таз → грудь с расширением к грудной клетке.
    b.setBone(Bone::Spine);
    {
        const BoneFrame spine = BoneFrame::of(sk, Bone::Spine);
        const BoneFrame chest = BoneFrame::of(sk, Bone::Chest);
        b.tube(spine.point(0.0f, 0.02f, 0.0f), chest.point(0.0f, 0.02f, 0.0f),
               d.waistWidth, d.chestWidth * 0.95f, 5);
    }

    // Грудная клетка + грудные мышцы + дельты.
    b.setBone(Bone::Chest);
    {
        const BoneFrame chest = BoneFrame::of(sk, Bone::Chest);
        b.ellipsoid(chest.point(0.0f, -d.chestHalfHeight * 0.30f, 0.0f),
                    {d.chestWidth, d.chestHalfHeight * 1.15f, d.torsoDepth * 1.05f},
                    chest.rotation(), 9, 16);
        for (float sx : {1.0f, -1.0f}) {
            b.ellipsoid(chest.point(sx * d.chestWidth * 0.45f, -d.chestHalfHeight * 0.20f,
                                    d.torsoDepth * 0.90f),
                        {d.chestWidth * 0.42f, d.chestHalfHeight * 0.55f, 0.035f},
                        chest.rotation(), 6, 10);
        }
        for (int side = 0; side < 2; ++side) {
            const HandBones hand = side == 0 ? handLeft() : handRight();
            const BoneFrame shoulder = BoneFrame::of(sk, hand.shoulder);
            b.ellipsoid(shoulder.point(0.0f, 0.02f, 0.0f),
                        {0.062f, 0.075f, 0.062f}, shoulder.rotation(), 6, 10);
        }
    }

    // Шея.
    b.setBone(Bone::Neck);
    {
        const BoneFrame neck = BoneFrame::of(sk, Bone::Neck);
        const BoneFrame head = BoneFrame::of(sk, Bone::Head);
        b.tube(neck.point(0.0f, 0.02f, -0.005f), head.point(0.0f, 0.03f, -0.01f),
               0.052f, 0.046f, 3);
    }
}

// Лицо смотрит в мировую -Z (совпадает с локальной -Z головы в bind-pose).
void buildHead(Builder& b, const Skeleton& sk, const Palette& pal) {
    const BodyDimensions& d = sk.dimensions();
    const BoneFrame head = BoneFrame::of(sk, Bone::Head);
    const float fz = -1.0f;  // множитель «вперёд» для лица

    b.setBone(Bone::Head);
    b.setColor(pal.skin);
    // Череп.
    b.ellipsoid(head.point(0.0f, 0.012f, 0.0f),
                {d.headRadius * 0.94f, d.headRadius * d.headElongation, d.headRadius * 1.02f},
                head.rotation(), 12, 18);
    // Лицевая треть: выступает вперёд.
    b.ellipsoid(head.point(0.0f, -d.headRadius * 0.20f, fz * d.headRadius * 0.42f),
                {d.headRadius * 0.72f, d.headRadius * 0.62f, d.headRadius * 0.55f},
                head.rotation(), 8, 14);
    // Лоб и скулы.
    b.ellipsoid(head.point(0.0f, d.headRadius * 0.40f, fz * d.headRadius * 0.60f),
                {d.headRadius * 0.70f, d.headRadius * 0.34f, d.headRadius * 0.30f},
                head.rotation(), 6, 10);
    for (float sx : {1.0f, -1.0f}) {
        b.ellipsoid(head.point(sx * d.headRadius * 0.55f, -d.headRadius * 0.10f,
                               fz * d.headRadius * 0.66f),
                    {0.028f, 0.022f, 0.020f}, head.rotation(), 5, 8);
    }

    // Нос: переносица, кончик, крылья.
    b.tube(head.point(0.0f, d.headRadius * 0.10f, fz * d.headRadius * 0.92f),
           head.point(0.0f, -d.headRadius * 0.30f, fz * d.headRadius * 1.16f),
           0.012f, 0.017f, 3);
    b.ellipsoid(head.point(0.0f, -d.headRadius * 0.30f, fz * d.headRadius * 1.12f),
                {0.020f, 0.014f, 0.016f}, head.rotation(), 5, 8);
    for (float sx : {1.0f, -1.0f}) {
        b.ellipsoid(head.point(sx * 0.017f, -d.headRadius * 0.33f, fz * d.headRadius * 1.02f),
                    {0.009f, 0.008f, 0.009f}, head.rotation(), 4, 6);
    }

    // Глаза: белок, радужка, зрачок, веко, бровь.
    for (float sx : {1.0f, -1.0f}) {
        const glm::vec3 socket = head.point(sx * d.headRadius * 0.36f, d.headRadius * 0.06f,
                                            fz * d.headRadius * 0.78f);
        b.setColor({0.92f, 0.92f, 0.90f});
        b.ellipsoid(socket, {0.0135f, 0.0125f, 0.0125f}, head.rotation(), 6, 10);
        b.setColor({0.24f, 0.17f, 0.11f});
        b.ellipsoid(socket + head.direction(0.0f, 0.0f, fz * 0.011f),
                    {0.0072f, 0.0072f, 0.006f}, head.rotation(), 5, 8);
        b.setColor({0.05f, 0.04f, 0.04f});
        b.ellipsoid(socket + head.direction(0.0f, 0.0f, fz * 0.016f),
                    {0.0032f, 0.0032f, 0.0025f}, head.rotation(), 4, 6);
        b.setColor(pal.skin);
        b.ellipsoid(socket + head.direction(0.0f, 0.010f, fz * 0.006f),
                    {0.016f, 0.006f, 0.012f}, head.rotation(), 4, 8);
        b.setColor({0.20f, 0.14f, 0.10f});
        b.ellipsoid(socket + head.direction(0.0f, 0.030f, fz * 0.008f),
                    {0.020f, 0.0045f, 0.006f}, head.rotation(), 4, 8);
    }

    // Уши (сзади-внешняя сторона головы).
    b.setColor(pal.skin);
    for (float sx : {1.0f, -1.0f}) {
        b.ellipsoid(head.point(sx * d.headRadius * 0.95f, -d.headRadius * 0.05f,
                               d.headRadius * 0.10f),
                    {0.008f, 0.030f, 0.020f}, head.rotation(), 5, 8);
        b.ellipsoid(head.point(sx * d.headRadius * 0.93f, -d.headRadius * 0.34f,
                               d.headRadius * 0.06f),
                    {0.008f, 0.012f, 0.010f}, head.rotation(), 4, 6);
    }

    // Волосы: шапка, чуть больше черепа, сдвинута вверх-назад.
    b.setColor({0.15f, 0.10f, 0.07f});
    b.ellipsoid(head.point(0.0f, d.headRadius * 0.28f, d.headRadius * 0.06f),
                {d.headRadius * 1.02f, d.headRadius * 0.92f, d.headRadius * 1.06f},
                head.rotation(), 8, 14);

    // Челюсть: нижняя треть лица, подбородок, губы, рот.
    b.setBone(Bone::Jaw);
    b.setColor(pal.skin);
    {
        const BoneFrame jaw = BoneFrame::of(sk, Bone::Jaw);
        b.ellipsoid(jaw.point(0.0f, -d.jawHalfHeight * 0.5f, fz * d.headRadius * 0.26f),
                    {d.headRadius * 0.76f, d.jawHalfHeight, d.headRadius * 0.64f},
                    jaw.rotation(), 6, 12);
        b.ellipsoid(jaw.point(0.0f, -d.jawHalfHeight * 0.9f, fz * d.headRadius * 0.66f),
                    {0.024f, 0.018f, 0.020f}, jaw.rotation(), 5, 8);
        b.setColor(pal.lips);
        b.ellipsoid(jaw.point(0.0f, -d.jawHalfHeight * 0.10f, fz * d.headRadius * 0.80f),
                    {0.026f, 0.007f, 0.010f}, jaw.rotation(), 4, 8);
        b.ellipsoid(jaw.point(0.0f, -d.jawHalfHeight * 0.55f, fz * d.headRadius * 0.78f),
                    {0.024f, 0.009f, 0.011f}, jaw.rotation(), 4, 8);
        b.setColor({0.25f, 0.10f, 0.09f});
        b.ellipsoid(jaw.point(0.0f, -d.jawHalfHeight * 0.32f, fz * d.headRadius * 0.84f),
                    {0.020f, 0.0035f, 0.006f}, jaw.rotation(), 3, 8);
    }
}

void buildArm(Builder& b, const Skeleton& sk, const Palette& pal, const HandBones& hand) {
    const BodyDimensions& d = sk.dimensions();

    // Плечо: сустав + рукав до локтя.
    b.setBone(hand.upperArm);
    b.setColor(pal.shirt);
    {
        const BoneFrame ua = BoneFrame::of(sk, hand.upperArm);
        b.ellipsoid(ua.point(0.0f, 0.01f, 0.0f), {d.armRadius, d.armRadius, d.armRadius},
                    ua.rotation(), 5, 10);
        b.tube(ua.point(0.0f, 0.0f, 0.0f), ua.point(0.0f, -d.upperArmHalfLength * 2.0f, 0.0f),
               d.armRadius, d.forearmRadius * 1.05f, 6);
    }

    // Предплечье: локтевой сустав, рукав до середины, дальше кожа.
    b.setBone(hand.forearm);
    {
        const BoneFrame fa = BoneFrame::of(sk, hand.forearm);
        b.setColor(pal.skin);
        b.ellipsoid(fa.point(0.0f, 0.0f, 0.0f),
                    {d.forearmRadius * 1.1f, d.forearmRadius * 1.05f, d.forearmRadius * 1.05f},
                    fa.rotation(), 5, 10);
        b.setColor(pal.shirt);
        b.tube(fa.point(0.0f, 0.0f, 0.0f), fa.point(0.0f, -d.forearmHalfLength * 0.9f, 0.0f),
               d.forearmRadius * 1.02f, d.forearmRadius * 0.9f, 3);
        b.setColor(pal.skin);
        b.tube(fa.point(0.0f, -d.forearmHalfLength * 0.9f, 0.0f),
               fa.point(0.0f, -d.forearmHalfLength * 2.0f, 0.0f),
               d.forearmRadius * 0.9f, d.wristRadius, 3);
    }

    // Ладонь: пластина с софт-нормалями.
    b.setBone(hand.hand);
    b.setColor(pal.skin);
    {
        const BoneFrame h = BoneFrame::of(sk, hand.hand);
        b.roundedBox(h.point(0.0f, -d.handHalfLength * 0.50f, 0.0f),
                     {d.wristRadius * 1.45f, d.handHalfLength * 0.55f, d.palmThickness},
                     h.rotation());
    }

    // Четыре длинных пальца: три секции + ноготь, естественный полусогнут.
    const int order[4] = {kIndex, kMiddle, kRing, kLittle};
    const float lengthScale[4] = {0.94f, 1.0f, 0.93f, 0.78f};
    const float curl = 0.25f;  // радианы покоящегося сгиба MCP
    for (int f = 0; f < 4; ++f) {
        const FingerBones& fb = hand.fingers[order[f]];
        const float ls = lengthScale[f];
        b.setColor(pal.skin);

        b.setBone(fb.prox);
        {
            const BoneFrame p = BoneFrame::of(sk, fb.prox);
            const glm::vec3 start = p.point(0.0f, 0.002f, 0.0f);
            const glm::vec3 mid = start +
                                  p.direction(0.0f, -std::cos(curl), std::sin(curl)) *
                                      d.fingerProxLen * ls;
            b.tube(start, mid, d.fingerRadius * 1.05f, d.fingerRadius * 0.95f, 2);
            b.ellipsoid(mid, glm::vec3(d.fingerRadius), p.rotation(), 4, 8);

            b.setBone(fb.mid);
            const BoneFrame m = BoneFrame::of(sk, fb.mid);
            const glm::vec3 mStart = m.point(0.0f, 0.0f, 0.0f);
            const glm::vec3 tip = mStart +
                                  m.direction(0.0f, -std::cos(curl * 1.5f), std::sin(curl * 1.5f)) *
                                      d.fingerMidLen * ls;
            b.tube(mStart, tip, d.fingerRadius * 0.95f, d.fingerRadius * 0.85f, 2);
            b.ellipsoid(tip, glm::vec3(d.fingerRadius * 0.9f), m.rotation(), 4, 8);

            b.setBone(fb.tip);
            const BoneFrame t = BoneFrame::of(sk, fb.tip);
            const glm::vec3 end = t.point(0.0f, -d.fingerTipLen * ls, 0.0f);
            b.tube(t.point(0.0f, 0.0f, 0.0f), end, d.fingerRadius * 0.85f,
                   d.fingerRadius * 0.55f, 2);
            b.ellipsoid(end, glm::vec3(d.fingerRadius * 0.7f), t.rotation(), 4, 8);
            // Ноготь на тыльной стороне кончика.
            b.setColor(pal.nails);
            b.ellipsoid(end + t.direction(0.0f, -0.002f, d.fingerRadius * 0.75f),
                        {d.fingerRadius * 0.62f, d.fingerTipLen * 0.42f, d.fingerRadius * 0.30f},
                        t.rotation(), 4, 6);
            b.setColor(pal.skin);
        }
    }

    // Большой палец: пястная + две фаланги + ноготь.
    {
        const FingerBones& thumb = hand.fingers[kThumb];
        b.setColor(pal.skin);
        b.setBone(thumb.prox);
        {
            const BoneFrame p = BoneFrame::of(sk, thumb.prox);
            b.tube(p.point(0.0f, 0.0f, 0.0f), p.point(0.0f, -d.thumbProxLen * 0.75f, 0.0f),
                   d.thumbRadius, d.thumbRadius * 0.9f, 2);
            b.setBone(thumb.mid);
            const BoneFrame m = BoneFrame::of(sk, thumb.mid);
            b.ellipsoid(m.point(0.0f, 0.0f, 0.0f), glm::vec3(d.thumbRadius), m.rotation(), 4, 8);
            b.tube(m.point(0.0f, 0.0f, 0.0f), m.point(0.0f, -d.thumbProxLen * 0.55f, 0.0f),
                   d.thumbRadius * 0.9f, d.thumbRadius * 0.8f, 2);
            b.setBone(thumb.tip);
            const BoneFrame t = BoneFrame::of(sk, thumb.tip);
            const glm::vec3 end = t.point(0.0f, -d.thumbTipLen, 0.0f);
            b.tube(t.point(0.0f, 0.0f, 0.0f), end, d.thumbRadius * 0.8f,
                   d.thumbRadius * 0.5f, 2);
            b.ellipsoid(end, glm::vec3(d.thumbRadius * 0.62f), t.rotation(), 4, 8);
            b.setColor(pal.nails);
            b.ellipsoid(end + t.direction(0.0f, -0.002f, d.thumbRadius * 0.6f),
                        {d.thumbRadius * 0.55f, d.thumbTipLen * 0.4f, d.thumbRadius * 0.28f},
                        t.rotation(), 4, 6);
        }
    }
}

void buildLeg(Builder& b, const Skeleton& sk, const Palette& pal, Bone upperLeg, Bone lowerLeg,
              Bone foot, Bone toes) {
    const BodyDimensions& d = sk.dimensions();

    // Бедро: тазобедренный сустав + штанина.
    b.setBone(upperLeg);
    b.setColor(pal.pants);
    {
        const BoneFrame u = BoneFrame::of(sk, upperLeg);
        b.ellipsoid(u.point(0.0f, 0.01f, 0.0f), {d.thighRadius, d.thighRadius, d.thighRadius},
                    u.rotation(), 5, 10);
        b.tube(u.point(0.0f, 0.0f, 0.0f), u.point(0.0f, -d.thighHalfLength * 2.0f, 0.0f),
               d.thighRadius, d.calfRadius * 1.15f, 7);
    }

    // Колено + голень (штанина до середины икры, ниже — ботинок).
    b.setBone(lowerLeg);
    {
        const BoneFrame l = BoneFrame::of(sk, lowerLeg);
        b.setColor(pal.pants);
        b.ellipsoid(l.point(0.0f, 0.0f, 0.008f),
                    {d.calfRadius * 1.1f, d.calfRadius * 1.05f, d.calfRadius * 1.05f},
                    l.rotation(), 5, 10);
        b.tube(l.point(0.0f, 0.0f, 0.0f), l.point(0.0f, -d.shinHalfLength * 1.2f, 0.0f),
               d.calfRadius * 1.1f, d.calfRadius * 0.85f, 4);
        b.setColor(pal.shoes);
        b.tube(l.point(0.0f, -d.shinHalfLength * 1.2f, 0.0f),
               l.point(0.0f, -d.shinHalfLength * 2.0f, 0.0f),
               d.calfRadius * 0.85f, d.ankleRadius, 3);
    }

    // Стопа: локальная -Y голеностопа направлена вперёд (к носку), поэтому
    // «вниз к земле» здесь — это -Y стопы в мировых координатах уже учтён
    // матрицей bind-pose; платформа ботинка идёт вдоль локальной -Y.
    b.setBone(foot);
    b.setColor(pal.shoes);
    {
        const BoneFrame f = BoneFrame::of(sk, foot);
        b.roundedBox(f.point(0.0f, -d.footLength * 0.42f, -0.012f),
                     {0.046f, d.footLength * 0.50f, 0.034f}, f.rotation());
        // Пятка.
        b.ellipsoid(f.point(0.0f, 0.005f, 0.022f), {0.040f, 0.045f, 0.042f}, f.rotation(), 5, 8);
    }

    // Пальцы ног: большой + четыре бугорка (под ботинком, но форма есть).
    b.setBone(toes);
    {
        const BoneFrame t = BoneFrame::of(sk, toes);
        const float st = sideOf(toes);
        b.ellipsoid(t.point(st * 0.028f, -0.012f, 0.0f),
                    {0.019f, 0.016f, 0.026f}, t.rotation(), 4, 8);
        for (int toe = 0; toe < 4; ++toe) {
            const float offset = -st * (0.004f + static_cast<float>(toe) * 0.019f);
            const float size = 0.014f - static_cast<float>(toe) * 0.002f;
            b.ellipsoid(t.point(offset, -0.008f, 0.0f),
                        {size, size * 0.9f, size * 1.15f}, t.rotation(), 4, 6);
        }
    }
}

}  // namespace

CharacterMeshGenerator::SkinnedModel CharacterMeshGenerator::generate(
    const Skeleton& skeleton, const Palette& palette) {
    Builder builder;
    for (int i = 0; i < Skeleton::kBoneCount; ++i) {
        builder.model.inverseBind[i] =
            glm::affineInverse(skeleton.bindMatrix(static_cast<Bone>(i)));
    }

    buildTorso(builder, skeleton, palette);
    buildHead(builder, skeleton, palette);
    buildArm(builder, skeleton, palette, handLeft());
    buildArm(builder, skeleton, palette, handRight());
    buildLeg(builder, skeleton, palette, Bone::UpperLegL, Bone::LowerLegL, Bone::FootL, Bone::ToesL);
    buildLeg(builder, skeleton, palette, Bone::UpperLegR, Bone::LowerLegR, Bone::FootR, Bone::ToesR);

    return std::move(builder.model);
}

renderer::ModelData CharacterMeshGenerator::generateStatic(const Skeleton& skeleton,
                                                           const Palette& palette) {
    SkinnedModel skinned = generate(skeleton, palette);
    renderer::ModelData data;
    data.vertices.reserve(skinned.vertices.size());
    for (const auto& sv : skinned.vertices) data.vertices.push_back(sv.vertex);
    data.indices = std::move(skinned.indices);
    return data;
}

}  // namespace character
