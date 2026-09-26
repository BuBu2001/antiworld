// Реализация FloatingOrigin — см. подробное описание в floating_origin.h.

#include "core/floating_origin.h"

#include <cmath>
#include <utility>

#include "core/logger.h"

namespace core {

FloatingOrigin::FloatingOrigin(double thresholdMeters, double snapToChunkSize)
    : threshold_(thresholdMeters > 0.0 ? thresholdMeters : 1000.0),
      snapSize_(snapToChunkSize > 0.0 ? snapToChunkSize : 0.0) {
    if (thresholdMeters <= 0.0) {
        core::Logger::warn("FloatingOrigin: некорректный порог, использован 1000 м");
    }
}

awdm::dvec3 FloatingOrigin::origin() const {
    std::shared_lock lock(mutex_);
    return origin_;
}

awdm::dvec3 FloatingOrigin::snap(const awdm::dvec3& p) const {
    if (snapSize_ <= 0.0) return p;
    // Округляем XZ до кратных размеру чанка; Y не трогаем (высота — локальная
    // величина и остаётся малой даже на краю мира).
    const auto floorDiv = [this](double v) {
        return std::floor(v / snapSize_) * snapSize_;
    };
    return {floorDiv(p.x), p.y, floorDiv(p.z)};
}

OriginShift FloatingOrigin::update(const awdm::dvec3& cameraGlobalPosition) {
    if (!awdm::isFinite(cameraGlobalPosition)) {
        // NaN/Inf в позиции камеры — баг логики; молча игнорируем сдвиг,
        // чтобы не испортить origin и всю сцену.
        core::Logger::error("FloatingOrigin::update: неконечная позиция камеры, сдвиг пропущен");
        return {};
    }

    // Эксклюзивный замок: читаем origin и при необходимости мутируем его
    // атомарно относительно читателей (рабочих потоков стриминга чанков).
    std::unique_lock lock(mutex_);

    const awdm::dvec3 delta = cameraGlobalPosition - origin_;
    // Порог проверяется по горизонтальной дистанции (XZ): вертикальные
    // перепады высот редко превышают сотни метров, и дёргать origin из-за
    // прыжка/полёта незачем — jitter определяется именно удалением по XZ.
    const double dist2D = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (dist2D < threshold_) {
        return {};  // камера ещё в безопасной зоне — ничего не меняем
    }

    // Новый origin — позиция камеры, округлённая (опционально) до сетки
    // чанков. Смещение origin ровно равно delta, поэтому ЛЮБАЯ глобальная
    // точка G сохраняет локальные координаты:
    //     newLocal = G - (origin + delta) == (G - origin) - oldLocal - delta
    // то есть подписчикам достаточно вычесть delta из своих локальных данных.
    const awdm::dvec3 newOrigin = snap(cameraGlobalPosition);
    const awdm::dvec3 actualDelta = newOrigin - origin_;
    if (actualDelta == awdm::dvec3(0.0)) {
        return {};  // после округления origin фактически не сдвинулся
    }

    origin_ = newOrigin;
    const std::uint64_t n = shiftCount_.fetch_add(1) + 1;

    // Логируем только сам факт сдвига — это редкое событие (раз в ~1000 м).
    core::Logger::info("FloatingOrigin: сдвиг #" + std::to_string(n) +
                       ", origin = (" + std::to_string(origin_.x) + ", " +
                       std::to_string(origin_.y) + ", " +
                       std::to_string(origin_.z) + ")");

    return OriginShift{actualDelta, true};
}

OriginShift FloatingOrigin::teleportTo(const awdm::dvec3& newOriginRaw) {
    if (!awdm::isFinite(newOriginRaw)) {
        core::Logger::error("FloatingOrigin::teleportTo: неконечная точка, игнорируем");
        return {};
    }
    std::unique_lock lock(mutex_);
    const awdm::dvec3 newOrigin = snap(newOriginRaw);
    const awdm::dvec3 delta = newOrigin - origin_;
    if (delta == awdm::dvec3(0.0)) return {};
    origin_ = newOrigin;
    shiftCount_.fetch_add(1);
    core::Logger::info("FloatingOrigin: телепорт origin, delta = (" +
                       std::to_string(delta.x) + ", " + std::to_string(delta.y) +
                       ", " + std::to_string(delta.z) + ")");
    return OriginShift{delta, true};
}

OriginShift FloatingOrigin::reset() {
    std::unique_lock lock(mutex_);
    const awdm::dvec3 delta = awdm::dvec3(0.0) - origin_;
    if (delta == awdm::dvec3(0.0)) return {};
    origin_ = awdm::dvec3(0.0);
    shiftCount_.fetch_add(1);
    return OriginShift{delta, true};
}

void FloatingOrigin::setThreshold(double meters) {
    if (meters <= 0.0 || !std::isfinite(meters)) {
        core::Logger::warn("FloatingOrigin::setThreshold: некорректное значение, игнорируем");
        return;
    }
    std::unique_lock lock(mutex_);
    threshold_ = meters;
}

double FloatingOrigin::threshold() const {
    std::shared_lock lock(mutex_);
    return threshold_;
}

double FloatingOrigin::distanceTo(const awdm::dvec3& globalPosition) const {
    std::shared_lock lock(mutex_);
    return awdm::distance2D(globalPosition, origin_);
}

awdm::fvec3 FloatingOrigin::toLocal(const awdm::dvec3& global) const {
    std::shared_lock lock(mutex_);
    return awdm::toLocal(global, origin_);
}

awdm::dvec3 FloatingOrigin::toGlobal(const awdm::fvec3& local) const {
    std::shared_lock lock(mutex_);
    return awdm::toGlobal(local, origin_);
}

}  // namespace core
