#include "world/noise.h"

#include <cmath>
#include <utility>

namespace world {

namespace {

// xorshift32 — быстрый детерминированный ГПСЧ. Нужен только для перестановки
// градиентов при инициализации, поэтому Period (2^32 - 1) более чем достаточен.
class XorShift32 {
public:
    explicit XorShift32(std::uint32_t seed)
        : state_(seed != 0u ? seed : 0x9e3779b9u) {}

    std::uint32_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Равномерное значение в [0, bound) для small bound.
    std::uint32_t below(std::uint32_t bound) noexcept {
        return next() % bound;
    }

private:
    std::uint32_t state_;
};

constexpr float kSqrtTwo = 1.4142135623730951f;
constexpr float kInvSqrtTwo = 0.7071067811865475f;
// Теоретический максимум значений шума Перлина при нормированных градиентах:
// sqrt(2)/2 в 2D и sqrt(3)/2 в 3D. Домножаем на обратное, чтобы noise2D и
// noise3D действительно занимали весь [-1, 1].
constexpr float kNormalize2D = kSqrtTwo;
constexpr float kNormalize3D = 1.1547005383792515f;  // 2 / sqrt(3)

// 8 направлений градиентов в 2D: четыре по сторонам квадрата и четыре
// по диагоналям. Длина каждого — 1.
constexpr float kGradients2D[8][2] = {
    {1.0f, 0.0f},          {-1.0f, 0.0f},
    {0.0f, 1.0f},          {0.0f, -1.0f},
    {kSqrtTwo / 2.0f, kSqrtTwo / 2.0f},  {kSqrtTwo / 2.0f, -kSqrtTwo / 2.0f},
    {-kSqrtTwo / 2.0f, kSqrtTwo / 2.0f}, {-kSqrtTwo / 2.0f, -kSqrtTwo / 2.0f},
};

// 12 направлений в 3D: рёбра куба, то есть (±1, ±1, 0) и все перестановки —
// ровно 12 вариантов, длина каждого sqrt(2).
constexpr float kGradients3D[12][3] = {
    {1.0f, 1.0f, 0.0f},   {1.0f, -1.0f, 0.0f},  {-1.0f, 1.0f, 0.0f},  {-1.0f, -1.0f, 0.0f},
    {1.0f, 0.0f, 1.0f},   {1.0f, 0.0f, -1.0f},  {-1.0f, 0.0f, 1.0f},  {-1.0f, 0.0f, -1.0f},
    {0.0f, 1.0f, 1.0f},   {0.0f, 1.0f, -1.0f},  {0.0f, -1.0f, 1.0f},  {0.0f, -1.0f, -1.0f},
};

}  // namespace

PerlinNoise::PerlinNoise(std::uint32_t seed) {
    reseed(seed);
}

void PerlinNoise::reseed(std::uint32_t seed) {
    XorShift32 random(seed);
    for (int i = 0; i < kTableSize; ++i) {
        permutation_[i] = static_cast<std::uint8_t>(i);
    }
    // Тасование Фишера-Йетса: таблица перестановок становится случайной,
    // но одинаковой для одного и того же seed.
    for (int i = kTableSize - 1; i > 0; --i) {
        const int j = static_cast<int>(random.below(static_cast<std::uint32_t>(i + 1)));
        std::swap(permutation_[i], permutation_[j]);
    }
}

float PerlinNoise::noise2D(float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return 0.0f;
    }

    const int cellX = static_cast<int>(std::floor(x));
    const int cellY = static_cast<int>(std::floor(y));
    // Смещение точки внутри ячейки: [0, 1) по каждой оси.
    const float dx = x - static_cast<float>(cellX);
    const float dy = y - static_cast<float>(cellY);
    const float u = fade(dx);
    const float v = fade(dy);

    // Значения в четырёх углах ячейки (градиент · смещение).
    const float n00 = gradient2D(hash(cellX, cellY), dx, dy);
    const float n10 = gradient2D(hash(cellX + 1, cellY), dx - 1.0f, dy);
    const float n01 = gradient2D(hash(cellX, cellY + 1), dx, dy - 1.0f);
    const float n11 = gradient2D(hash(cellX + 1, cellY + 1), dx - 1.0f, dy - 1.0f);

    // Билинейная интерполяция с квинтическим сглаживанием по каждой оси.
    return kNormalize2D * lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

float PerlinNoise::noise3D(float x, float y, float z) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return 0.0f;
    }

    const int cellX = static_cast<int>(std::floor(x));
    const int cellY = static_cast<int>(std::floor(y));
    const int cellZ = static_cast<int>(std::floor(z));
    const float dx = x - static_cast<float>(cellX);
    const float dy = y - static_cast<float>(cellY);
    const float dz = z - static_cast<float>(cellZ);
    const float u = fade(dx);
    const float v = fade(dy);
    const float w = fade(dz);

    // Значения в восьми углах ячейки.
    const float n000 = gradient3D(hash(cellX, cellY, cellZ), dx, dy, dz);
    const float n100 = gradient3D(hash(cellX + 1, cellY, cellZ), dx - 1.0f, dy, dz);
    const float n010 = gradient3D(hash(cellX, cellY + 1, cellZ), dx, dy - 1.0f, dz);
    const float n110 = gradient3D(hash(cellX + 1, cellY + 1, cellZ), dx - 1.0f, dy - 1.0f, dz);
    const float n001 = gradient3D(hash(cellX, cellY, cellZ + 1), dx, dy, dz - 1.0f);
    const float n101 = gradient3D(hash(cellX + 1, cellY, cellZ + 1), dx - 1.0f, dy, dz - 1.0f);
    const float n011 = gradient3D(hash(cellX, cellY + 1, cellZ + 1), dx, dy - 1.0f, dz - 1.0f);
    const float n111 = gradient3D(hash(cellX + 1, cellY + 1, cellZ + 1), dx - 1.0f, dy - 1.0f, dz - 1.0f);

    // Три последовательные билинейные интерполяции (сначала по X, затем Z, Y).
    const float x00 = lerp(n000, n100, u);
    const float x10 = lerp(n010, n110, u);
    const float x01 = lerp(n001, n101, u);
    const float x11 = lerp(n011, n111, u);
    const float plane0 = lerp(x00, x01, w);
    const float plane1 = lerp(x10, x11, w);
    return kNormalize3D * lerp(plane0, plane1, v);
}

float PerlinNoise::fbm2D(float x, float y, int octaves, float lacunarity,
                          float gain) const {
    if (octaves <= 0 || !std::isfinite(lacunarity) || !std::isfinite(gain)) {
        return 0.0f;
    }

    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    // Сумма амплитуд нужна для нормировки: без неё fbm тем больше выходит за
    // [-1, 1], чем больше октав.
    float amplitudeSum = 0.0f;

    for (int octave = 0; octave < octaves; ++octave) {
        sum += amplitude * noise2D(x * frequency, y * frequency);
        amplitudeSum += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }

    if (amplitudeSum <= 0.0f) {
        // Вырожденный случай: gain = 0 убивает все октавы кроме первой, а
        // отрицательная сумма амплитуд означает некорректные параметры.
        return 0.0f;
    }
    return sum / amplitudeSum;
}

float PerlinNoise::fade(float t) noexcept {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float PerlinNoise::lerp(float from, float to, float t) noexcept {
    return from + (to - from) * t;
}

int PerlinNoise::hash(int x, int y) const noexcept {
    return permutation_[(permutation_[x & kTableMask] + y) & kTableMask];
}

int PerlinNoise::hash(int x, int y, int z) const noexcept {
    return permutation_[(permutation_[(permutation_[x & kTableMask] + y) & kTableMask] + z) &
                        kTableMask];
}

float PerlinNoise::gradient2D(int cell, float dx, float dy) const noexcept {
    const float* gradient = kGradients2D[cell & (kGradientCount2D - 1)];
    return gradient[0] * dx + gradient[1] * dy;
}

float PerlinNoise::gradient3D(int cell, float dx, float dy, float dz) const noexcept {
    // cell может быть 0..255, а направлений 12 — приводим по модулю.
    const float* gradient = kGradients3D[cell % kGradientCount3D];
    return (gradient[0] * dx + gradient[1] * dy + gradient[2] * dz) * kInvSqrtTwo;
}

}  // namespace world
