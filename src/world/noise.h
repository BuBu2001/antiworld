#pragma once

// Улучшенный градиентный шум Перлина (Ken Perlin, 2002) и его фрактальное
// суммирование (fBm). Реализация своя, без внешних зависимостей — для карты
// высот ландшафта этого достаточно.
//
// Устройство: для ячейки целочисленной сетки выбирается градиент из
// фиксированного набора направлений по хешу ячейки, значение в углу — скалярное
// произведение градиента на смещение относительно этого угла. Интерполяция
// квинтическая (6t^5 - 15t^4 + 10t^3), поэтому шум непрерывен вместе с первой
// производной: поверхность получается гладкой, без ступенек на границах ячеек.

#include <cstdint>

namespace world {

class PerlinNoise {
public:
    // Seed по умолчанию — ландшафт воспроизводится от запуска к запуску.
    static constexpr std::uint32_t kDefaultSeed = 20240517u;

    explicit PerlinNoise(std::uint32_t seed = kDefaultSeed);

    PerlinNoise(const PerlinNoise&) = default;
    PerlinNoise& operator=(const PerlinNoise&) = default;

    // Пересобирает таблицу перестановок из нового seed (тот же seed — тот же
    // шум, поэтому генерация ландшафта детерминирована).
    void reseed(std::uint32_t seed);

    // Шум в точке (x, y), нормирован в [-1, 1].
    // Нечисловые координаты дают 0, чтобы бинарь не уходил в NaN.
    float noise2D(float x, float y) const;

    // === double-путь (обязателен для чанкового мира) ===
    //
    // Мир 510 млн км² => глобальные координаты до ~2.26e7 м на сторону.
    // float32 там имеет ULP ~2 м, а таблица перестановок на 256 ячеек вдобавок
    // ТОЧНО повторяет поле каждые 256 ячеек. Обе проблемы фатальны: рельеф
    // превращается в ступени, а на таком масштабе повторяемость бросается в
    // глаза. Поэтому:
    //   * координаты принимаются в double, индекс ячейки — int64;
    //   * градиент выбирается НЕ из таблицы на 256 элементов, а 64-битным
    //     непериодическим хешем (splitmix64) => поле не повторяется ни в одном
    //     достижимом диапазоне.
    // float-версия выше — тонкая обёртка над этим double-путём, поэтому рельеф
    // всюду в проекте вычисляется ОДНОЙ и той же функцией (иначе стык старого
    // Terrain и новых чанков был бы виден как разрыв в рельефе).
    float noise2D(double x, double y) const;

    // Трёхмерный вариант: z — дополнительная координата (слой ландшафта,
    // направление искажения и т.п.), тоже нормирован в [-1, 1].
    float noise3D(float x, float y, float z) const;

    // Фрактальное суммирование октав: каждая следующая октава повышает
    // частоту в lacunarity раз и понижает амплитуду в gain раз, поэтому
    // крупные формы подавляют мелкие детали. Результат делится на сумму
    // амплитуд и остаётся в [-1, 1].
    //
    // octaves <= 0 даёт 0. gain < 1 «сглаживает» рельеф (меньше деталей),
    // gain >= 1 наоборот подчёркивает шероховатость на всех масштабах.
    float fbm2D(float x, float y, int octaves, float lacunarity = 2.0f,
                float gain = 0.5f) const;

    // double-версия fBm — то, чем реально считается рельеф чанков.
    float fbm2D(double x, double y, int octaves, float lacunarity = 2.0f,
                float gain = 0.5f) const;

private:
    static constexpr int kTableSize = 256;
    static constexpr int kTableMask = kTableSize - 1;
    // Наборы градиентов: 8 направлений в 2D (стороны + диагонали квадрата) и
    // 12 в 3D (рёбра куба) — как в оригинальной реализации Перлина.
    static constexpr int kGradientCount2D = 8;
    static constexpr int kGradientCount3D = 12;

    // Квинтическая интерполяция: f(0) = 0, f(1) = 1, f'(0) = f'(1) = 0.
    static float fade(float t) noexcept;
    static float lerp(float from, float to, float t) noexcept;
    static double fadeD(double t) noexcept;
    static double lerpD(double from, double to, double t) noexcept;

    // Хеш ячейки по таблице перестановок (общий для 2D и 3D).
    int hash(int x, int y) const noexcept;
    int hash(int x, int y, int z) const noexcept;

    // Непериодический 64-битный хеш ячейки решётки (double-путь шума).
    static std::uint32_t hashCell(std::int64_t x, std::int64_t y) noexcept;

    // Скалярное произведение градиента ячейки на смещение от угла до точки.
    float gradient2D(int cell, float dx, float dy) const noexcept;
    float gradient3D(int cell, float dx, float dy, float dz) const noexcept;
    static double gradient2D(std::uint32_t cell, double dx, double dy) noexcept;

    // Таблица перестановок П (значения 0..255, каждый встречается один раз);
    // хеш — обычная формула П[x & 255] + y, всё приводится по маске.
    std::uint8_t permutation_[kTableSize]{};
};

}  // namespace world
