#version 450

// Вершинный шейдер: позиция/нормаль/UV из вершины, альбедо цвета биома и
// склонность к снегу. Ничего не освещает — это делает фрагментный шейдер.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inColor;
layout(location = 4) in float inSnowBias;
layout(location = 5) in mat4 inModel;

layout(binding = 0) uniform UBO {
    mat4 viewProjection;
    // xyz — единичный вектор НА солнце, w — интенсивность солнца.
    vec4 sunDirection;
    // x — frost (морозность сезона), y — ambient, z — доля дня, w — время суток.
    vec4 environment;
} ubo;

layout(location = 0) out vec3 fragNormal;
// Альбедо биома идёт в фрагментный шейдер как есть: снег накладывается уже
// по пикселям, а не по вершинам, поэтому граница снега не зависит от
// плотности сетки.
layout(location = 1) out vec3 fragAlbedo;
layout(location = 2) out float fragSnowBias;

void main() {
    gl_Position = ubo.viewProjection * inModel * vec4(inPosition, 1.0);
    // Нормаль переводится в мировые координаты обратной транспонированной
    // матрицей: у неунитарного масштаба (например, у сплюснутой модели)
    // transform(normal) исказил бы направление.
    fragNormal = normalize(transpose(inverse(mat3(inModel))) * inNormal);
    fragAlbedo = inColor;
    fragSnowBias = inSnowBias;
}
