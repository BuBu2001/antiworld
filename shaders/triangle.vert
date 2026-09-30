#version 450

// Вершинный шейдер: позиция/нормаль/UV из вершины, альбедо цвета биома и
// склонность к снегу. Ничего не освещает — это делает фрагментный шейдер.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inColor;
layout(location = 4) in float inSnowBias;
// Флаг географии: 1 — узел сетки под уровнем моря (вода), 0 — суша.
// Считается на CPU один раз при генерации mesh из карты высот, потому что
// «где океан» — это факт мира, а не визуальный эффект.
layout(location = 5) in float inWater;
// Локальный уровень моря в этой точке, мировые единицы. Глубина воды и
// прибойная полоса считаются по нему, а не по среднемировому уровню из UBO:
// океан стоит выше в тропиках и ниже у полюсов, и по среднему уровню у
// локально затопленного берега вода получалась бы «глубже», чем есть.
layout(location = 6) in float inLocalSeaLevel;
layout(location = 7) in mat4 inModel;
// Множитель альбедо инстанса (renderer::InstanceData::tint). Вид и вариант
// задают форму растения, а этот атрибут — оттенок: сухой склон желтее,
// затенённая тайга синее. Для рельефа и прочих объектов он равен (1,1,1).
layout(location = 11) in vec4 inTint;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    // xyz — единичный вектор НА солнце, w — интенсивность солнца.
    vec4 sunDirection;
    // x — frost (морозность сезона), y — ambient, z — seaLevel (уровень моря),
    // w — время суток 0..1.
    vec4 environment;
    // x — есть ли в мире океан (1/0).
    vec4 waterFlags;
    // Обратная viewProjection и позиция камеры нужны небу (sky.vert), здесь
    // объявлены, чтобы layout UBO был один на все шейдеры кадра.
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    // x — время в секундах, y — сила ветра, z — покрытость облаками.
    vec4 wind;
    // xyz — сдвиг floating origin в мировых координатах.
    vec4 worldOrigin;
} ubo;

layout(location = 0) out vec3 fragNormal;
// Альбедо биома идёт в фрагментный шейдер как есть: снег накладывается уже
// по пикселям, а не по вершинам, поэтому граница снега не зависит от
// плотности сетки.
layout(location = 1) out vec3 fragAlbedo;
layout(location = 2) out float fragSnowBias;
// Флаг воды и мировая высота вершины — нужны фрагментному шейдеру для
// океанской глади (см. inWater выше).
layout(location = 3) out float fragWater;
layout(location = 4) out vec3 fragWorldPosition;
// Насколько точка ниже ЛОКАЛЬНОГО уровня моря. Отрицательное значение — суша
// выше воды; по нему же считается прибойная полоса на берегу.
layout(location = 5) out float fragWaterDepth;
// Насколько точка гибкая: 0 у рельефа и камней, больше — у травы и крон.
layout(location = 6) out float fragFlex;

void main() {
    vec3 localPos = inPosition;

    // Ветер. Гибкость закодирована генератором геометрии в uv.x, а высота
    // внутри растения — в uv.y (0 у основания, 1 у макушки). У рельефа uv
    // нулевой, поэтому качается только инстансная растительность, и
    // ландшафт не «дышит» вместе с травой.
    if (inUV.x > 0.0) {
        // Смещение растёт к вершине (квадрат высоты), иначе ствол у самой
        // земли двигался бы вместе с макушкой, и растение казалось бы
        // выдвинутым из земли целиком.
        const float lever = inUV.y * inUV.y;
        // Три несинхронные частоты: порывистая основная (около 0.33 Гц),
        // медленная (0.09 Гц) и быстрая мелкая дрожь (0.75 Гц). Одна
        // синусоида даёт заметную «маятниковую» качку, три — ветер.
        const float t = ubo.wind.x;
        // Пространственная фаза: у соседних растений она разная, иначе всё
        // поле колыхалось синхронно, что выдаёт общий порыв ветра.
        const float phaseX = inModel[3].x * 0.35 + inModel[3].z * 0.27;
        const float phaseZ = inModel[3].z * 0.41 - inModel[3].x * 0.19;
        const float sway = sin(t * 2.1 + phaseX) * 0.6 + sin(t * 0.57 + phaseZ) * 0.4 +
                           sin(t * 4.7 + phaseX * 2.3) * 0.18;
        const float cross = cos(t * 1.7 + phaseZ * 1.1);
        // Порывы: медленная низкочастотная модуляция амплитуды.
        const float gust = 0.65 + 0.35 * sin(t * 0.37 + phaseZ * 0.13);
        // Смещение задаём в ЛОКАЛЬНЫХ единицах растения: ниже localPos
        // умножается на inModel, поэтому в мировых метрах отклонение
        // само пропорционально высоте растения.
        //
        // ДЕЛИТЬ на масштаб нельзя — это ровно тот случай, когда «на глаз
        // правильная» правка даёт обратный эффект: у травы масштаб ~0.4, и
        // после деления её макушка улетала на метр в сторону при высоте
        // самого пучка 0.4 м, то есть трава размазывалась шире себя.
        const float kMaxBend = 0.18f;  // макушка отклоняется не более ~18% высоты
        const float bend = inUV.x * lever * ubo.wind.y * gust * kMaxBend;
        localPos.xz += vec2(sway, cross * 0.7) * bend;
    }

    gl_Position = ubo.viewProjection * inModel * vec4(localPos, 1.0);
    // Нормаль переводится в мировые координаты обратной транспонированной
    // матрицей: у неунитарного масштаба (например, у сплюснутой модели)
    // transform(normal) исказил бы направление.
    // inverse() вычисляется ОДИН раз на вершину и используется для обеих
    // нужных операций; раньше обратная матрица считалась дважды (для
    // mat3-нормалей и для трансформации позиции), что удваивало стоимость
    // самой дорогой GLSL-операции в вершинном шейдере.
    mat3 modelRotationScaleInverseT = transpose(inverse(mat3(inModel)));
    // Защита от вырожденного масштаба (нулевой scale => сингулярная mat3 =>
    // inverse даёт NaN, normalize(NaN) «отравляет» все последующие цвета).
    // Если нормаль получилась нулевой/NaN, оставляем исходную — освещение
    // такой вершины деградировано, но кадр остаётся корректным.
    vec3 transformedNormal = modelRotationScaleInverseT * inNormal;
    // isfinite для vec3 недоступен в GLSL без расширения — проверяем по
    // компонентам: NaN/Inf не проходят ни одно сравнение, поэтому !(x>=-inf)
    // срабатывает ровно на нечисловых значениях.
    const float nLen2 = dot(transformedNormal, transformedNormal);
    if (!(nLen2 >= 0.0f) || nLen2 < 1e-12) {
        transformedNormal = inNormal;
    }
    fragNormal = normalize(transformedNormal);
    fragAlbedo = inColor * inTint.rgb;
    fragSnowBias = inSnowBias;
    fragWater = inWater;
    fragWorldPosition = (inModel * vec4(localPos, 1.0)).xyz;
    fragFlex = inUV.x;
    // Глубина в ЛОКАЛЬНЫХ координатах вершины, а не в мировых: флаг воды и
    // уровень моря заполняются из одной и той же карты высот в локальных
    // единицах, поэтому сравнение обязано быть в тех же единицах, иначе
    // floating origin (сдвиг матрицы модели) испортил бы границу воды.
    fragWaterDepth = inLocalSeaLevel - inPosition.y;
}
