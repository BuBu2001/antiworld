#version 450

// Полноэкранный треугольник неба.
//
// Геометрии у неба нет вообще: три вершины растягиваются на весь экран, и
// направление взгляда приходится восстанавливать обратной viewProjection.
// Поэтому же небо рисуется ПОСЛЕ ландшафта с depthCompare = EQUAL и
// записью глубины выключенной — тогда шейдер выполняется ровно для тех
// пикселей, где ландшафт глубину не записал (там осталась очистка 1.0).
// Для неба это чистая экономия: половина экрана занята землёй, и небо
// считается только для видимой половины.

#extension GL_GOOGLE_include_directive : require

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

layout(location = 0) out vec3 fragRay;
layout(location = 1) out vec3 fragEye;

void main() {
    // Треугольник на весь экран из трёх вершин: (0,0) -> (-1,-1),
    // (2,0) -> (3,-1), (0,2) -> (-1,3). Третья вершина уходит за кадр,
    // но за один треугольник дешевле, чем два и шов между ними.
    const vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;

    // z = w = 1 даёт глубину ровно 1.0, то есть ровно ту, что осталась от
    // очистки на пустых пикселях. Отсюда и EQUAL в пайплайне.
    gl_Position = vec4(ndc, 1.0, 1.0);

    // Луч из глаза в этот пиксель: обратной VP раскладываем NDC в точку на
    // дальней плоскости и вычитаем глаз. Направление нормализуется в
    // фрагментном шейдере — там же, где считается длина до слоя облаков.
    //
    // ВАЖНО: обратная viewProjection даёт точку в СИСТЕМЕ КАМЕРЫ, а та
    // считает position() = global - origin, то есть точку в локальных
    // координатах относительно floating origin. Глаз же приходит в мировых.
    // Без обратного перевода в мировые координаты вычитание даёт вектор
    // длиной в десятки километров, направленный произвольно: небо становится
    // чёрным и прыгает при каждом сдвиге origin. Поэтому сначала возвращаем
    // дальнюю точку в мир, и только потом вычитаем глаз.
    const vec4 far = ubo.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    const vec3 farWorld = far.xyz / far.w + ubo.worldOrigin.xyz;
    fragRay = farWorld - ubo.cameraPosition.xyz;
    fragEye = ubo.cameraPosition.xyz;
}
