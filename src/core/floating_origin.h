#pragma once

// Система Floating Origin (плавающее начало координат).
//
// Проблема: рендер (Vulkan/GLSL) и физика (Jolt) работают в float32. На
// дистанциях порядка тысяч-миллионов метров от глобального центра (0,0,0)
// у float заканчивается мантисса: шаг квантования растёт линейно с
// расстоянием, вершины mesh и тела начинают «дрожать» (jitter), а матрицы
// трансформации теряют разряды. Для мира площадью ~510 млн км² это критично
// уже на десятках километров.
//
// Решение: держим «центр рендера» (origin) всегда рядом с игроком.
//   * Истинное положение камеры хранится в double — globalPosition;
//   * Всё, что уходит в шейдеры/физику, считается как local = global - origin
//     и остаётся маленьким числом, точным в float;
//   * Когда |global - origin| превышает порог (по умолчанию 1000 м), origin
//     «переезжает» к игроку. Переезд выражается СМЕЩЕНИЕМ delta (double),
//     которое подписчики применяют к своим данным:
//         chunk.globalPos -= delta        // глобальные позиции чанков
//         body.SetPosition(local - float(delta))  // тела Jolt
//     Визуально сцена не меняется НИ НА СКОЛЬКО: точка, которая была в
//     local G-O, после сдвига оказывается в local (G-delta)-(O-delta) — то
//     же самое. Камера не дёргается, потому что её локальная позиция
//     пересчитывается из double-разности, а не из «сырого» float.
//
// Поток данных каждый кадр:
//   1) логика двигает камеру в глобальных double-координатах;
//   2)FloatingOrigin::update(cameraGlobal) — при необходимости сдвигает origin
//      и возвращает событие OriginShifted (смещение delta);
//   3) при nonzero delta все подсистемы (world chunks, ECS transforms, Jolt
//      bodies, water plane...) сдвигают свои ЛОКАЛЬНЫЕ координаты на -delta;
//   4) рендер берёт локальные float-координаты — они всегда близки к нулю.
//
// Потокобезопасность: все методы защищают состояние shared_mutex — читать
// origin могут несколько рабочих потоков генерации/стриминга одновременно,
// сдвиг выполняет один поток логики (эксклюзивный lock).

#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "core/double_math.h"

namespace core {

// Результат update(): ненулевой delta означает, что origin сдвинулся и
// подписчики обязаны пересчитать свои локальные координаты.
struct OriginShift {
    awdm::dvec3 delta{0.0};       // на сколько сместился origin (global'ные метры)
    bool shifted = false;         // было ли смещение в этом кадре

    explicit operator bool() const noexcept { return shifted; }
};

class FloatingOrigin {
public:
    // threshold — расстояние от origin до камеры, после которого происходит
    // сдвиг (по ТЗ — 1000 метров). snapToChunk — округлять новый origin до
    // сетки чанков, чтобы границы чанков в локальных координатах не «плыли»
    // между сдвигами (рекомендуется true при chunkSize = размеру меша чанка).
    explicit FloatingOrigin(double thresholdMeters = 1000.0,
                            double snapToChunkSize = 0.0);

    FloatingOrigin(const FloatingOrigin&) = delete;
    FloatingOrigin& operator=(const FloatingOrigin&) = delete;

    // Текущий origin в глобальных координатах мира (double). Потокобезопасно
    // (shared lock) — можно читать из рабочих потоков стриминга чанков.
    awdm::dvec3 origin() const;

    // Смещение origin от глобального центра (0,0,0). То же, что origin().
    awdm::dvec3 offset() const { return origin(); }

    // Проверка порога и (при необходимости) сдвиг origin к камере.
    // Вызывается ОДИН раз за кадр из потока логики. Возвращает delta, если
    // произошёл сдвиг (подписчики должны сдвинуть локальные координаты на
    // -delta), иначе {0, false}.
    OriginShift update(const awdm::dvec3& cameraGlobalPosition);

    // Принудительный сдвиг origin в точку newOrigin (например, при телепорте
    // игрока или загрузке сейва). Возвращает delta так же, как update().
    OriginShift teleportTo(const awdm::dvec3& newOrigin);

    // Сброс origin в (0,0,0) — только для тестов/рестарта мира.
    OriginShift reset();

    // Настройки порога (потокобезопасно).
    void setThreshold(double meters);
    double threshold() const;

    // Расстояние камеры до origin по XZ (для отладки/HUD). Принимает уже
    // посчитанную позицию камеры, состояние не мутирует.
    double distanceTo(const awdm::dvec3& globalPosition) const;

    // Глобальная <-> локальная конверсия через текущий origin (см. awdm).
    awdm::fvec3 toLocal(const awdm::dvec3& global) const;
    awdm::dvec3 toGlobal(const awdm::fvec3& local) const;

    // Сколько раз origin смещался (монотонный счётчик, для логов/телеметрии).
    std::uint64_t shiftCount() const noexcept { return shiftCount_.load(); }

private:
    // Округление точки до сетки чанков (если snapToChunkSize > 0).
    awdm::dvec3 snap(const awdm::dvec3& p) const;

    // Порог срабатывания в метрах (обычно 1000).
    double threshold_;
    // Размер ячейки привязки (0 = без привязки). Immutable после конструктора.
    double snapSize_;

    // Мутируемое состояние под замком.
    mutable std::shared_mutex mutex_;
    awdm::dvec3 origin_{0.0, 0.0, 0.0};

    // Счётчик сдвигов (атомарный — читается без замка из HUD).
    std::atomic<std::uint64_t> shiftCount_{0};
};

}  // namespace core
