#version 450

// Фрагментный шейдер: альбедо цвета биома, снежный покров по сезону,
// океанская гладь по географии мира и направленное освещение от солнца.
//
// Снег — единственное, что меняется во времени года без перестройки mesh:
// вершина несёт постоянную «склонность к снегу» (snowBias, 0..1), а кадр —
// текущую морозность сезона (frost, 0..1). Снег лежит там, где их сумма
// превышает 1: у тундры bias около 0.85, поэтому она белая с середины осени
// до середины весны, а у пустыни bias = 0 и снега не бывает вовсе.
//
// Вода — часть ГЕОГРАФИИ: вершина несёт флаг water (1 — под уровнем моря),
// а уровень моря приходит из UBO. Там, где terrain ниже уровня моря, поверх
// рисуется водная гладь ровно на seaLevel: с волнами, прозрачностью над
// мелководьем и отражением неба. Суша под водой остаётся видна сквозь
// тонкий слой — как в реальном море у берега.

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragAlbedo;
layout(location = 2) in float fragSnowBias;
layout(location = 3) in float fragWater;
layout(location = 4) in vec3 fragWorldPosition;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    vec4 sunDirection;
    vec4 environment;
    vec4 waterFlags;
} ubo;

layout(location = 0) out vec4 outColor;

// Цвет свежего снега: чуть голубой в тени, поэтому ночью снег не выглядит
// серым. Снег почти не поглощает свет — отражение на грани светах.
const vec3 kSnowColor = vec3(0.90f, 0.93f, 0.98f);

// Цвет глубокой воды и цвет мелководья (у берега видно дно).
const vec3 kDeepWaterColor = vec3(0.05f, 0.18f, 0.34f);
const vec3 kShallowWaterColor = vec3(0.16f, 0.42f, 0.52f);

void main() {
    const vec3 normal = normalize(fragNormal);
    const vec3 sunDirection = normalize(ubo.sunDirection.xyz);
    const float frost = ubo.environment.x;
    const float ambient = ubo.environment.y;
    const float seaLevel = ubo.environment.z;
    const float hasWater = ubo.waterFlags.x;

    // Порог снега: frost + snowBias > 1. Полоса перехода 0.25 по frost
    // означает, что снег появляется и тает постепенно, а не скачком.
    const float cold = frost + fragSnowBias - 1.0f;
    const float snow = smoothstep(0.0f, 0.25f, cold);

    // Снег не держится на отвесных скалах, поэтому на крутых склонах видно
    // камень даже зимой. normal.y = 1 на горизонтали, 0 на вертикали.
    const float flatness = normal.y * normal.y;
    const float cover = snow * mix(0.25f, 1.0f, flatness);

    vec3 albedo = mix(fragAlbedo, kSnowColor, cover);

    // Ламбертово освещение: у ландшафта без теней от карты, поэтому одного
    // направленного света и ambient достаточно.
    const float diffuse = max(dot(normal, sunDirection), 0.0f);
    vec3 lit = albedo * (ambient + diffuse * ubo.sunDirection.w);

    // --- Океан ---
    // Гладь только там, где география говорит «вода» И рельеф реально ниже
    // уровня моря (оба условия нужны: флаг задаёт биом-океан, а высота —
    // корректную линию берега против плавных переходов смеси биомов).
    if (hasWater > 0.5 && fragWater > 0.5 && fragWorldPosition.y < seaLevel) {
        // Глубина в точке: 0 у берега, 1 на максимальной глубине океана.
        const float depth = clamp((seaLevel - fragWorldPosition.y) / 12.0f, 0.0f, 1.0f);

        // Волны: две синусоидальные ряби в мировых координатах + нормаль,
        // наклонённая по их градиенту. Это дешёвая имитация, но вода сразу
        // перестаёт выглядеть плоским полигоном.
        const vec2 p = fragWorldPosition.xz;
        const float waveA = sin(p.x * 0.12 + p.y * 0.07);
        const float waveB = sin(p.x * 0.05 - p.y * 0.11);
        const vec3 waveNormal = normalize(vec3(
            0.06 * cos(p.x * 0.12 + p.y * 0.07) * 0.12 +
            0.04 * cos(p.x * 0.05 - p.y * 0.11) * 0.05,
            1.0,
            0.06 * cos(p.x * 0.12 + p.y * 0.07) * 0.07 -
            0.04 * cos(p.x * 0.05 - p.y * 0.11) * 0.11));

        const vec3 waterColor = mix(kShallowWaterColor, kDeepWaterColor, depth);
        // Блик: specular от солнца по нормали волн даёт живую поверхность.
        const float glint = pow(max(dot(reflect(-sunDirection, waveNormal),
                                        vec3(0.0, 1.0, 0.0)), 0.0f), 24.0f);
        const float skyLike = ambient + 0.35 * ubo.sunDirection.w;
        vec3 water = waterColor * skyLike + vec3(0.9f, 0.95f, 1.0f) * glint *
                     ubo.sunDirection.w;
        // Пена у самого берега.
        const float shore = 1.0f - smoothstep(0.0f, 1.2f, seaLevel - fragWorldPosition.y);
        water = mix(water, vec3(0.85f, 0.9f, 0.95f), shore * 0.35f);

        // Смешивание с дном: чем мельче, тем сильнее видно сушу под водой.
        const float opacity = mix(0.45f, 0.92f, depth);
        lit = mix(lit, water, opacity);
    }

    outColor = vec4(lit, 1.0f);
}
