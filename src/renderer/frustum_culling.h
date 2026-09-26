#pragma once

// frustum_culling — отсечение чанков по пирамиде видимости (CPU, double).
//
// Почему это нужно: при view distance в несколько чанков и LRU-кэше на 500
// чанков бо́льшая часть загруженных чанков в конкретный кадр находится ЗА
// камерой или сбоку от неё. Отправить их в командный буфер — значит подарить
// GPU сотни draw call'ов с гарантированно пустыми треугольниками. Дешевле
// проверить пересечение AABB чанка с frustum на CPU до записи команды.
//
// Точность: мир глобальный (double, см. core/double_math.h), поэтому AABB
// чанка строится и проверяется в DOUBLE: на дистанции 10 км шаг float ~1 мм,
// но при вычитании больших координат ошибка «съедает» граничные случаи
// (чанк у края экрана мигал бы visible/hidden). Камера приходит в глобальных
// double-координатах, view-projection — float (она из шейдера/GPU-конвейера),
// плоскости извлекаются из неё, но точка камеры подставляется double —
// итоговые проверки идут целиком в double.
//
// Извлечение плоскостей (метод Gribb-Hartmann, «строки матрицы»): для
// column-major glm::mat4 m строка i = (m[0][i], m[1][i], m[2][i], m[3][i]),
// шесть плоскостей (нормали направлены ВНУТРЬ frustum):
//   left = row3 + row0, right = row3 - row0,
//   bottom = row3 + row1, top = row3 - row1,
//   near = row3 + row2, far = row3 - row2.
// Для правосторонней перспективы (glm::perspectiveRH_ZO, конвенция камеры
// проекта) это точные границы видимого объёма. Нормализация делает
// signedDistanceTo евклидовым расстоянием в метрах.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "core/double_math.h"
// kLodLevels нужен для CullingStats. include обязан стоять ДО namespace
// renderer: сам lod_manager.h открывает namespace renderer, и включение его
// внутрь нашего namespace давало renderer::renderer::kLodLevels.
#include "renderer/lod_manager.h"

namespace renderer {

// Ось-выровненный bounding box в double-координатах.
struct AABB {
    awdm::dvec3 min{0.0};
    awdm::dvec3 max{0.0};

    static AABB fromCenterExtent(const awdm::dvec3& center,
                                 const awdm::dvec3& halfExtent) noexcept {
        return {center - halfExtent, center + halfExtent};
    }
    // Расширить box по Y симметрично (запас под растительность/перепады).
    AABB expandedY(double extraHalf) const noexcept {
        return {awdm::dvec3{min.x, min.y - extraHalf, min.z},
                awdm::dvec3{max.x, max.y + extraHalf, max.z}};
    }
    awdm::dvec3 center() const noexcept { return (min + max) * 0.5; }
};

class Frustum {
public:
    struct Plane {
        awdm::dvec3 normal{0.0, 0.0, 1.0};
        double distance{0.0};  // n·p + d = 0 для точек плоскости

        // Знаковое расстояние до точки (>0 — со стороны нормали).
        double signedDistanceTo(const awdm::dvec3& p) const noexcept {
            return glm::dot(normal, p) + distance;
        }
    };

    Frustum() = default;

    // Пересчитать 6 плоскостей из view-projection матрицы камеры.
    // viewProjection — column-major glm::mat4 (то, что кладут в UBO);
    // cameraPositionGlobal — позиция камеры в ГЛОБАЛЬНЫХ double-координатах
    // (floating origin учтён вызывающим: локальная позиция камеры добавляется
    // к origin до вызова — см. world::ChunkManager::updateCulling).
    void updateFromViewProjection(const glm::mat4& viewProjection,
                                  const awdm::dvec3& cameraPositionGlobal) noexcept;

    // Пересекает ли AABB frustum. Тест по «положительному углу» (p-vertex):
    // для каждой плоскости берётся вершина box, максимально удалённая вдоль
    // нормали; если она строго снаружи — весь box снаружи (отсекаем).
    // False negative невозможен: ничего видимое не теряем, допустимы лишь
    // редкие false positive на гранях (цена — один лишний draw, не артефакт).
    bool intersectsAABB(const AABB& box) const noexcept;

    // Заодно считает квадрат евклидова расстояния от cameraPosition() до
    // ближайшей точки box (в метрах^2, double) — одна итерация по 6
    // плоскостям вместо двух проходов (culling + LOD-выбор).
    bool intersectsAABB(const AABB& box, double& outDistanceSqToCamera) const noexcept;

    // Квадрат расстояния от точки до ближайшей точки AABB (double, метры^2).
    // Именно по расстоянию ДО КРАЯ (а не до центра) выбирается LOD: чанк с
    // далёким центром может иметь ближний край в сотне метров от игрока.
    double distanceSqToAABB(const AABB& box) const noexcept {
        return distanceSqToAABB(box, cameraPosition_);
    }
    static double distanceSqToAABB(const AABB& box, const awdm::dvec3& point) noexcept;

    const awdm::dvec3& cameraPosition() const noexcept { return cameraPosition_; }
    const std::array<Plane, 6>& planes() const noexcept { return planes_; }

private:
    std::array<Plane, 6> planes_{};
    awdm::dvec3 cameraPosition_{0.0};
};

// Статистика отсечения за кадр (для HUD: доказательство, что culling реально
// снижает число draw call'ов).
struct CullingStats {
    std::uint32_t candidates{0};     // чанков-кандидатов (Loaded)
    std::uint32_t culled{0};         // отсечено frustum
    std::uint32_t submitted{0};      // попало в командный буфер
    std::uint32_t lodCount[kLodLevels]{};  // разбивка по выбранным LOD

    // Суммарно индексов в отрисовке текущего кадра (по выбранным LOD) —
    // показывает реальную экономию вершинного/индексного объёма от LOD.
    std::uint64_t submittedIndices{0};
    // Сколько индексов ушло бы без LOD (все чанки полным LOD0).
    std::uint64_t unculledIndicesNoLod{0};
};

// Выбор уровня детализации по расстоянию до AABB (пороги в метрах).
// hysteresisBias — запас, чтобы чанк на границе порога не «мигал» LOD'ами:
// переход вверх (к более детальному) требует запаса, вниз — нет.
inline int selectLodLevel(double distanceMeters, double lod1Distance,
                          double lod2Distance, double hysteresisBias = 0.0) noexcept {
    if (distanceMeters > lod2Distance - hysteresisBias) return 2;
    if (distanceMeters > lod1Distance - hysteresisBias) return 1;
    return 0;
}

}  // namespace renderer
