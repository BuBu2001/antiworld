#include "world/world_map.h"

#include "core/logger.h"

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

}  // namespace

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed)
    : WorldMap(terrain, seed, Config{}) {}

WorldMap::WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed, Config cfg)
    : terrain_(terrain), seed_(seed), config_(cfg) {
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
            // же меридиан, поэтому рельеф по X обязан быть периодичным.
            const float h = TerrainGenerator::sampleHeightAt(terrain_, seed_, wx, wz,
                                                              config_.extent);

            const glm::vec3 color = (h < seaLevel)
                                        ? oceanColor((seaLevel - h) / oceanDepth)
                                        : landColor((h - seaLevel) / maxLand);
            if (h >= seaLevel) {
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
