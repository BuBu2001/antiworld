#include "renderer/frustum_culling.h"

#include <cmath>

namespace renderer {

namespace {

// Строка i column-major матрицы (glm-конвенция: m[col][row]).
awdm::dvec4 rowOf(const glm::mat4& m, int row) noexcept {
    return awdm::dvec4{static_cast<double>(m[0][row]), static_cast<double>(m[1][row]),
                       static_cast<double>(m[2][row]), static_cast<double>(m[3][row])};
}

// Плоскость из строки view-projection (Gribb-Hartmann). Нормаль направлена
// ВНУТРЬ frustum; нормализация делает signedDistanceTo евклидовым расстоянием.
Frustum::Plane planeFromRow(const awdm::dvec4& row) noexcept {
    Frustum::Plane plane;
    const double length = std::sqrt(row.x * row.x + row.y * row.y + row.z * row.z);
    if (!(length > 0.0) || !std::isfinite(length)) {
        // Вырожденная матрица (например, до первой инициализации камеры):
        // плоскость «пропускает всё» — false positive безопаснее false negative.
        plane.normal = awdm::dvec3{0.0, 0.0, 0.0};
        plane.distance = 0.0;
        return plane;
    }
    plane.normal = awdm::dvec3{row.x, row.y, row.z} / length;
    plane.distance = row.w / length;
    return plane;
}

}  // namespace

void Frustum::updateFromViewProjection(const glm::mat4& viewProjection,
                                       const awdm::dvec3& cameraPositionGlobal) noexcept {
    // Порядок фиксирован: left, right, bottom, top, near, far (см. заголовок).
    planes_[0] = planeFromRow(rowOf(viewProjection, 3) + rowOf(viewProjection, 0));
    planes_[1] = planeFromRow(rowOf(viewProjection, 3) - rowOf(viewProjection, 0));
    planes_[2] = planeFromRow(rowOf(viewProjection, 3) + rowOf(viewProjection, 1));
    planes_[3] = planeFromRow(rowOf(viewProjection, 3) - rowOf(viewProjection, 1));
    planes_[4] = planeFromRow(rowOf(viewProjection, 3) + rowOf(viewProjection, 2));
    planes_[5] = planeFromRow(rowOf(viewProjection, 3) - rowOf(viewProjection, 2));
    cameraPosition_ = cameraPositionGlobal;
}

bool Frustum::intersectsAABB(const AABB& box) const noexcept {
    for (const Plane& plane : planes_) {
        // p-vertex: вершина box, максимально удалённая вдоль нормали плоскости.
        const awdm::dvec3 p{plane.normal.x >= 0.0 ? box.max.x : box.min.x,
                            plane.normal.y >= 0.0 ? box.max.y : box.min.y,
                            plane.normal.z >= 0.0 ? box.max.z : box.min.z};
        if (plane.signedDistanceTo(p) < 0.0) return false;  // весь box снаружи
    }
    return true;
}

bool Frustum::intersectsAABB(const AABB& box,
                             double& outDistanceSqToCamera) const noexcept {
    bool visible = true;
    double distanceSq = 0.0;
    for (const Plane& plane : planes_) {
        const awdm::dvec3 p{plane.normal.x >= 0.0 ? box.max.x : box.min.x,
                            plane.normal.y >= 0.0 ? box.max.y : box.min.y,
                            plane.normal.z >= 0.0 ? box.max.z : box.min.z};
        const double d = plane.signedDistanceTo(p);
        if (d < 0.0) {
            visible = false;
            // Для отсечённых чанков накапливаем квадрат отрицательного
            // расстояния до ближайшей «нарушающей» плоскости — нижняя оценка
            // реального расстояния до frustum (дальше порога LOD2 оно заведомо,
            // поэтому для LOD-статистики этого достаточно).
            distanceSq += d * d;
        }
    }
    outDistanceSqToCamera = distanceSq;
    return visible;
}

double Frustum::distanceSqToAABB(const AABB& box, const awdm::dvec3& point) noexcept {
    // Квадрат расстояния от точки до ближайшей точки box: по каждой оси —
    // избыток за пределы интервала (0, если точка внутри по этой оси).
    double sum = 0.0;
    const auto excess = [](double v, double lo, double hi) noexcept {
        if (v < lo) return lo - v;
        if (v > hi) return v - hi;
        return 0.0;
    };
    const double dx = excess(point.x, box.min.x, box.max.x);
    const double dy = excess(point.y, box.min.y, box.max.y);
    const double dz = excess(point.z, box.min.z, box.max.z);
    sum = dx * dx + dy * dy + dz * dz;
    return sum;
}

}  // namespace renderer
