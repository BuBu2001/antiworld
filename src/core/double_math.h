#pragma once

// Математика с двойной точностью (double precision) для глобальных координат мира.
//
// Почему это нужно: мир площадью ~510 млн км² (радиус порядка 12,7 млн м)
// физически не влезает в точность float32 — у float шаг квантования на
// расстоянии 10^7 метров составляет уже ~1 метр, и координаты «прыгают»
// крупнее пикселя. Поэтому в проекте введено жёсткое правило:
//
//   * double (64-бит) — ВСЕ глобальные координаты: позиции чанков, мировые
//     позиции сущностей, позиция камеры в мире, смещение floating origin;
//   * float (32-бит) — ТОЛЬКО локальные координаты внутри чанка (вершины
//     mesh относительно центра чанка, смещения относительно origin) и
//     данные, передаваемые в шейдеры Vulkan (GLSL-шейдеры работают с float).
//
// Файл оборачивает GLM (который сам по себе дженерик-библиотека и отлично
// работает с double: glm::dvec3, glm::dmat4, ...) в единый namespace awdm
// ("AntiWorld Double Math") и добавляет утилиты прецизионной арифметики:
// конверсию global <-> local через Floating Origin, безопасную нормализацию,
// дискретизацию координат в индексы чанков и т. д.
//
// Header-only: всё inline, зависимостей кроме GLM нет.

#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>   // lookAt/perspective/translate для dmat4
#include <glm/gtc/type_ptr.hpp>           // value_ptr для double-матриц

namespace awdm {

// === 64-битные алиасы GLM ===
// Векторы и матрицы двойной точности. Используем их для всего, что живёт в
// глобальной системе координат мира.
using dvec2 = glm::dvec2;
using dvec3 = glm::dvec3;
using dvec4 = glm::dvec4;
using dmat2 = glm::dmat2;
using dmat3 = glm::dmat3;
using dmat4 = glm::dmat4;

// === 32-битные алиасы GLM ===
// Только для локальных координат внутри чанка и для передачи в шейдеры.
using fvec2 = glm::vec2;
using fvec3 = glm::vec3;
using fvec4 = glm::vec4;
using fmat4 = glm::mat4;

// Индекс чанка в сетке мира (64-бит: при размере чанка 1 км координата чанка
// для радиуса 12,7 млн м ~ 12700 — в int32 помещается с запасом, но берём
// int64, чтобы хэш-функция и арифметика не имели сюрпризов на границах).
using ChunkIndex = std::int64_t;
struct ChunkCoord {
    ChunkIndex x = 0;
    ChunkIndex z = 0;

    constexpr bool operator==(const ChunkCoord&) const = default;
};

// === Константы точности ===

// Максимальное расстояние от начала координат (floating origin), после
// которого начинается заметный jitter: у float32 мантисса 24 бита, шаг
// квантования на дистанции d примерно равен d * 2^-23. При d = 1000 м шаг
// ~1,2e-4 м — приемлемо; дальше растёт линейно.
inline constexpr double kSafeRenderRadius = 1000.0;

// Эпсилон для сравнений double.
inline constexpr double kEpsilon = 1e-9;

// Бесконечность / NaN-проверки для векторов.
inline bool isFinite(const dvec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}
inline bool isFinite(const dvec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool isFinite(const dvec4& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::isfinite(v.w);
}

// === Преобразование глобальных и локальных координат ===
//
// Глобальная точка G (double, «истинные» координаты мира) переводится в
// локальную L (float, «координаты рендера») вычитанием origin O (double):
//     L = float(G - O)
// Вычитание делается в double — именно поэтому камера не дёргается: мы
// теряем точность только на малых числах (расстояниях до origin), а не на
// огромных глобальных координатах.

// global -> local (результат float — для вершин, uniform'ов, шейдеров).
inline fvec3 toLocal(const dvec3& global, const dvec3& origin) noexcept {
    const dvec3 delta = global - origin;
    return fvec3(static_cast<float>(delta.x),
                 static_cast<float>(delta.y),
                 static_cast<float>(delta.z));
}

// global -> local (результат double — для физики Jolt и логики игры).
inline dvec3 toLocalD(const dvec3& global, const dvec3& origin) noexcept {
    return global - origin;
}

// local -> global: поднимаем float-локальную координату обратно в double-мир.
inline dvec3 toGlobal(const fvec3& local, const dvec3& origin) noexcept {
    return origin + dvec3(static_cast<double>(local.x),
                          static_cast<double>(local.y),
                          static_cast<double>(local.z));
}
inline dvec3 toGlobal(const dvec3& local, const dvec3& origin) noexcept {
    return origin + local;
}

// === Безопасная нормализация (нулевой вектор не превращаем в NaN) ===
inline dvec3 safeNormalize(const dvec3& v, const dvec3& fallback = dvec3(0.0, 0.0, 1.0)) {
    const double len2 = glm::dot(v, v);
    if (len2 <= kEpsilon * kEpsilon) return fallback;
    return v * (1.0 / std::sqrt(len2));
}

// Расстояние между глобальными точками без промeжуточного dvec3 (меньше аллокаций
// в горячих циклах поиска ближайших чанков).
inline double distance2D(const dvec2& a, const dvec2& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}
inline double distance2D(const dvec3& a, const dvec3& b) noexcept {
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// === Дискретизация: мировые координаты <-> координаты чанка ===
//
// Floor-деление: отрицательные координаты уходят в соседний чанк корректно
// (мировой стиль, как в Minecraft). Округление к нулю здесь недопустимо.

// Мировая позиция (double) -> индекс чанка.
inline ChunkIndex worldToChunk(double worldCoord, double chunkSize) noexcept {
    return static_cast<ChunkIndex>(std::floor(worldCoord / chunkSize));
}
inline ChunkCoord worldToChunk(const dvec3& worldPos, double chunkSize) noexcept {
    return {worldToChunk(worldPos.x, chunkSize), worldToChunk(worldPos.z, chunkSize)};
}

// Индекс чанка -> мировая координата его минимального угла (corner).
inline double chunkToWorld(ChunkIndex chunkIdx, double chunkSize) noexcept {
    return static_cast<double>(chunkIdx) * chunkSize;
}

// Индекс чанка -> мировая координата центра чанка (тоже double!).
inline double chunkCenter(ChunkIndex chunkIdx, double chunkSize) noexcept {
    return (static_cast<double>(chunkIdx) + 0.5) * chunkSize;
}
inline dvec3 chunkCenter(const ChunkCoord& c, double chunkSize, double y = 0.0) noexcept {
    return {chunkCenter(c.x, chunkSize), y, chunkCenter(c.z, chunkSize)};
}

// Локальная позиция внутри чанка (float, [0, chunkSize) -> [-half, +half]).
// Используется при сборке mesh: вершины хранятся относительно центра чанка.
inline fvec3 centerLocal(double chunkSize, const fvec3& cornerLocal) noexcept {
    const float h = static_cast<float>(chunkSize) * 0.5f;
    return cornerLocal - fvec3(h, 0.0f, h);
}

// === Матрицы ===
// view/projection считаются в double, а в UBO кладутся уже как float — так
// camera-матрица не теряет разряды на этапе lookAt/perspective.

// lookAt в двойной точности (Vulkan-конвенция: правосторонняя, Y вверх).
inline dmat4 lookAt(const dvec3& eye, const dvec3& center, const dvec3& up) {
    return glm::lookAtRH(eye, center, up);
}

// Перспектива в двойной точности, Z in [0,1] (Vulkan-ready, OpenGL convention
// зеркалится вызывающим кодом так же, как в core::Camera).
inline dmat4 perspectiveZO(double fovyRadians, double aspect,
                           double zNear, double zFar) {
    return glm::perspectiveRH_ZO(fovyRadians, aspect, zNear, zFar);
}

// Подъём double-матрицы в float для загрузки в uniform-буфер.
inline fmat4 toFloat(const dmat4& m) noexcept {
    return glm::mat4(m);  // GLM делает поэлементное сужение
}

// Создание dmat4 из float-модельной матрицы (локальный transform объекта).
inline dmat4 toDouble(const fmat4& m) noexcept {
    return dmat4(m);
}

// Трансляция dmat4 на глобальное смещение (double).
inline dmat4 translate(const dmat4& m, const dvec3& v) {
    return glm::translate(m, v);
}

}  // namespace awdm
