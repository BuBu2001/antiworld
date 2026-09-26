#version 450

// Фрагментный шейдер: альбедо цвета биома, снежный покров по сезону и
// направленное освещение от солнца.
//
// Снег — единственное, что меняется во времени года без перестройки mesh:
// вершина несёт постоянную «склонность к снегу» (snowBias, 0..1), а кадр —
// текущую морозность сезона (frost, 0..1). Снег лежит там, где их сумма
// превышает 1: у тундры bias около 0.85, поэтому она белая с середины осени
// до середины весны, а у пустыни bias = 0 и снега не бывает вовсе.

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragAlbedo;
layout(location = 2) in float fragSnowBias;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    vec4 sunDirection;
    vec4 environment;
} ubo;

layout(location = 0) out vec4 outColor;

// Цвет свежего снега: чуть голубой в тени, поэтому ночью снег не выглядит
// серым. Снег почти не поглощает свет — отражение на грани светах.
const vec3 kSnowColor = vec3(0.90f, 0.93f, 0.98f);

void main() {
    const vec3 normal = normalize(fragNormal);
    const vec3 sunDirection = normalize(ubo.sunDirection.xyz);
    const float frost = ubo.environment.x;
    const float ambient = ubo.environment.y;

    // Порог снега: frost + snowBias > 1. Полоса перехода 0.25 по frost
    // означает, что снег появляется и тает постепенно, а не скачком.
    const float cold = frost + fragSnowBias - 1.0f;
    const float snow = smoothstep(0.0f, 0.25f, cold);

    // Снег не держится на отвесных скалах, поэтому на крутых склонах видно
    // камень даже зимой. normal.y = 1 на горизонтали, 0 на вертикали.
    const float flatness = normal.y * normal.y;
    const float cover = snow * mix(0.25f, 1.0f, flatness);

    const vec3 albedo = mix(fragAlbedo, kSnowColor, cover);

    // Ламбертово освещение: у ландшафта без теней от карты, поэтому одного
    // направленного света и ambient достаточно.
    const float diffuse = max(dot(normal, sunDirection), 0.0f);
    const vec3 lit = albedo * (ambient + diffuse * ubo.sunDirection.w);

    outColor = vec4(lit, 1.0f);
}
