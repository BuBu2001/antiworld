#version 450

// Карта мира: текстура мира + маркер игрока.

layout(location = 0) in vec2 vUv;

layout(binding = 0) uniform sampler2D mapSampler;
layout(binding = 1) uniform MapUniforms {
    // xy — UV игрока на карте, z — радиус маркера в UV, w — 1, если игрок есть.
    vec4 player;
} ubo;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(mapSampler, vUv).rgb;

    // Крестик игрока: тонкая белая «прицельная» метка, чтобы её было видно и
    // на тёмном океане, и на светлой суше. Радиус задан в UV карты, поэтому
    // на любом разрешении и при любом масштабе маркер одного размера на карте.
    vec2 delta = vUv - ubo.player.xy;
    // Карта шире кадра в 2 раза (22585 x 22585 км), поэтому расстояние
    // считаем в «квадратных» UV: иначе маркер был бы вдвое выше, чем шире.
    delta.x *= 2.0;
    const float d = length(delta);
    const float r = max(ubo.player.z, 1e-5);

    // Кольцо + точка в центре.
    const float ring = smoothstep(r, r * 0.72, d);
    const float dot_ = smoothstep(r * 0.34, r * 0.20, d);
    const float mark = clamp(ring + dot_, 0.0, 1.0) * ubo.player.w;

    // Смешиваем с белым, но не до полного — иначе пропадает карта под маркером.
    color = mix(color, vec3(1.0, 1.0, 1.0), mark * 0.85);
    // Тонкая тёмная обводка кольца, чтобы белый крестик читался и на снегу.
    color = mix(color, vec3(0.0), smoothstep(r * 1.25, r * 1.05, d) *
                                       (1.0 - ring) * 0.5 * ubo.player.w);

    outColor = vec4(color, 1.0);
}
