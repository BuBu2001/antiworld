#include "world/climatology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

#include "core/logger.h"

namespace world {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kDegToRad = kTwoPi / 360.0;
constexpr double kRadToDeg = 360.0 / kTwoPi;

// Плавная ступень 0..1, кламплющая оба края. Без неё на границах климатических
// поясов были бы видны прямые линии — как раз то, чего климат не имеет.
double smoothstep(double edge0, double edge1, double value) {
    if (!(edge1 > edge0)) {
        return value < edge0 ? 0.0 : 1.0;
    }
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Линейная интерполяция, кламплющая t в 0..1.
double mix(double a, double b, double t) {
    return a + (b - a) * std::clamp(t, 0.0, 1.0);
}

}  // namespace

std::unique_ptr<Climatology> Climatology::build(const TerrainGenerator::Config& terrain,
                                                std::uint32_t seed) {
    return build(terrain, seed, Config{});
}

std::unique_ptr<Climatology> Climatology::build(const TerrainGenerator::Config& terrain,
                                                std::uint32_t seed, Config config) {
    const auto& kMin = config;
    if (kMin.width < 8 || kMin.height < 8) {
        throw std::invalid_argument("Climatology: сетка меньше 8x8 не имеет смысла");
    }
    if (!std::isfinite(config.extent) || config.extent <= 0.0) {
        throw std::invalid_argument("Climatology: размер мира должен быть положительным");
    }
    // Климат и рельеф обязаны считать ОДНО И ТО ЖЕ поле шума. Если у них
    // разные extent, то частоты дают разные периоды, и карта покажет
    // материки там, где их нет, а шов по X разойдётся. Раньше это
    // диагностировалось только по симптому (карта врёт), теперь — сразу.
    if (std::abs(config.extent - terrain.worldExtent) > 1.0) {
        throw std::invalid_argument(
            "Climatology: extent климата (" + std::to_string(config.extent) +
            ") не совпадает с extent рельефа (" + std::to_string(terrain.worldExtent) +
            "): карта и мир разойдутся");
    }
    if (config.lapseRate < 0.0) {
        throw std::invalid_argument("Climatology: градиент температуры по высоте отрицателен");
    }
    if (!(config.verticalMetersPerUnit > 0.0) || !std::isfinite(config.verticalMetersPerUnit)) {
        throw std::invalid_argument(
            "Climatology: вертикальный масштаб рельефа должен быть положительным");
    }

    std::unique_ptr<Climatology> field(new Climatology());
    field->config_ = config;
    field->seed_ = seed;
    field->terrain_ = terrain;

    const std::size_t count = static_cast<std::size_t>(config.width) *
                              static_cast<std::size_t>(config.height);
    field->height_.assign(count, 0.0f);
    field->distance_.assign(count, 0.0f);
    field->temperature_.assign(count, 0.0f);
    field->swing_.assign(count, 0.0f);
    field->precipitation_.assign(count, 0.0f);
    field->humidity_.assign(count, 0.0f);
    field->seaLevel_.assign(count, 0.0f);
    field->ice_.assign(count, 0.0f);
    field->seaIce_.assign(count, 0.0f);
    field->snowLine_.assign(count, 0.0f);

    field->computeHeights();
    field->computeDistanceToOcean();
    field->computeTemperature();
    field->computePrecipitation();
    field->computeHumidity();
    field->computeIce();
    field->computeSeaLevel();
    return field;
}

// ============================ построение полей ============================

void Climatology::computeHeights() {
    // Высоты — самая дорогая часть: в каждой ячейке две суммы fBm на 9 и 3
    // октавы. Строки независимы, поэтому берём все ядра: 512x512 = 262 тысячи
    // вызовов по одному потоку занимают почти секунду, по восьми — доли секунды.
    // Потоки только пишут в СВОИ строки, синхронизации не нужно.
    const std::uint32_t rows = config_.height;
    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) {
        workers = 4;
    }
    workers = std::min<unsigned>(workers, rows);
    workers = std::max(1u, workers);

    const TerrainGenerator::Config terrain = terrain_;
    const std::uint32_t seed = seed_;
    const double extent = config_.extent;
    const std::uint32_t width = config_.width;
    std::vector<float>* const heights = &height_;

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        const std::uint32_t begin = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(rows) * w / workers);
        const std::uint32_t end = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(rows) * (w + 1) / workers);
        threads.emplace_back([=, heights]() {
            const double half = 0.5 * extent;
            for (std::uint32_t j = begin; j < end; ++j) {
                const double wz = -half + extent * (static_cast<double>(j) + 0.5) /
                                             static_cast<double>(rows);
                for (std::uint32_t i = 0; i < width; ++i) {
                    const double wx = -half + extent * (static_cast<double>(i) + 0.5) /
                                                 static_cast<double>(width);
                    // Период по X: ровно тот, что и у рельефа в 3D, иначе
                    // климат получился бы «сдвинутой» копией мира.
                    (*heights)[static_cast<std::size_t>(j) * width + i] =
                        static_cast<float>(TerrainGenerator::sampleHeightAt(
                            terrain, seed, wx, wz, extent));
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

void Climatology::computeDistanceToOcean() {
    // Расстояние до океана в метрах: два прохода «шахматной» метрики по
    // сетке 3x3. Это то, что даёт материальность, а через неё — и сухую
    // холодную Сибирь, и влажные западные побережья.
    //
    // Мир цикличен по X (соседи заворачиваются), по Z полюса не заворачиваются:
    // за полюсом «соседа» нет, иначе расстояние считалось бы до несуществующей
    // земли.
    const std::uint32_t width = config_.width;
    const std::uint32_t rows = config_.height;
    const double cell = config_.extent / static_cast<double>(std::max(width, rows));
    const double diag = cell * std::sqrt(2.0);
    const double seaLevel = terrain_.geography.enabled ? terrain_.geography.seaLevel : 0.0;
    const double infinite = std::numeric_limits<double>::infinity();

    std::size_t landCells = 0;
    for (std::uint32_t j = 0; j < rows; ++j) {
        for (std::uint32_t i = 0; i < width; ++i) {
            const std::size_t at = index(i, j);
            if (static_cast<double>(height_[at]) <= seaLevel) {
                distance_[at] = 0.0f;
            } else {
                distance_[at] = static_cast<float>(infinite);
                ++landCells;
            }
        }
    }
    const double cells = static_cast<double>(width) * static_cast<double>(rows);
    landFraction_ = cells > 0.0 ? static_cast<double>(landCells) / cells : 0.0;

    const auto wrapX = [width](std::int64_t i) {
        std::int64_t w = i % static_cast<std::int64_t>(width);
        if (w < 0) {
            w += static_cast<std::int64_t>(width);
        }
        return static_cast<std::uint32_t>(w);
    };

    // Прямой проход: соседи сверху и слева (и диагонали).
    for (std::uint32_t j = 0; j < rows; ++j) {
        for (std::uint32_t i = 0; i < width; ++i) {
            const std::size_t at = index(i, j);
            double best = static_cast<double>(distance_[at]);
            const std::int64_t left = static_cast<std::int64_t>(i) - 1;
            const std::int64_t right = static_cast<std::int64_t>(i) + 1;
            if (j > 0) {
                const std::size_t up = index(i, j - 1);
                best = std::min(best, static_cast<double>(distance_[up]) + cell);
                best = std::min(best, static_cast<double>(distance_[index(wrapX(left), j - 1)]) + diag);
                best = std::min(best, static_cast<double>(distance_[index(wrapX(right), j - 1)]) + diag);
            }
            best = std::min(best, static_cast<double>(distance_[index(wrapX(left), j)]) + cell);
            distance_[at] = static_cast<float>(best);
        }
    }
    // Обратный проход: соседи снизу и справа. Вместе с прямым это даёт
    // расстояние до ближайшей воды по всей карте, а не только «до воды выше».
    for (std::int64_t jj = static_cast<std::int64_t>(rows) - 1; jj >= 0; --jj) {
        const std::uint32_t j = static_cast<std::uint32_t>(jj);
        for (std::int64_t ii = static_cast<std::int64_t>(width) - 1; ii >= 0; --ii) {
            const std::uint32_t i = static_cast<std::uint32_t>(ii);
            const std::size_t at = index(i, j);
            double best = static_cast<double>(distance_[at]);
            const std::int64_t left = static_cast<std::int64_t>(i) - 1;
            const std::int64_t right = static_cast<std::int64_t>(i) + 1;
            if (jj + 1 < static_cast<std::int64_t>(rows)) {
                const std::size_t down = index(i, j + 1);
                best = std::min(best, static_cast<double>(distance_[down]) + cell);
                best = std::min(best, static_cast<double>(distance_[index(wrapX(left), j + 1)]) + diag);
                best = std::min(best, static_cast<double>(distance_[index(wrapX(right), j + 1)]) + diag);
            }
            best = std::min(best, static_cast<double>(distance_[index(wrapX(right), j)]) + cell);
            distance_[at] = static_cast<float>(best);
        }
    }

    // На всякий случай: если океана нет вообще (сплошной материк), расстояние
    // осталось бы бесконечным, и любой делитель по нему дал бы NaN.
    const double fallback = config_.extent * 2.0;
    for (float& value : distance_) {
        if (!std::isfinite(value)) {
            value = static_cast<float>(fallback);
        }
    }
}

double Climatology::continentalityFromDistance(double distance) const {
    if (!(config_.continentalRange > 0.0)) {
        return 0.0;
    }
    // Степень 0.8: материк становится «глубоким» не линейно, а с насыщением —
    // уже в 600 км от берега зима заметно злее, а в 2000 км злее в разы.
    return std::pow(std::clamp(distance / config_.continentalRange, 0.0, 1.0), 0.8);
}

double Climatology::currentAt(double latitudeDegrees) const {
    // Тепло, которое гонят широтные течения. Нормировка такая, что у экватора
    // поправка близка к currentWarmth, к 70° обнуляется, а выше океан даже
    // холоднее широтного среднего (полярное течение).
    const double cosLat = std::cos(std::abs(latitudeDegrees) * kDegToRad);
    const double reference = std::cos(70.0 * kDegToRad);
    return config_.currentWarmth * (std::pow(std::max(cosLat, 0.0), 1.5) - reference);
}

double Climatology::windFromSign(double latitudeDegrees, double subtropicalLatitude) {
    // Пассаты (тропики) дуют С востока: наветренная сторона +X. В умеренных
    // широтах западные ветры дуют С запада: наветренная сторона -X.
    return std::abs(latitudeDegrees) < subtropicalLatitude ? 1.0 : -1.0;
}

double Climatology::upwindHeight(double wx, double wz, double latitudeDegrees) const {
    // Наветренная высота. Смотрим на 350 км в ту сторону, откуда дует ветер:
    // если там горы на километр выше, точка в дождевой тени и осадков мало.
    const double sign = windFromSign(latitudeDegrees, config_.subtropicalLatitude);
    return static_cast<double>(TerrainGenerator::sampleHeightAt(
        terrain_, seed_, wx + sign * config_.rainShadowDistance, wz, config_.extent));
}

void Climatology::computeTemperature() {
    const std::uint32_t width = config_.width;
    const std::uint32_t rows = config_.height;
    const double half = 0.5 * config_.extent;

    double sumT = 0.0;
    for (std::uint32_t j = 0; j < rows; ++j) {
        const double wz = -half + config_.extent * (static_cast<double>(j) + 0.5) /
                                     static_cast<double>(rows);
        const double latitude = latitudeAt(wz);
        const double absLat = std::abs(latitude);

        // Широтный профиль: от экватора к полюсу, с выпуклостью
        // latitudeExponent. Именно она делает средние широты холоднее, чем
        // дала бы линейная зависимость, — то есть тундрой, а не тайгой.
        const double latT = config_.equatorTemperature -
                            (config_.equatorTemperature - config_.polarTemperature) *
                                std::pow(absLat / 90.0, config_.latitudeExponent);
        const double current = currentAt(latitude);

        for (std::uint32_t i = 0; i < width; ++i) {
            const std::size_t at = index(i, j);
            const double distance = static_cast<double>(distance_[at]);
            // Морское влияние: полное у берега, сходящее на нет в глубине.
            const double maritime = config_.currentDecay > 0.0
                                        ? std::exp(-distance / config_.currentDecay)
                                        : 0.0;
            const double continental = continentalityFromDistance(distance);

            double t = latT;
            t += current * maritime;                 // тёплые течения
            t -= config_.continentalCooling * continental;  // континентальность
            // Высота: тёплое море — на нуле, суша выше уровня моря холодает.
            // lapseRate задан в ℃ на РЕАЛЬНЫЙ метр, а рельеф — в мировых
            // единицах, поэтому переводим через verticalMetersPerUnit.
            t -= config_.lapseRate * config_.verticalMetersPerUnit *
                 std::max(0.0, static_cast<double>(height_[at]));
            temperature_[at] = static_cast<float>(t);

            // Полуразмах сезона: у полюса сезон длиннее и холоднее, в глубине
            // материка размах резко растёт, море гасит размах. Именно большой
            // размах вместе со средней, близкой к нулю, и даёт «-35 зимой,
            // +20 летом» — то есть континентальный климат, а не тундру.
            double swing = 2.0 + 26.0 * std::pow(absLat / 90.0, 1.4);
            swing += config_.continentalSwing * continental;
            swing -= 6.0 * maritime;
            swing_[at] = static_cast<float>(std::clamp(swing, 0.5, 42.0));

            sumT += t;
        }
    }
    const double count = static_cast<double>(width) * static_cast<double>(rows);
    meanTemperature_ = count > 0.0 ? sumT / count : 0.0;
}

void Climatology::computePrecipitation() {
    // Осадки: сначала ячейки Хедли и полярного фронта (зависят только от
    // широты), потом источник влаги (океан против глубины материка), потом
    // орография (наветренный склон мокрый, подветренный сухой).
    const std::uint32_t width = config_.width;
    const std::uint32_t rows = config_.height;
    const double half = 0.5 * config_.extent;
    const double sub = config_.subtropicalLatitude;
    const double mid = config_.midLatitude;

    for (std::uint32_t j = 0; j < rows; ++j) {
        const double wz = -half + config_.extent * (static_cast<double>(j) + 0.5) /
                                     static_cast<double>(rows);
        const double latitude = latitudeAt(wz);
        const double absLat = std::abs(latitude);

        // Профиль по широте: максимум на экваторе, минимум в субтропиках,
        // второй максимум у полярного фронта, спад к полюсу.
        double cell;
        if (absLat <= sub) {
            cell = mix(config_.precipitationEquator, config_.precipitationSubtropical,
                       smoothstep(0.0, sub, absLat));
        } else if (absLat <= mid) {
            cell = mix(config_.precipitationSubtropical, config_.precipitationMidLat,
                       smoothstep(sub, mid, absLat));
        } else {
            cell = mix(config_.precipitationMidLat, config_.precipitationPolar,
                       smoothstep(mid, 85.0, absLat));
        }

        for (std::uint32_t i = 0; i < width; ++i) {
            const std::size_t at = index(i, j);
            const double distance = static_cast<double>(distance_[at]);
            const double continental = continentalityFromDistance(distance);
            const double maritime = config_.currentDecay > 0.0
                                        ? std::exp(-distance / config_.currentDecay)
                                        : 0.0;

            // Влага не доходит вглубь континента: это и есть главная причина
            // сухости внутренних районов Евразии и Северной Америки.
            double p = cell * (1.0 - config_.continentalDryness * continental);
            // Тёплое море испаряет больше: множитель 0.75..1.
            const double t = static_cast<double>(temperature_[at]);
            p *= 0.75 + 0.25 * std::clamp((t + 10.0) / 35.0, 0.0, 1.0);

            // Орография. Сравниваем с наветренной высотой: выше наветренной —
            // склон поднимает воздух, идут дожди; ниже — воздух уже всё отдал,
            // и стоит дождевая тень.
            const double here = std::max(0.0, static_cast<double>(height_[at]));
            const double upwind = std::max(0.0, upwindHeight(
                                                  -half + config_.extent *
                                                          (static_cast<double>(i) + 0.5) /
                                                          static_cast<double>(width),
                                                  wz, latitude));
            // Подъём в километрах РЕАЛЬНОЙ высоты: рельеф в мировых единицах,
            // поэтому снова нужен пересчёт. Без него дождевая тень не читается
            // вовсе: orographicGain умножался бы на 0.01 вместо 0.6.
            const double rise = (here - upwind) * config_.verticalMetersPerUnit / 1000.0;
            p *= rise > 0.0 ? 1.0 + config_.orographicGain * rise
                            : std::exp(config_.orographicGain * rise);
            // В глубине океана орография не должна ничего давать: там ровно.
            p *= 0.9 + 0.1 * (1.0 - maritime);

            precipitation_[at] = static_cast<float>(std::clamp(p, 0.0, 1.5));
        }
    }
}

void Climatology::computeHumidity() {
    const std::size_t count = height_.size();
    for (std::size_t at = 0; at < count; ++at) {
        const double distance = static_cast<double>(distance_[at]);
        const double continental = continentalityFromDistance(distance);
        const double maritime = config_.currentDecay > 0.0
                                    ? std::exp(-distance / config_.currentDecay)
                                    : 0.0;
        const double p = std::min(1.0, std::max(0.0, static_cast<double>(precipitation_[at])));
        // Влажность = база + осадки + близость моря - континентальность.
        // Осадки доминируют: без них влажность в глубине континента всё равно
        // низкая, а над морем она высокая даже в пустынях широтных поясов.
        double rh = config_.humidityBase + config_.humidityPrecipitation * p +
                    config_.humidityOcean * maritime - config_.humidityContinental * continental;
        humidity_[at] = static_cast<float>(std::clamp(rh, 0.02, 1.0));
    }
}

void Climatology::computeIce() {
    // Лёд требует трёх вещей сразу: холода, высоты (выше снеговой линии) и
    // осадков. Одного холода мало — в Сибири зимой -35, а ледников почти нет,
    // потому что сухо. Одних осадков мало — в тропиках ливни, а ледников нет.
    const std::size_t count = height_.size();
    const double warm = config_.iceTemperature + config_.iceTemperatureRange;
    const double floorP = config_.icePrecipitationFloor;

    for (std::size_t at = 0; at < count; ++at) {
        const double t = static_cast<double>(temperature_[at]);
        const double p = std::max(0.0, std::min(1.0, static_cast<double>(precipitation_[at])));
        const double h = static_cast<double>(height_[at]);

        // Снеговая линия: от экватора к полюсу, и ниже во влажных местах
        // (влажный снег сходит ниже сухого пыльного).
        const double line = std::max(
            0.0, config_.snowLineEquator *
                     std::pow(std::clamp(1.0 - std::abs(latitudeAtCell(at)) / 90.0, 0.0, 1.0), 1.2) -
                     config_.snowLinePrecipitation * p);
        snowLine_[at] = static_cast<float>(line);

        const double coldness = 1.0 - smoothstep(config_.iceTemperature, warm, t);
        // Высота в метрах: снеговая линия посчитана в метрах, а рельеф — в
        // мировых единицах. Полметра погрешности не должны решать, ледник там
        // или нет.
        const double heightMeters = h * config_.verticalMetersPerUnit;
        const double altitude = std::clamp((heightMeters - line) / 500.0 + 0.5, 0.0, 1.0);
        const double snowy = smoothstep(floorP, floorP + 0.24, p);
        ice_[at] = static_cast<float>(std::clamp(coldness * altitude * snowy, 0.0, 1.0));

        // Морской лёд: только на воде и только в настоящий холод.
        const bool ocean = h <= (terrain_.geography.enabled ? terrain_.geography.seaLevel : 0.0);
        if (ocean) {
            const double freeze = 1.0 - smoothstep(config_.seaIceTemperature - config_.seaIceRange,
                                                   config_.seaIceTemperature, t);
            seaIce_[at] = static_cast<float>(std::clamp(freeze, 0.0, 1.0));
        } else {
            seaIce_[at] = 0.0f;
        }
    }

    // Доля суши подо льдом: по ней считается мировой объём ледников, а из него
    // — глобальное падение уровня моря.
    const double total = static_cast<double>(count);
    double land = 0.0;
    double iced = 0.0;
    const double seaLevel = terrain_.geography.enabled ? terrain_.geography.seaLevel : 0.0;
    for (std::size_t at = 0; at < count; ++at) {
        if (static_cast<double>(height_[at]) > seaLevel) {
            land += 1.0;
            iced += static_cast<double>(ice_[at]);
        }
    }
    iceAreaFraction_ = land > 0.0 ? iced / land : 0.0;
}

void Climatology::computeSeaLevel() {
    // Уровень моря складывается из двух вещей.
    //
    // 1) Тепловое расширение, и оно НЕ постоянно: тёплый океан стоит выше
    //    холодного на десятки сантиметров, поэтому уровень — поле, а не
    //    константа. Влияние моря затухает вглубь суши: в глубине континента
    //    «уровень моря» — понятие условное, и там остаётся только общий сдвиг.
    // 2) Ледниковый объём: вода, ушедшая в лёд, уровень понижает. Считается
    //    по фактической площади ледников: чем больше льда, тем ниже океан.
    const std::size_t count = height_.size();
    seaLevelGlobalOffset_ =
        -config_.seaLevelFromIce * std::clamp(iceAreaFraction_ / 0.15, 0.0, 2.0);

    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (std::size_t at = 0; at < count; ++at) {
        const double distance = static_cast<double>(distance_[at]);
        const double maritime = config_.currentDecay > 0.0
                                    ? std::exp(-distance / config_.currentDecay)
                                    : 0.0;
        const double thermal = config_.seaLevelPerDegree *
                               (static_cast<double>(temperature_[at]) -
                                config_.seaLevelReferenceTemperature) *
                               maritime;
        const double value =
            seaLevelGlobalOffset_ + std::clamp(thermal, -config_.seaLevelLimit,
                                               config_.seaLevelLimit);
        seaLevel_[at] = static_cast<float>(value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        sum += value;
    }
    seaLevelMin_ = minValue;
    seaLevelMax_ = maxValue;
    seaLevelMean_ = count > 0 ? sum / static_cast<double>(count) : 0.0;
}

// ============================ запросы ============================

double Climatology::latitudeAt(double wz) const noexcept {
    const double half = 0.5 * config_.extent;
    if (!(half > 0.0)) {
        return 0.0;
    }
    // Мир натянут на сферу, поэтому широта это НЕ wz/half, а arcsin: полюса
    // должны сжимать высокие широты, иначе у полюсов не будет холода.
    const double ratio = std::clamp(wz / half, -1.0, 1.0);
    return std::asin(ratio) * kRadToDeg;
}

double Climatology::latitudeAtCell(std::size_t at) const {
    const std::uint32_t rows = config_.height;
    const double half = 0.5 * config_.extent;
    const std::uint32_t j = static_cast<std::uint32_t>(at / config_.width);
    const double wz = -half + config_.extent * (static_cast<double>(j) + 0.5) /
                                 static_cast<double>(rows);
    return latitudeAt(wz);
}

double Climatology::sample(const std::vector<float>& grid, double wx, double wz) const {
    if (grid.empty()) {
        return 0.0;
    }
    const double half = 0.5 * config_.extent;
    const double W = static_cast<double>(config_.width);
    const double H = static_cast<double>(config_.height);

    // X цикличен: приводим индекс в [0, W), сохраняя дробную часть.
    double fx = (wx + half) / config_.extent * W - 0.5;
    const double fxWrapped = std::fmod(fx, W);
    if (fxWrapped < 0.0) {
        fx = fxWrapped + W;
    } else {
        fx = fxWrapped;
    }
    const double floorX = std::floor(fx);
    const double tx = fx - floorX;
    const std::int64_t widthI = static_cast<std::int64_t>(config_.width);
    const std::int64_t i0 = static_cast<std::int64_t>(floorX) % widthI;
    const std::int64_t i1 = (i0 + 1) % widthI;

    // Z зажат полюсами. За полюсом мира нет, и на самом полюсе интервала нет
    // тоже: там берётся одна крайняя ячейка, а не половина интервала.
    const double fz = (std::clamp(wz, -half, half) + half) / config_.extent * H - 0.5;
    std::int64_t j0 = 0;
    std::int64_t j1 = 0;
    double tz = 0.0;
    if (fz <= 0.0) {
        j0 = 0;
        j1 = 0;
    } else if (fz >= H - 1.0) {
        j0 = static_cast<std::int64_t>(H) - 1;
        j1 = j0;
    } else {
        const double floorZ = std::floor(fz);
        j0 = static_cast<std::int64_t>(floorZ);
        j1 = j0 + 1;
        tz = fz - floorZ;
    }

    const double v00 = grid[index(static_cast<std::uint32_t>(i0), static_cast<std::uint32_t>(j0))];
    const double v10 = grid[index(static_cast<std::uint32_t>(i1), static_cast<std::uint32_t>(j0))];
    const double v01 = grid[index(static_cast<std::uint32_t>(i0), static_cast<std::uint32_t>(j1))];
    const double v11 = grid[index(static_cast<std::uint32_t>(i1), static_cast<std::uint32_t>(j1))];
    return mix(mix(v00, v10, tx), mix(v01, v11, tx), tz);
}

float Climatology::heightAt(double wx, double wz) const {
    return static_cast<float>(sample(height_, wx, wz));
}

double Climatology::distanceToOceanAt(double wx, double wz) const {
    return sample(distance_, wx, wz);
}

double Climatology::continentalityAt(double wx, double wz) const {
    return continentalityFromDistance(sample(distance_, wx, wz));
}

double Climatology::temperatureAt(double wx, double wz) const {
    return sample(temperature_, wx, wz);
}

double Climatology::seasonalSwingAt(double wx, double wz) const {
    return sample(swing_, wx, wz);
}

double Climatology::precipitationAt(double wx, double wz) const {
    return sample(precipitation_, wx, wz);
}

double Climatology::precipitationMillimetersAt(double wx, double wz) const {
    return precipitationAt(wx, wz) * config_.precipitationMillimeters;
}

double Climatology::humidityAt(double wx, double wz) const {
    return sample(humidity_, wx, wz);
}

double Climatology::seaLevelAt(double wx, double wz) const {
    return sample(seaLevel_, wx, wz);
}

double Climatology::seaLevelWorldUnitsAt(double wx, double wz) const {
    if (!(config_.verticalMetersPerUnit > 0.0)) {
        return 0.0;
    }
    return seaLevelAt(wx, wz) / config_.verticalMetersPerUnit;
}

double Climatology::iceAt(double wx, double wz) const {
    return sample(ice_, wx, wz);
}

double Climatology::seaIceAt(double wx, double wz) const {
    return sample(seaIce_, wx, wz);
}

double Climatology::snowLineAt(double wx, double wz) const {
    return sample(snowLine_, wx, wz);
}

double Climatology::snowLineWorldUnitsAt(double wx, double wz) const {
    if (!(config_.verticalMetersPerUnit > 0.0)) {
        return 0.0;
    }
    return snowLineAt(wx, wz) / config_.verticalMetersPerUnit;
}

bool Climatology::southernHemisphere(double wz) const noexcept {
    return wz < 0.0;
}

}  // namespace world
