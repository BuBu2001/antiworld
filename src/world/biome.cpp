#include "world/biome.h"

#include <algorithm>
#include <cmath>

namespace world {

namespace {

// Плавный переход 0 -> 1 на отрезке [edge0, edge1] (smoothstep). Если edge1 < edge0,
// переход убывает: значение выше edge0 даёт 0, ниже edge1 — 1. Один этот
// помощник покрывает и «холоднее — значит тундра», и «влажнее — значит лес».
float ramp(float value, float edge0, float edge1) {
    if (edge1 == edge0) {
        // Вырожденный интервал: ступенька, чтобы не делить на ноль.
        return value < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

// --- BiomeBlend ---

float BiomeBlend::weight(Biome biome) const {
    return weights_[static_cast<std::size_t>(biome)];
}

void BiomeBlend::setWeight(Biome biome, float value) {
    weights_[static_cast<std::size_t>(biome)] = value;
}

Biome BiomeBlend::dominant() const {
    return static_cast<Biome>(
        static_cast<std::size_t>(std::max_element(weights_, weights_ + kBiomeCount) -
                                 weights_));
}

glm::vec3 BiomeBlend::color() const {
    glm::vec3 sum{0.0f};
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        sum += weights_[index] * biomeColor(static_cast<Biome>(index));
    }
    return sum;
}

// --- Классификация ---

Biome getBiome(float height, float temperature, float humidity, const BiomeParams& params) {
    return sampleBiome(height, temperature, humidity, params).dominant();
}

BiomeBlend sampleBiome(float height, float temperature, float humidity,
                       const BiomeParams& params) {
    // Каждый признак — независимый гладкий индикатор в [0, 1], где 1 означает
    // «признак выражен сильнее всего».
    const float altitude = ramp(height, params.mountainStart, params.mountainEnd);
    const float cold = ramp(temperature, params.tundraWarm, params.tundraCold);
    const float heat = ramp(temperature, params.desertCool, params.desertHot);
    const float dry = ramp(humidity, params.humidityMoist, params.humidityDry);

    // Горы отнимают площадь у всех остальных биомов: выше снеговой линии
    // высокая температура не превращает склон в пустыню.
    const float lowland = 1.0f - altitude;

    // Сырые веса: они не нормированы и могут превышать 1 — это и есть
    // «конкуренция» биомов за одно место.
    //
    // Ключевое решение: лес определяется не жарой, а отсутствием холода и
    // сухости. Иначе в жарком влажном климате (heat = 1) множитель (1 - heat)
    // обнулял бы лес, и такой регион целиком становился пустыней — что неверно.
    // Жара входит только в пустыню и только вместе с сухостью: жарко и сухо —
    // пустыня, жарко и влажно — лес.
    float raw[kBiomeCount] = {
        lowland * heat * dry,             // Desert
        lowland * ((1.0f - cold) * (1.0f - dry) + params.forestFloor),  // Forest
        lowland * cold,                  // Tundra
        altitude,                        // Mountains
    };

    float total = 0.0f;
    for (const float weight : raw) {
        total += weight;
    }
    // Сумма не может быть нулевой: горы дают 1 на вершинах, лес — минимум
    // forestFloor в низинах. Но подстраховка от деления на ноль не лишняя.
    if (!(total > 0.0f)) {
        raw[static_cast<std::size_t>(Biome::Forest)] = 1.0f;
        total = 1.0f;
    }

    BiomeBlend blend;
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        blend.setWeight(static_cast<Biome>(index), raw[index] / total);
    }

    // Снежность: в основном холод, частично высота. Не зависит от сезона —
    // сезон подмешивает шейдер, поэтому смена времени года не требует
    // перестройки mesh.
    const float frost = ramp(temperature, params.snowMelt, params.snowFreeze);
    blend.setSnowBias(
        std::clamp(params.snowFromCold * frost + params.mountainSnow * altitude, 0.0f, 1.0f));

    return blend;
}

void fitBiomeHeights(BiomeParams& params, float minHeight, float maxHeight) {
    if (!std::isfinite(minHeight) || !std::isfinite(maxHeight) || maxHeight <= minHeight) {
        // Плоская или некорректная карта высот: оставляем абсолютные пороги как
        // были, гор просто не появятся — это честнее, чем горы из шума.
        return;
    }
    const float range = maxHeight - minHeight;
    params.mountainStart = minHeight + params.mountainStartFraction * range;
    params.mountainEnd = minHeight + params.mountainEndFraction * range;
    if (params.mountainEnd <= params.mountainStart) {
        // Доли заданы не по возрастанию: делим интервал пополам, чтобы
        // переход всё равно оставался гладким и невырожденным.
        const float middle = minHeight + 0.5f * range;
        params.mountainStart = middle;
        params.mountainEnd = middle + 0.05f * range;
    }
}

glm::vec3 biomeColor(Biome biome) {
    switch (biome) {
        // Песок с охрой — сухой, яркий, сильно отражает свет.
        case Biome::Desert:
            return glm::vec3{0.86f, 0.74f, 0.47f};
        // Тёмная хвоя: альбедо низкое, поэтому лес визуально «тяжелее» пустыни.
        case Biome::Forest:
            return glm::vec3{0.20f, 0.42f, 0.18f};
        // Тундра — не белая: это вытоптанный мох и лишайник, буро-зелёный.
        // Белым её делает только снег (см. snowBias и шейдер).
        case Biome::Tundra:
            return glm::vec3{0.42f, 0.42f, 0.31f};
        // Голый камень: серый с холодным оттенком, чтобы читалась порода.
        case Biome::Mountains:
            return glm::vec3{0.45f, 0.44f, 0.46f};
    }
    return glm::vec3{1.0f};
}

const char* biomeName(Biome biome) {
    switch (biome) {
        case Biome::Desert:
            return "пустыня";
        case Biome::Forest:
            return "лес";
        case Biome::Tundra:
            return "тундра";
        case Biome::Mountains:
            return "горы";
    }
    return "неизвестно";
}

}  // namespace world
