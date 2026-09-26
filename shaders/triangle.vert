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
layout(location = 6) in mat4 inModel;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    // xyz — единичный вектор НА солнце, w — интенсивность солнца.
    vec4 sunDirection;
    // x — frost (морозность сезона), y — ambient, z — seaLevel (уровень моря),
    // w — время суток.
    vec4 environment;
    // x — есть ли в мире океан (1/0).
    vec4 waterFlags;
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

void main() {
    gl_Position = ubo.viewProjection * inModel * vec4(inPosition, 1.0);
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
    fragAlbedo = inColor;
    fragSnowBias = inSnowBias;
    fragWater = inWater;
    fragWorldPosition = (inModel * vec4(inPosition, 1.0)).xyz;
}
