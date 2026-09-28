#version 450

// Мир как Земля: сфера с картой, атмосферой и закатным терминатором.
//
// Геометрии нет — сфера считается аналитически, пересечением луча с
// единичной сферой. Это даёт гладкий лимб без полигональных артефактов и не
// требует вершинного/индексного буфера (у полноэкранного пайплайна их нет).
//
// Карта равнопромежуточная (4096x2048 = 2:1) и натянута как оболочка:
// u — долгота, v — широта, полюс — +Z мира. Ровно так её считал
// WorldMap::uvOf(), поэтому очертания материков те же, что на плоской карте.
//
// Поворот строит базис, ТРЕТЬЕЙ осью которого является направление игрока.
// Из этого следует ровно одно, что нужно знать для маркера: точка сферы в
// центре экрана — это (0,0,1) в видовых координатах, и после поворота она
// даёт q == f. Поэтому маркер рисуется там, где q близко к f, и он ВСЕГДА в
// центре диска, а центрировать точку вручную не нужно.

layout(location = 0) in vec2 vUv;

layout(binding = 0) uniform sampler2D mapSampler;
layout(binding = 1) uniform MapUniforms {
    // player: xy — UV игрока на карте, w — 1, если игрок есть.
    // params:  x — аспект кадра (ширина/высота), y — время, секунды.
    vec4 player;
    vec4 params;
} ubo;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const vec3 SUN = vec3(0.78, 0.30, 0.22);  // ~62% видимой поверхности освещено

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// UV равнопромежуточной карты для точки сферы. Полюс — +Z.
//
// atan(n.y, n.x), а НЕ atan(n.x, n.y): порядок осей задаёт, зеркальна ли
// карта. С atan(x,y) базис (восток, север, наружу) имеет det = -1, и
// материк смотрится зеркально — игрок, идущий на восток (+X), уезжал бы
// по карте на запад. Круговая проверка sphereUV(uvToDir(uv)) == uv такой
// баг не находит: она инвариантна к перестановке x/y.
vec2 sphereUV(vec3 n) {
    return vec2(0.5 + atan(n.y, n.x) / (2.0 * PI),
                0.5 + asin(clamp(n.z, -1.0, 1.0)) / PI);
}

// Направление на сфере, соответствующее UV карты (обратное к sphereUV).
// Порядок sin/cos здесь обязан совпадать с atan(y,x) в sphereUV: иначе
// карта зеркалится относительно своего же кругового теста.
vec3 uvToDir(vec2 uv) {
    const float lon = 2.0 * PI * (uv.x - 0.5);
    const float lat = PI * (uv.y - 0.5);
    return vec3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
}

void main() {
    const float aspect = max(ubo.params.x, 0.01);
    // 50° по вертикали. Меньше — сфера не влезает: при d=3 её угловой
    // диаметр 39°, то есть 24° камеры давали бы 166% высоты экрана.
    const float tanHalfFov = tan(0.5 * 0.87);

    const vec2 ndc = vUv * 2.0 - 1.0;
    const vec3 ro = vec3(0.0, 0.0, 3.0);
    const vec3 rd = normalize(vec3(ndc.x * aspect * tanHalfFov,
                                    ndc.y * tanHalfFov, -1.0));

    const bool hasPlayer = ubo.player.w > 0.5;
    // Направление на САМОГО игрока — это цель маркера, он не двигается при
    // вращении глобуса.
    const vec3 fPlayer = uvToDir(ubo.player.xy);
    // Точка, вставленная в ЦЕНТР диска: игрок + смещение от вращения мышью.
    // Так глобус доводится в любую сторону, а игрок уезжает по диску (и на
    // дальнюю полусферу, если довернуть сильно — как на настоящей модели).
    const vec2 centerUv = vec2(fract(ubo.player.x + ubo.params.z / (2.0 * PI)),
                               clamp(ubo.player.y + ubo.params.w / PI, 0.002, 0.998));
    const vec3 f = uvToDir(centerUv);

    // Базис с осью, направленной в центр диска. Ось наклонена, иначе полюс
    // карты совпал бы с осью обзора и мы бы видели только шапку выше 19° с.ш.
    const vec3 axis = normalize(vec3(0.0, 0.62, 0.78));
    vec3 e1 = cross(axis, f);
    if (dot(e1, e1) < 1e-8) {
        // Игрок ровно на полюсе карты: любая долгота там вырождена, берём
        // заранее выбранную, иначе базис развалится в NaN.
        e1 = vec3(1.0, 0.0, 0.0);
    }
    e1 = normalize(e1);
    const vec3 e2 = cross(f, e1);

    const float b = dot(ro, rd);
    const float disc = b * b - (dot(ro, ro) - 1.0);
    const float distToLine = length(cross(ro, rd));  // расстояние от центра до луча

    vec3 color = vec3(0.004, 0.006, 0.016);  // космос

    if (disc < 0.0) {
        const float star = hash13(floor(rd * 420.0));
        color += vec3(0.85, 0.9, 1.0) * step(0.9988, star) * (0.35 + 0.65 * star);
    }

    if (disc >= 0.0) {
        const vec3 n = normalize(ro + rd * (-b - sqrt(disc)));
        // Поворот в базис игрока. det = +1 (поворот, не отражение), поэтому
        // север остаётся севером, а материки не зеркалятся.
        const vec3 q = e1 * n.x + e2 * n.y + f * n.z;
        const vec3 surface = texture(mapSampler, sphereUV(q)).rgb;

        // Материк/океан по цвету: океан синий (b > r), суша зелёная/рыжая (r > b).
        const float ocean = smoothstep(-0.02, 0.06, surface.b - surface.r);

        // Солнце в экранных координатах: лимб и терминатор не крутятся
        // вместе с глобусом.
        const vec3 sun = normalize(SUN);
        const float ndl = dot(n, sun);
        const float day = smoothstep(-0.14, 0.32, ndl);

        // На тёмной стороне остаётся силуэт, на светлой — сама карта.
        color = surface * (vec3(0.05, 0.07, 0.13) + day * 1.05);

        // Блик только на воде: суша матовая.
        const vec3 view = -rd;
        const vec3 halfVec = normalize(sun + view);
        color += vec3(1.0, 0.95, 0.85) *
                 pow(max(dot(n, halfVec), 0.0), 90.0) * ocean * day * 0.7;

        // Атмосферный ободок по лимбу.
        const float rim = pow(1.0 - max(dot(n, view), 0.0), 3.2);
        color += vec3(0.28, 0.48, 0.95) * rim * day * 0.55;

        if (hasPlayer) {
            // Маркер там, где q == fPlayer. При нулевом смещении это ровно
            // центр диска; при повороте глобуса он едет по диску вместе с
            // игроком. Радиус задан в «единицах сферы», поэтому не зависит ни
            // от FOV, ни от разрешения окна.
            const float pulse = 0.5 + 0.5 * sin(ubo.params.y * 3.0);
            const float d = length(q - fPlayer);
            const float dot_ = smoothstep(0.016, 0.006, d);
            const float ring = smoothstep(0.052, 0.038, d) * smoothstep(0.026, 0.032, d);
            color = mix(color, vec3(1.0, 0.30, 0.20), dot_ * 0.9);
            color = mix(color, vec3(1.0, 0.95, 0.88),
                        ring * (0.55 + 0.35 * pulse) * (0.30 + 0.70 * day));
        }
    } else {
        // Свечение атмосферы снаружи диска, ярче со стороны солнца.
        const float halo = exp(-max(distToLine - 1.0, 0.0) * 9.0);
        const vec3 limbPoint = normalize(ro + rd * (-b));
        const float lit = smoothstep(-0.35, 0.55, dot(limbPoint, normalize(SUN)));
        color += vec3(0.30, 0.50, 0.98) * halo * lit * 0.55;
    }

    outColor = vec4(color, 1.0);
}
