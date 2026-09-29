#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "world/terrain_generator.h"

namespace world {

class Climatology;

// Карта всего мира: одна текстура RGBA8, покрывающая квадрат extent × extent
// метров с центром в (0, 0).
//
// Проекция: u растёт по +X, v растёт по +Z. В 3D это натягивается на сферу
// (см. shaders/map.frag), где те же UV читаются как долгота и широта, поэтому
// карта равнопромежуточная — 2:1.
//
// Карта считается из ТОЙ ЖЕ функции высоты, что и рельеф
// (TerrainGenerator::sampleHeightAt), поэтому материки на карте физически
// совпадают с миром: если на карте зелёное — там суша, и наоборот. Раньше
// для «проб» спавна и генерации чанков была одна функция, а карты просто не
// существовало; общая точка истины одна.
//
// Считается в фоновом потоке: 4096×2048 = 8.4 млн пикселей, каждый — две
// суммы fBm, это ~2-3 с. Пока карта не готова, main продолжает рисовать 3D.
class WorldMap {
public:
    struct Config {
        // 4096×2048: один пиксель ≈ 5.5 км при extent 22 585 км. Материк
        // ~4500 км — около 800 пикселей, то есть форма читается.
        std::uint32_t width{4096};
        std::uint32_t height{2048};
        // Сторона мира в метрах (22 585 км).
        double extent{world::kWorldExtent};
    };

    // Два конструктора вместо одного с аргументом по умолчанию: NSDMI вложенной
    // структуры (width/height/extent) не видны в аргументе по умолчанию до конца
    // объемлющего класса — компилятор требовал «default member initializer for
    // Config::width required before the end of its enclosing class».
    WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed);
    WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed, Config cfg);
    // С климатологией карта красится ПО КЛИМАТУ, а не по высоте: температура,
    // осадки, лёд и материальность вместо песка/зелени/скал. Передаётся
    // указателем (не владеет): карта живёт в фоновом потоке, а Climatology
    // построен раньше и умирает позже.
    WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed,
             const Climatology* climatology);
    WorldMap(TerrainGenerator::Config terrain, std::uint32_t seed, Config cfg,
             const Climatology* climatology);
    ~WorldMap();

    WorldMap(const WorldMap&) = delete;
    WorldMap& operator=(const WorldMap&) = delete;

    // Запускает генерацию в фоне. Повторный вызов, пока поток жив, игнорируется.
    void start();
    // Готова ли карта (пиксели можно читать на главном потоке).
    bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    // Идёт ли генерация прямо сейчас.
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // Пиксели RGBA8, tightly packed, размер width*height*4.
    // До ready() вектор пуст — читать нельзя.
    const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

    const Config& config() const noexcept { return config_; }

    // Мировые координаты -> UV карты [0,1]. u по +X, v по +Z.
    glm::vec2 uvOf(double globalX, double globalZ) const noexcept {
        const double half = 0.5 * config_.extent;
        return glm::vec2(static_cast<float>((globalX + half) / config_.extent),
                         static_cast<float>((globalZ + half) / config_.extent));
    }

    // Доля суши на карте (0..1) и время генерации в мс — для лога.
    double landFraction() const noexcept { return landFraction_; }
    double generationMs() const noexcept { return generationMs_; }

private:
    void generate();

    TerrainGenerator::Config terrain_;
    std::uint32_t seed_;
    Config config_;
    // Не владеет, может быть nullptr — тогда карта красится по высоте, как
    // раньше. Читается только в воркере, поле неизменяемо после build().
    const Climatology* climatology_{nullptr};

    std::vector<std::uint8_t> pixels_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;

    // Заполняется в воркере, читается на главном потоке только после
    // ready() (release/acquire в ready_ и делает эти записи видимыми).
    double landFraction_{0.0};
    double generationMs_{0.0};
};

}  // namespace world
