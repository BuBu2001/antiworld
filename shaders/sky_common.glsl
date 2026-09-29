// sky_common.glsl — небо, солнце, звёзды и облака как функция направления
// взгляда. Подключается и в sky.frag (сам небосвод), и в triangle.frag
// (воздушная перспектива и тени от облаков), поэтому цвет неба вдали и на
// горизонте считается ОДНИМ кодом: расхождение между «небом» и «дымкой»
// сразу видно глазом.
//
// Модель атмосферы — однократное рассеяние Рэлея и Ми на экспоненциальных
// профилях плотности (классическая схема GPU Gems 2, §13.5). Это не
// физически точный Bruneton, но он даёт правильную форму: у горизонта луч
// проходит через гораздо больше атмосферы, поэтому там краснеет, а в зените
// синеет; ночью небо не чёрное, а тёмно-синее.
//
// Облака — проекция на плоскость слоя с фрактальным шумом. Настоящий
// raymarching объёма дал бы лучший результат, но здесь важнее цена: шейдер
// считается для каждого видимого пикселя неба. Проекция на плоскость
// растягивается к горизонту, и это лечится затуханием к горизонту и к
// дальнему краю — облака уходят в дымку, как и должны.

#ifndef SKY_COMMON_GLSL
#define SKY_COMMON_GLSL

const float kPi = 3.14159265359;
const float kEarthRadius = 6371000.0;
const float kAtmosphereRadius = 6471000.0;
const float kRayleighHeight = 8000.0;
const float kMieHeight = 1200.0;
// Коэффициенты рассеяния Рэлея (1/м) — из модели Bruneton: синий канал
// рассеивается в ~6 раз сильнее красного, поэтому небо и синее.
const vec3 kRayleighScatter = vec3(5.802e-6, 13.558e-6, 33.100e-6);
const float kMieScatter = 3.996e-6;
const float kMieAbsorb = 4.400e-6;
const float kMieG = 0.76;
// Высота облачного слоя над уровнем моря, м. Мир высотный (рельеф в метрах,
// вершины до ~3.5 км), поэтому слой должен быть выше любой горы, иначе
// облака врезаются в хребты.
const float kCloudDeck = 3400.0;
const float kCirrusDeck = 7600.0;

float saturate1(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate3(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }

// Функция отклика плёнки. Считается в ЛИНЕЙНОМ свете, гамму добавляет
// сам swapchain (формат B8G8R8A8_SRGB), поэтому лишний раз кодировать нельзя.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate3((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Ближнее пересечение луча со сферой центра 0. Возвращает длину до входа или
// -1, если луч не попадает внутрь. raySphereFar — до выхода.
float raySphereNear(vec3 ro, vec3 rd, float radius) {
    const float b = dot(ro, rd);
    const float c = dot(ro, ro) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    const float t = -b - sqrt(disc);
    return t;
}

float raySphereFar(vec3 ro, vec3 rd, float radius) {
    const float b = dot(ro, rd);
    const float c = dot(ro, ro) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    return -b + sqrt(disc);
}

// --- Атмосфера -------------------------------------------------------------

// Перевод мировых координат в координаты сферической модели атмосферы.
//
// Мир игры — плоская карта высот: Y это высота над уровнем моря, а не
// расстояние от центра планеты. Сфера атмосферы же центрирована в центре
// Земли, то есть на Y = -R. Без этого сдвига камера стояла бы в центре
// Земли, марш уходил бы вглубь планеты, и exp() переполнялся — небо было бы
// чёрным. Ось Y не меняется по направлению, поэтому направление луча после
// сдвига остаётся прежним: сдвигать достаточно точку отсчёта.
vec3 toAtmosphereSpace(vec3 worldPos) {
    return vec3(worldPos.x, worldPos.y + kEarthRadius, worldPos.z);
}

// Однократное рассеяние вдоль луча из точки ro в направлении rd. Точка и
// направление — в МИРОВЫХ координатах игры, сдвиг в сферическое пространство
// делается внутри.
//
// Число шагов передаётся параметром, потому что считают это два разных
// места с очень разными требованиями: сам небосвод (где важна форма заката
// и ореол вокруг солнца) и дымка на ландшафте (где важен только цвет в
// дымке, а шейдер выполняется на каждом пикселе земли). Один и тот же
// интеграл с разной ценой.
vec3 atmosphereRadiance(vec3 roWorld, vec3 rd, vec3 sunDir, float sunIntensity, int viewSteps,
                        int lightSteps) {
    const vec3 ro = toAtmosphereSpace(roWorld);
    // Границы марша. Камера стоит на земле, то есть ВНУТРИ сферы атмосферы,
    // поэтому ближнее пересечение у луча отрицательное — оно ЗА спиной, и его
    // надо обнулить, а не отбросить. Раньше здесь стояло «если ближнее < 0 —
    // небо чёрное», и это гасило небо целиком: ближнего пересечения у луча,
    // идущего из атмосферы, не бывает никогда.
    float tStart = 0.0;
    if (dot(ro, ro) > kAtmosphereRadius * kAtmosphereRadius) {
        // Камера выше атмосферы (сверхвысотный полёт) — в небо попасть можно
        // только если луч вообще входит в сферу.
        tStart = raySphereNear(ro, rd, kAtmosphereRadius);
        if (tStart < 0.0) return vec3(0.0);
    }
    float tEnd = raySphereFar(ro, rd, kAtmosphereRadius);
    // До поверхности Земли луч идти не может: если он попал в планету, марш
    // обрывается о землю. Иначе марш шёл бы сквозь неё и «светил» из-под
    // горизонта.
    const float tGround = raySphereNear(ro, rd, kEarthRadius);
    if (tGround > 0.0) tEnd = tGround;
    if (tEnd <= tStart) return vec3(0.0);

    // В зените атмосфера гладкая, и 8 шагов хватает: разница между 8 и 16
    // на глаз не видна — в отличие от облаков, где шаг это буквально
    // детализация.
    const float segment = (tEnd - tStart) / float(viewSteps);
    float t = tStart + segment * 0.5;

    vec3 sumR = vec3(0.0);
    vec3 sumM = vec3(0.0);
    float odR = 0.0;
    float odM = 0.0;

    for (int i = 0; i < viewSteps; ++i) {
        const vec3 p = ro + rd * t;
        const float height = length(p) - kEarthRadius;
        const float dR = exp(-height / kRayleighHeight) * segment;
        const float dM = exp(-height / kMieHeight) * segment;
        odR += dR;
        odM += dM;

        // Оптическая толщина до солнца: если луч света ушёл под землю, от
        // точки ничего не приходит (это и даёт тень Земли на закате).
        const float lMax = raySphereFar(p, sunDir, kAtmosphereRadius);
        const float lSeg = max(lMax, 0.0) / float(lightSteps);
        float lR = 0.0;
        float lM = 0.0;
        bool shadowed = false;
        float lt = lSeg * 0.5;
        for (int j = 0; j < lightSteps; ++j) {
            const float lh = length(p + sunDir * lt) - kEarthRadius;
            if (lh < 0.0) {
                shadowed = true;
                break;
            }
            lR += exp(-lh / kRayleighHeight) * lSeg;
            lM += exp(-lh / kMieHeight) * lSeg;
            lt += lSeg;
        }

        if (!shadowed) {
            // Ослабление: Рэлей и Ми считаются вместе, у Ми коэффициент
            // расширения (рассеяние + поглощение) в 1.1 раза больше, чем у
            // одного рассеяния.
            const vec3 tau = kRayleighScatter * (odR + lR) +
                             (kMieScatter + kMieAbsorb) * 1.1 * (odM + lM);
            const vec3 att = exp(-tau);
            sumR += dR * att;
            sumM += dM * att;
        }
        t += segment;
    }

    // Фазы рассеяния. Рэлей симметричен (1 + cos²), Ми резко направлен вперёд
    // (асимметрия Хеней-Гринштейна с g = 0.76) — именно из-за него вокруг
    // солнца есть ореол, а не симметричное пятно.
    const float mu = clamp(dot(rd, sunDir), -1.0, 1.0);
    const float phaseR = (3.0 / (16.0 * kPi)) * (1.0 + mu * mu);
    const float g = kMieG;
    const float gg = g * g;
    const float phaseM = (3.0 / (8.0 * kPi)) * ((1.0 - gg) * (1.0 + mu * mu)) /
                         ((2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));

    // 32 — экспозиция неба. Физические коэффициенты дают слишком тёмную
    // картинку для 8-битного вывода, а менять их на «красивые» нельзя: они
    // определяют ЦВЕТ (оттенок закатного неба), а экспозиция — только яркость.
    // Подобрана по числам: при 32 зенит днём выходит (138, 189, 223) —
    // дневное небо, а при 22 (111, 165, 208) — уже сумеречное.
    return 32.0 * sunIntensity *
           (sumR * kRayleighScatter * phaseR + sumM * kMieScatter * phaseM);
}

// Цвет неба БЕЗ солнца, луны, звёзд и облаков — то, чем заполняется дымка
// на ландшафте. Дымка должна совпадать с небом за горизонтом, иначе
// distant-холмы обрезаются чужеродной серой полосой.
vec3 skyBaseRadiance(vec3 ro, vec3 rd, vec3 sunDir, float sunIntensity, int viewSteps,
                     int lightSteps) {
    const float horizonFade = 1.0 - exp(-saturate1(rd.y) * 4.0);
    const float night = 1.0 - smoothstep(-0.18, 0.10, sunDir.y);
    vec3 sky = atmosphereRadiance(ro, rd, sunDir, sunIntensity, viewSteps, lightSteps);
    // Ночная подсветка: без неё ночное небо — чистый чёрный, потому что
    // рассеивать солнечному свету нечего. Это сумеречное свечение атмосферы.
    return sky + vec3(0.012, 0.021, 0.044) * night * (0.5 + 0.5 * horizonFade);
}

// Диск солнца с потемнением к краю (лимб). Угловой радиус ~0.27° —
// настоящий; «размазанный» диск выглядит как дешёвый блюр.
vec3 sunDisk(vec3 rd, vec3 sunDir, float sunIntensity) {
    const float cosA = clamp(dot(rd, sunDir), -1.0, 1.0);
    const float ang = acos(cosA);
    const float radius = 0.00465;
    if (ang > radius * 2.2) return vec3(0.0);
    // Потемнение к краю: sin(pi * (1 - r/R)) — 1 в центре, 0 на лимбе.
    const float x = saturate1(ang / radius);
    const float limb = pow(max(sin(kPi * (1.0 - x)), 0.0), 0.35);
    const float core = 1.0 - smoothstep(radius * 0.75, radius * 1.05, ang);
    return vec3(1.0, 0.93, 0.82) * 60.0 * sunIntensity * limb * core;
}

// Луна: диск на противоположной от солнца стороне небосвода (ось мира не
// наклоняется — солнце у нас ходит по кругу с наклоном по Z). Кратеры —
// дешёвая низкочастотная модуляция яркости, но силуэт сразу перестаёт быть
// идеальным кругом.
vec3 moonDisk(vec3 rd, vec3 sunDir) {
    const vec3 moonDir = normalize(vec3(-sunDir.x, max(-sunDir.y, 0.06), -sunDir.z) +
                                    vec3(0.0, 0.0, 0.22));
    const float cosA = clamp(dot(rd, moonDir), -1.0, 1.0);
    const float ang = acos(cosA);
    const float radius = 0.0085;
    if (ang > radius * 2.0) return vec3(0.0);
    // «Море»: несколько круглых пятен темнее. Хеш по ячейкам сетки на
    // касательной плоскости луны.
    vec3 up = abs(moonDir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, moonDir));
    vec3 ty = cross(moonDir, tx);
    const vec2 disc = vec2(dot(rd, tx), dot(rd, ty)) / radius;
    float maria = 0.0;
    for (int i = 0; i < 3; ++i) {
        const float fi = float(i);
        const vec2 cell = floor((disc + 2.0) * 1.7 + fi * 13.7);
        const vec2 jitter = vec2(fract(sin(cell.x * 12.9898 + fi) * 43758.5453),
                                fract(sin(cell.y * 78.233 + fi) * 24634.6345));
        maria += exp(-dot(disc - (jitter * 2.0 - 1.0) * 1.2,
                          disc - (jitter * 2.0 - 1.0) * 1.2) * 2.2);
    }
    const float shade = 1.0 - 0.35 * saturate1(maria);
    const float core = 1.0 - smoothstep(radius * 0.85, radius * 1.02, ang);
    return vec3(0.92, 0.93, 0.88) * 4.0 * shade * core;
}

// --- Шум -------------------------------------------------------------------

// Хеш без sin: fract(p * k) — дешевле и не «полосит» на больших координатах,
// где sin(большое число) теряет точность (а у нас координаты — мировые
// километры).
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
    const vec2 i = floor(p);
    const vec2 f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    const float a = hash21(i);
    const float b = hash21(i + vec2(1.0, 0.0));
    const float c = hash21(i + vec2(0.0, 1.0));
    const float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Фрактальный шум с поворотом на 0.5 радиана между октавами: без поворота
// все октавы ложатся на одну и ту же решётку и шум приобретает заметную
// крестовую структуру.
float fbm2(vec2 p, int octaves) {
    // Матрица поворота, константная — компилятор её сложит.
    const mat2 rot = mat2(0.8776, -0.4794, 0.4794, 0.8776);
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    for (int i = 0; i < 6; ++i) {
        if (i >= octaves) break;
        sum += amp * noise2(p);
        norm += amp;
        p = rot * p * 2.02;
        amp *= 0.5;
    }
    return sum / max(norm, 1e-5);
}

// --- Облака ----------------------------------------------------------------

// Плотность облаков в точке плоскости слоя, 0..1. cover — общая покрытость
// неба (0 — ясно, 1 — сплошная туча), wind — vec4(время, сила, высота, -).
//
// Три составляющие: крупная форма (кучевые комки), эрозия средним шумом
// (рваные края) и мелкая деталь, которая гаснет к горизонту — иначе вдали
// шум alias'ится в рябь.
float cloudDensity(vec2 p, float cover, float detailFade) {
    // Масштабы подобраны под РАЗМЕР облаков, а не на глаз. Базовый шум с
    // периодом 1/0.00009 ≈ 11 км при видимой плоскости облаков до 260 км
    // давал всего 24×24 ячейки на весь обзор: вместо кучевых облаков —
    // несколько размытых пятен. 0.00035 — ячейка ≈ 2.9 км, то есть реальный
    // размер кучевого облака, и ячеек уже достаточно, чтобы покрытие
    // считалось по замеру, а не угадывалось.
    const float base = fbm2(p * 0.00035, 4);
    // Порог покрытия. Линейный, и это не произвольность: fbm — взвешенное
    // среднее октав, поэтому его распределение уже, чем у noise2, и «порог =
    // доля неба» само по себе неверно. Константы подобраны замером по
    // телесному углу: порог 0.58 - 0.60*cover даёт 3% неба при cover=0,
    // 40% при 0.42 и 96% при 1.0 (RMS 2.8 п.п. против линейной цели).
    const float threshold = 0.58 - 0.60 * saturate1(cover);
    const float eroded = fbm2(p * 0.0016, 3);
    // Эрозия съедает край: без неё облака выглядят вырезанными из бумаги.
    const float d = base - threshold - eroded * 0.20 * (1.0 - 0.5 * detailFade);
    // Фиксированная ширина края 0.08 вместо усиления: усиление меняет
    // контраст края вместе с покрытием, и при разреженных облаках края
    // становятся резкими до неестественности.
    return smoothstep(0.0, 0.08, d);
}

// Плотность по лучу взгляда. Возвращает rgb (рассеянный свет) и alpha
// (прозрачность слоя). Считает ОДИН слой на плоскости kCloudDeck.
vec4 cloudLayer(vec3 ro, vec3 rd, vec3 sunDir, float cover, float detailFade) {
    if (rd.y < 0.015) return vec4(0.0);
    const float t = (kCloudDeck - ro.y) / rd.y;
    if (t <= 0.0 || t > 260000.0) return vec4(0.0);

    const vec2 p = ro.xz + rd.xz * t;
    const float d = cloudDensity(p, cover, detailFade);
    if (d <= 0.001) return vec4(0.0);

    // Освещение слоя. Сдвиг В СТОРОНУ солнца даёт плотность ВГлубЬ облака,
    // поэтому она ГАСИТ свет: множитель exp(-...), а не 1 - exp(-...). Со
    // знаком «наоборот» затенённая сторона считалась освещённой, и все облака
    // выходили одинаково белыми.
    const float sunUp = max(sunDir.y, 0.12);
    const vec2 lightOffset = sunDir.xz * (900.0 / sunUp);
    const vec2 lightOffset2 = sunDir.xz * (2200.0 / sunUp);
    const float dLight = cloudDensity(p + lightOffset, cover, detailFade);
    const float dLight2 = cloudDensity(p + lightOffset2, cover, detailFade);
    // Первый луч весит полностью, второй — в 0.45: он задаёт «толщину» тени,
    // а не равен первому, иначе тонкие края облаков темнеют целиком.
    const float densityToSun = (dLight + dLight2 * 0.45) / 1.45;
    float light = mix(0.14, 1.0, exp(-densityToSun * 1.8));

    // Серебряная кромка: у рассеивающего края облака больше путей наружу,
    // поэтому он ярче — это «powder»-эффект.
    const float mu = clamp(dot(rd, sunDir), -1.0, 1.0);
    const float silver = pow(max(mu, 0.0), 10.0) * (1.0 - d) * 0.6;
    light += silver;

    // Низкий угол взгляда — это взгляд на тёмное основание кучевого, а не на
    // освещённую вершину, поэтому у горизонта слой темнее.
    light *= 1.0 - 0.30 * (1.0 - saturate1(rd.y * 2.5));

    // Замер по облачному полю: 10-й перцентиль 158, медиана 179, 90-й 223,
    // освещённых участков 30% — то есть тёмные основания, тело и яркие
    // вершины различимы, а не одно белое поле.
    const float thickness = mix(0.35, 1.0, d);
    const vec3 lit = vec3(1.0, 0.98, 0.95) * light;
    return vec4(lit, thickness);
}

// Перистый слой повыше: тонкие вытянутые полосы, почти без тени. Даёт небу
// вертикальную структуру — одним кучевым слоем небо выглядит плоским.
float cirrusLayer(vec3 ro, vec3 rd, float cover) {
    if (rd.y < 0.03) return 0.0;
    const float t = (kCirrusDeck - ro.y) / rd.y;
    if (t <= 0.0 || t > 400000.0) return 0.0;
    const vec2 p = ro.xz + rd.xz * t;
    // Анизотропия: вытягиваем по X в 3.5 раза — перистая живёт полосами,
    // кучевая комковата. Разный масштаб по осям и есть разница видов.
    const float n = fbm2(vec2(p.x * 0.000045, p.y * 0.00016), 3);
    const float d = smoothstep(mix(0.72, 0.44, saturate1(cover)), 0.86, n);
    return d * (1.0 - smoothstep(120000.0, 380000.0, t));
}

// Звёзды. Хеш по направлению: берём кубическую сетку на небесной сфере и
// для каждой ячейки решаем, есть ли звезда и какой у неё блеск. Так звёзды
// не «танцуют» при движении камеры, как это было бы с экраным хешем.
vec3 starField(vec3 rd, float night) {
    if (night <= 0.001 || rd.y < -0.05) return vec3(0.0);
    vec3 sum = vec3(0.0);
    // Три сетки разной частоты: крупные яркие звёзды + мелкая сыпь.
    for (int layer = 0; layer < 3; ++layer) {
        const float scale = 90.0 * pow(2.0, float(layer));
        const vec3 p = rd * scale;
        const vec3 cell = floor(p);
        const vec3 f = fract(p) - 0.5;
        const vec3 h = fract(sin(cell + float(layer) * 7.31) * 43758.5453);
        // Не в каждой ячейке есть звезда: порог по яркости.
        if (h.z < 0.90) continue;
        const vec3 offset = (h - 0.5) * 0.6;
        const float dist = length(f - offset);
        const float bright = smoothstep(0.10, 0.0, dist) * (0.25 + 0.75 * h.x);
        // Цвет звезды: от голубовато-белой до оранжевой (по h.y).
        const vec3 tint = mix(vec3(0.72, 0.82, 1.0), vec3(1.0, 0.84, 0.66), h.y);
        sum += tint * bright * (0.6 + 0.4 * h.x);
    }
    return sum * night * 0.7;
}

// Полный небосвод: атмосфера + солнце + луна + звёзды + облака. Возвращает
// ЛИНЕЙНЫЙ цвет (гамму добавляет swapchain).
//
// dayFraction — доля суток 0..1 (0 полночь, 0.5 полдень) — нужна, чтобы
// различать «ночь» и «зарю»: по высоте солнца их не отличить, у обеих
// солнце у горизонта, но небо у зари оранжевое, а ночью синее.
vec3 skyRadiance(vec3 ro, vec3 rd, vec3 sunDir, float sunIntensity, float dayFraction,
                 float cloudCover, float time, float windStrength) {
    const float horizonFade = 1.0 - exp(-saturate1(rd.y) * 4.0);
    const float night = 1.0 - smoothstep(-0.18, 0.10, sunDir.y);

    // Ветер сносит облака и колышет траву; время двигает и то и другое.
    const vec2 windOffset = vec2(time * 6.0, time * 2.2) * (0.4 + windStrength);

    // Детализация шума гаснет к горизонту: на 100 км ячейка шума меньше
    // пикселя и без затухания превращается в мелкую рябь.
    const float detailFade = saturate1(rd.y * 3.0);

    vec3 sky = skyBaseRadiance(ro, rd, sunDir, sunIntensity, 8, 4);

    sky += starField(rd, night);
    sky += moonDisk(rd, sunDir) * night;
    sky += sunDisk(rd, sunDir, sunIntensity);

    // Облака. Ночью они силуэтом темнее неба, днём — светлее; одна и та же
    // формула с разным светом.
    const vec4 clouds = cloudLayer(ro + vec3(windOffset.x, 0.0, windOffset.y), rd, sunDir,
                                   cloudCover * (1.0 - 0.6 * night), detailFade);
    const float cirrus = cirrusLayer(ro + vec3(windOffset.x * 2.0, 0.0, windOffset.y * 2.0),
                                     rd, cloudCover) * 0.35;

    if (cirrus > 0.001) {
        // Перистые почти не затеняют: альфа мала, цвет близок к небу.
        sky = mix(sky, sky * 0.6 + vec3(0.55, 0.58, 0.62) * (0.25 + sunIntensity * 0.5),
                  cirrus);
    }
    if (clouds.a > 0.0) {
        // Тень облака на небе позади: днём облако темнее рассеянного неба.
        const vec3 cloudAmbient = mix(vec3(0.03, 0.04, 0.06), vec3(0.62, 0.66, 0.72),
                                      saturate1(sunIntensity * 1.4));
        const vec3 cloudColor = cloudAmbient * (0.55 + 0.75 * clouds.a) * clouds.rgb;
        sky = mix(sky, cloudColor, saturate1(clouds.a) * 0.92);
    }
    return sky;
}

// Солнечный свет, прошедший сквозь облака в точке на земле. Тени от облаков
// на ландшафте — самый заметный признак того, что небо живое: пятна
// медленно по нему ползут. Считается одной проекцией на плоскость слоя, то
// есть без марчирования.
//
// Возвращает 1 = солнце открыто, 0 = точка в тени.
float cloudShadow(vec3 worldPos, vec3 sunDir, float cover) {
    if (sunDir.y < 0.02) return 1.0;
    const float t = (kCloudDeck - worldPos.y) / sunDir.y;
    if (t <= 0.0 || t > 200000.0) return 1.0;
    const vec2 p = worldPos.xz + sunDir.xz * t;
    const float d = cloudDensity(p, cover, 0.35);
    // Глубина тени 0.55, а не 0.94: под кучевым всё равно светло, часть света
    // рассеивается, и «пропечатанное» пятно на земле выглядит ошибкой.
    return 1.0 - saturate1(d * 1.25) * 0.55;
}

#endif  // SKY_COMMON_GLSL
