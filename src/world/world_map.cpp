#include "world/world_map.h"

#include "core/logger.h"
#include "world/climatology.h"

#include <algorithm>
#include <chrono>

namespace world {
namespace {

// Смешивание цветов каналами.
glm::vec3 mix(const glm::vec3& a, const glm::vec3& b, double t) {
    return a + (b - a) * static_cast<float>(std::clamp(t, 0.0, 1.0));
}

// Цвет океана: от мелководья к глубокой воде.
glm::vec3 oceanColor(double depthFraction) {
    const glm::vec3 shallow{0.16f, 0.55f, 0.68f};
    const glm::vec3 mid{0.05f, 0.24f, 0.52f};
    const glm::vec3 deep{0.01f, 0.05f, 0.20f};
    if (depthFraction < 0.35) {
        return mix(shallow, mid, depthFraction / 0.35);
    }
    return mix(mid, deep, (depthFraction - 0.35) / 0.65);
}

// Цвет суши: песок у воды -> зелень -> склоны/скалы на высоте.
glm::vec3 landColor(double heightFraction) {
    const glm::vec3 sand{0.76f, 0.70f, 0.52f};
    const glm::vec3 grass{0.24f, 0.45f, 0.22f};
    const glm::vec3 dryGrass{0.45f, 0.48f, 0.24f};
    const glm::vec3 rock{0.42f, 0.40f, 0.38f};
    if (heightFraction < 0.06) {
        return mix(sand, grass, heightFraction / 0.06);
    }
    if (heightFraction < 0.55) {
        return mix(grass, dryGrass, (heightFraction - 0.06) / 0.49);
    }
    return mix(dryGrass, rock, (heightFraction - 0.55) / 0.45);
}

// --- Карта по климату ---
//
// Высотная раскраска показывает РЕЛЬЕФ, но не показывает главное: суша может
// быть зелёной от материковой сырости и жёлтой от океанической, а это разные
// миры. Климатическая раскраска отвечает на другой вопрос — «как здесь
// живётся» — и читается как настоящая карта погоды/климата.

constexpr double kPi = 3.14159265358979323846;

// Синий -> зелёный -> жёлтый -> красный: универсальная шкала «мало -> много».
glm::vec3 heatRamp(double value) {
    const glm::vec3 cold{0.05f, 0.15f, 0.55f};
    const glm::vec3 mild{0.10f, 0.55f, 0.30f};
    const glm::vec3 warm{0.85f, 0.80f, 0.25f};
    const glm::vec3 hot{0.80f, 0.18f, 0.10f};
    const double t = std::clamp(value, 0.0, 1.0);
    if (t < 0.4) {
        return mix(cold, mild, t / 0.4);
    }
    if (t < 0.75) {
        return mix(mild, warm, (t - 0.4) / 0.35);
    }
    return mix(warm, hot, (t - 0.75) / 0.25);
}

// Оттенки земли: тропики зелёные, пустыни песочные, тайга тёмная, тундра
// серо-зелёная, лёд белый.
glm::vec3 biomeTint(double temperature, double precipitation) {
    const glm::vec3 tropic{0.16f, 0.42f, 0.18f};
    const glm::vec3 temperate{0.22f, 0.45f, 0.20f};
    const glm::vec3 taiga{0.14f, 0.30f, 0.17f};
    const glm::vec3 tundra{0.42f, 0.44f, 0.36f};
    const glm::vec3 ice{0.88f, 0.92f, 0.96f};
    const glm::vec3 desert{0.80f, 0.72f, 0.48f};

    if (temperature < -8.0) {
        return ice;
    }
    if (temperature < 2.0) {
        return mix(tundra, taiga, (temperature + 8.0) / 10.0);
    }
    // Суше и жарко — пустыня, но только если это не горы: их оттенок задаёт
    // высота, которая приходит отдельным аргументом.
    if (precipitation < 0.25 && temperature > 12.0) {
        return desert;
    }
    return temperature < 14.0 ? mix(temperate, tropic, (temperature - 14.0) / 13.0)
                              : mix(temperate, tropic, 1.0 - (temperature - 14.0) / 13.0);
}

// Лёд на суше: белый, но не чисто белый — с голубым оттенком, как настоящий
// ледник в тени.
glm::vec3 iceTint() { return glm::vec3{0.88f, 0.93f, 0.98f}; }

// Снег/лёд на море: заметно площе, чем на суше, и с серым оттенком.
glm::vec3 seaIceTint() { return glm::vec3{0.70f, 0.78f, 0.84f}; }

glm::vec3 climateMapColor(const Climatology& climate, double wx, double wz, double height,
                          double seaLevel, double oceanDepth, double maxLand) {
    const double temperature = climate.temperatureAt(wx, wz);
    const double precipitation = climate.precipitationAt(wx, wz);
    const double ice = climate.iceAt(wx, wz);
    const double seaIce = climate.seaIceAt(wx, wz);

    if (height < seaLevel) {
        // Океан: чем холоднее, тем светлее (полярный лёд), плюс оттенок по
        // глубине, как в heightColor.
        glm::vec3 water = oceanColor((seaLevel - height) / oceanDepth);
        if (seaIce > 0.01) {
            water = mix(water, seaIceTint(), seaIce);
        }
        return water;
    }

    glm::vec3 color = biomeTint(temperature, precipitation);
    // Скалы на высоте: тот же приём, что и в heightColor, но мягче — на климатической
    // карте высота не главный сюжет.
    const double altitude = (height - seaLevel) / maxLand;
    color = mix(color, glm::vec3{0.45f, 0.43f, 0.41f}, std::clamp((altitude - 0.55) / 0.45, 0.0, 1.0));
    if (ice > 0.01) {
        color = mix(color, iceTint(), ice);
    }
    return color;
}

}  // namespace

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed)
    : WorldMap(terrain, seed, Config{}, nullptr) {}

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed, Config cfg)
    : WorldMap(terrain, seed, std::move(cfg), nullptr) {}

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed,
                   const Climatology* climatology)
    : WorldMap(terrain, seed, Config{}, climatology) {}

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed, Config cfg,
                   const Climatology* climatology)
    : terrain_(terrain), seed_(seed), config_(std::move(cfg)), climatology_(climatology) {
    pixels_.reserve(static_cast<std::size_t>(config_.width) * config_.height * 4);

    // Шов карты сомкнётся ТОЧНО, если extent*scale и extent*continentScale —
    // ЧЁТНЫЕ целые числа ячеек решётки. Молча пропустить это нельзя: получится
    // ровно тот дефект, ради которого добавлена периодичность, а диагностировать
    // его потом нечем — край карты просто выглядит «склеенным».
    //
    // Почему именно чётное: координата края (x/extent)*periodCells на x=±extent/2
    // равна ±periodCells/2. При нечётном periodCells это полуцелое, и дробные
    // части решётки с двух сторон края совпадают лишь с точностью до последнего
    // бита double — на практике шов прыгал на 0.08 м. При чётном это целое
    // число, поэтому совпадение побитовое.
    //
    // Критерий не «равно целому», а «остаток меньше сотой пикселя карты».
    // Точность не обязана быть абсолютной: остаток в ячейках превращается в
    // остаток по долготе, и важен он ровно настолько, насколько заметен.
    // Нечётность при этом не прощается никогда: это уже заметный дефект.
    const double pixel = config_.extent / static_cast<double>(config_.width);
    const auto check = [this, pixel](const char* name, double scale) {
        const double cells = config_.extent * scale;
        const std::int64_t period = std::llround(cells);
        const double residualMeters = std::abs(cells - static_cast<double>(period)) / scale;
        if (period <= 0) {
            core::Logger::log(core::LogLevel::Warn,
                              std::string("WorldMap: ") + name +
                                  " даёт неположительный период, карта не сомкнётся");
            return;
        }
        if (period % 2 != 0) {
            core::Logger::log(
                core::LogLevel::Warn,
                std::string("WorldMap: ") + name + " даёт нечётный период " +
                    std::to_string(period) +
                    " ячеек, край карты будет со швом; сделай extent*scale чётным");
        }
        if (residualMeters > pixel * 0.01) {
            core::Logger::log(
                core::LogLevel::Warn,
                "WorldMap: остаток периода по " + std::string(name) + " = " +
                    std::to_string(static_cast<int>(residualMeters)) + " м (" +
                    std::to_string(static_cast<int>(residualMeters / pixel)) +
                    " пикселей карты), шов не сомкнётся");
        }
    };
    check("scale", terrain_.scale);
    check("continentScale", terrain_.geography.continentScale);
}

WorldMap::~WorldMap() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void WorldMap::start() {
    if (worker_.joinable() || ready_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() {
        generate();
        running_.store(false, std::memory_order_release);
        // Пиксели публикуются ПОСЛЕ записи: release/acquire в ready_ делает
        // их видимыми главному потоку (иначе он прочитал бы полупустой буфер).
        ready_.store(true, std::memory_order_release);
    });
}

void WorldMap::generate() {
    const auto started = std::chrono::steady_clock::now();

    const std::size_t width = config_.width;
    const std::size_t height = config_.height;
    std::vector<std::uint8_t> pixels(width * height * 4);

    const double half = 0.5 * config_.extent;
    const double seaLevel = terrain_.geography.enabled ? terrain_.geography.seaLevel : 0.0;
    const double oceanDepth = std::max(1e-3f, terrain_.geography.oceanDepth);
    const double maxLand = std::max(1e-3f, terrain_.geography.maxLandHeight);

    // Климатическая карта и карта по высоте дают разные ответы на вопрос
    // «суша здесь или нет», если уровень моря стал полем: подтопленный берег
    // климатически океанический, но высотой всё ещё суша. Считаем обе маски
    // и берём по климатической — иначе карта и ландшафт разойдутся.
    const double climatologyBaseSea =
        terrain_.geography.enabled ? terrain_.geography.seaLevel : 0.0;

    std::size_t landPixels = 0;
    for (std::size_t j = 0; j < height; ++j) {
        // v растёт по +Z, а изображение строится сверху вниз, поэтому первая
        // строка — это МИНИМАЛЬНЫЙ Z. Иначе карта была бы ещё и перевёрнутой
        // по вертикали относительно маркера игрока.
        const double wz = -half + config_.extent * (static_cast<double>(j) + 0.5) /
                                     static_cast<double>(height);
        for (std::size_t i = 0; i < width; ++i) {
            const double wx = -half + config_.extent * (static_cast<double>(i) + 0.5) /
                                         static_cast<double>(width);
            // Ровно та же функция, что строит рельеф чанков, плюс период по X:
            // карта натягивается на сферу, где левый и правый край — один и тот
            // же меридиан, поэтому рельеф по X обязан быть периодическим.
            const float h = TerrainGenerator::sampleHeightAt(terrain_, seed_, wx, wz,
                                                              config_.extent);

            glm::vec3 color;
            bool isLand;
            if (climatology_ != nullptr) {
                // Локальный уровень моря: в метрах, и в тех же единицах, что
                // h. Тёплый океан выше холодного, поэтому берег от климата
                // немного «плавает» — так же, как в ландшафте.
                const double localSea = climatologyBaseSea +
                                        climatology_->seaLevelWorldUnitsAt(wx, wz);
                color = climateMapColor(*climatology_, wx, wz, h, localSea, oceanDepth,
                                        maxLand);
                isLand = h >= localSea;
            } else {
                color = (h < seaLevel) ? oceanColor((seaLevel - h) / oceanDepth)
                                       : landColor((h - seaLevel) / maxLand);
                isLand = h >= seaLevel;
            }
            if (isLand) {
                ++landPixels;
            }


            const std::size_t idx = (j * width + i) * 4;
            pixels[idx + 0] = static_cast<std::uint8_t>(std::lround(color.r * 255.0f));
            pixels[idx + 1] = static_cast<std::uint8_t>(std::lround(color.g * 255.0f));
            pixels[idx + 2] = static_cast<std::uint8_t>(std::lround(color.b * 255.0f));
            pixels[idx + 3] = 255;
        }
    }

    const auto finished = std::chrono::steady_clock::now();
    landFraction_ = static_cast<double>(landPixels) /
                    static_cast<double>(width * height);
    generationMs_ = std::chrono::duration<double, std::milli>(finished - started).count();
    pixels_ = std::move(pixels);
}

}  // namespace world
