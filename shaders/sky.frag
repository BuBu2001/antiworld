#version 450

// Фрагментный шейдер неба: атмосферное рассеяние, солнце, луна, звёзды и
// два слоя облаков. Вся математика — в sky_common.glsl, тот же файл
// подключается в triangle.frag, чтобы дымка у горизонта и тени от облаков
// считались тем же кодом, что и сам небосвод.

#extension GL_GOOGLE_include_directive : require

#include "sky_common.glsl"

layout(location = 0) in vec3 fragRay;
layout(location = 1) in vec3 fragEye;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    vec4 sunDirection;
    vec4 environment;
    vec4 waterFlags;
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 wind;
    vec4 worldOrigin;
} ubo;

layout(location = 0) out vec4 outColor;

void main() {
    const vec3 rd = normalize(fragRay);
    const vec3 ro = fragEye;
    const vec3 sunDir = normalize(ubo.sunDirection.xyz);
    // w канала солнца — интенсивность из климата: ночью она 0, и весь
    // рассеянный свет гаснет вместе с диском солнца.
    const float sunIntensity = ubo.sunDirection.w;
    // environment.w — время суток 0..1, wind.x — время в секундах,
    // wind.y — сила ветра, wind.z — покрытость неба облаками 0..1.
    const float dayFraction = ubo.environment.w;
    const float time = ubo.wind.x;
    const float windStrength = ubo.wind.y;
    const float cloudCover = ubo.wind.z;

    vec3 color = skyRadiance(ro, rd, sunDir, sunIntensity, dayFraction, cloudCover, time,
                             windStrength);

    // Ниже горизонта небо не видно: там либо земля, либо её дымка. Выходим
    // с уже посчитанным цветом зенита, иначе появится жёсткая линия
    // «небо/поднебесье» на стыке с рельефом.
    if (rd.y < 0.0) {
        const float t = saturate1(-rd.y * 6.0);
        color = mix(color, color * 0.55, t);
    }

    // Экспозиция общего кадра. Тонмаппер ACES удерживает солнечный диск и
    // подсвеченные кромки облаков от пересвета, при этом небо остаётся
    // контрастным, а не «молочным» — как при простой линейной клиппинге.
    outColor = vec4(tonemapACES(color), 1.0);
}
