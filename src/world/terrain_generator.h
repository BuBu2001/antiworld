#pragma once

// Процедурный ландшафт: карта высот по фрактальному шуму Перлина, индексированный
// mesh поверхности и совпадающий с ним коллайдер Jolt.
//
// Разделение ответственности:
//   * PerlinNoise (world/noise.h) — только шум, без знания о ландшафте;
//   * Heightmap — данные сетки высот и запросы к поверхности (билинейная
//     выборка, экстремумы);
//   * TerrainGenerator — превращает шум в Heightmap, а Heightmap в mesh и в
//     физическое тело;
//   * Terrain (world/terrain.h) — связывает всё это вместе и с ECS.
//
// Соглашение о мировых координатах (общее для mesh, коллайдера и Heightmap):
// узел (x, z) сетки стоит в точке
//     position = (x * cellSize - sizeX / 2, height, z * cellSize - sizeZ / 2),
// то есть ландшафт центрирован в начале координат, X растёт вправо, Z —
// вдоль +Z. Индексы растут так же, как координаты, поэтому преобразование
// между ними — одно вычитание, а поверхность mesh и поверхность коллайдера
// совпадают без дополнительных поправок.

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "physics/physics_world.h"
#include "renderer/model_loader.h"
#include "world/noise.h"

namespace world {

// Сетка высот: width x depth узлов с равным шагом cellSize по осям X и Z.
// Высоты хранятся построчно (индекс узла = z * width + x).
class Heightmap {
public:
    Heightmap() = default;

    // Проверяет размеры, шаг и значения высот; при ошибке — исключение.
    Heightmap(std::uint32_t width, std::uint32_t depth, float cellSize,
              std::vector<float> heights);

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t depth() const noexcept { return depth_; }
    float cellSize() const noexcept { return cellSize_; }
    bool empty() const noexcept { return heights_.empty(); }

    // Уровень моря этой карты высот. 0, если карта без океанов (чистый fBm).
    // Это часть ГЕОГРАФИИ мира: всё, что ниже — вода (океан/море), а не
    // «низкий холм». Рендер и биомы обязаны сверяться с этим значением.
    float seaLevel() const noexcept { return seaLevel_; }
    void setSeaLevel(float level) noexcept { seaLevel_ = level; }

    // Истина, если точка под водой (географически, а не визуально).
    bool isUnderwater(std::uint32_t x, std::uint32_t z) const {
        return heightAt(x, z) < seaLevel_;
    }
    bool isUnderwaterWorld(float worldX, float worldZ) const {
        return sample(worldX, worldZ) < seaLevel_;
    }
    // Доля узлов карты, занятых водой (для логов и отладки генерации).
    float waterFraction() const noexcept { return waterFraction_; }
    void setWaterFraction(float fraction) noexcept { waterFraction_ = fraction; }

    // Габариты ландшафта по X и Z (в мировых единицах).
    float sizeX() const noexcept;
    float sizeZ() const noexcept;

    float minHeight() const noexcept { return minHeight_; }
    float maxHeight() const noexcept { return maxHeight_; }

    // Высота узла сетки; индексы вне диапазона — исключение.
    float heightAt(std::uint32_t x, std::uint32_t z) const;

    // Высота поверхности в произвольной точке плоскости XZ: билинейная
    // интерполяция по сетке, поэтому результат гладкий и совпадает с
    // поверхностью mesh. За пределами ландшафта берётся высота ближайшего
    // края (функцияtotal — удобно для игровых запросов под ногами объекта).
    float sample(float worldX, float worldZ) const;

    const std::vector<float>& heights() const noexcept { return heights_; }
    std::vector<float>& heights() noexcept { return heights_; }

    // Индекс узла в массиве высот.
    std::size_t indexOf(std::uint32_t x, std::uint32_t z) const noexcept {
        return static_cast<std::size_t>(z) * width_ + x;
    }

private:
    std::uint32_t width_{0};
    std::uint32_t depth_{0};
    float cellSize_{1.0f};
    float minHeight_{0.0f};
    float maxHeight_{0.0f};
    float seaLevel_{0.0f};
    float waterFraction_{0.0f};
    std::vector<float> heights_;
};

// Конфигурация «географии» мира: континентально-океаническая маска, по которой
// фрактальный шум превращается в земеподобный ландшафт с океанами, морями,
// побережьями, равнинами и горами. Без этой маски чистый fBm-шум даёт просто
// холмы около нуля («странное», которое видел игрок), а не сушу и воду.
//
// Маска считается от того же координатного центра, что и вся сетка высот,
// поэтому при одном seed меньший ландшафт остаётся куском большего.
struct GeographyConfig {
    // Включена ли маска вовсе. false — чистый fBm (прежнее поведение).
    bool enabled{true};
    // Уровень моря по высоте (в мировых единицах). Всё, что ниже — вода.
    float seaLevel{0.0f};
    // Глубина океана: насколько ниже уровня моря опускается oceanDepth = 1.
    float oceanDepth{24.0f};
    // Максимальная высота суши над уровнем моря (горы).
    float maxLandHeight{38.0f};
    // Частота континентальной маски (крупные «материки»). Меньше — крупнее
    // континенты относительно размера карты.
    float continentScale{0.0035f};
    // Смещение маски шума континентов (свой слой шума, независимый от рельефа).
    std::uint32_t continentSeedOffset{7919u};

    // Доля площади карты, которую должна занимать вода (0..1). После генерации
    // маска линейно растягивается так, чтобы ровно эта доля высот оказалась
    // под уровнем моря — иначе на части сидов получался либо сплошной океан,
    // либо сплошня суша (детерминированно, но непригодно для игры).
    float targetOceanFraction{0.62f};
    // Насколько резко выражены берега: exponent > 1 делает мелководье уже,
    // глубокие места — резче (s-curve маски глубины).
    float coastSharpness{1.6f};
};

// Генератор ландшафта: шум -> карта высот -> mesh / коллайдер.
// Генератор не хранит состояние между вызовами (кроме таблицы шума), поэтому
// один и тот же seed всегда даёт один и тот же ландшафт.
class TerrainGenerator {
public:
    // Раскраска вершины при построении mesh: вызывается для каждого узла
    // сетки с его мировыми координатами и высотой, а заполняет в вершине всё,
    // что не выводится из карты высот (цвет биома, склонность к снегу).
    // Вычисления только на CPU и только во время createMesh, поэтому lifetimes
    // замыканий не переживает вызова.
    //
    // Смысл разделения такой: генератор знает геометрию (позиция, нормаль, UV),
    // а цвет — это уже про климат и биомы, о которых генератор ничего не знает.
    // Поэтому цвет приходит снаружи, а world::Terrain подставляет свою лямбду
    // по world::Climate.
    using VertexPainter = std::function<void(float worldX, float worldZ, float height,
                                             renderer::Vertex&)>;

    struct Config {
        // Узлов по X и Z (не ячеек: сетка 256x256 узлов = 255x255 ячеек).
        std::uint32_t width{256};
        std::uint32_t depth{256};
        // Мировых единиц между соседними узлами.
        float cellSize{1.0f};
        // Частота базовой октавы в «шумовых» единицах на мировую единицу:
        // меньше значение — крупнее формы. 0.005 при cellSize = 1 даёт
        // базовую волну длиной ~200 узлов, то есть крупные холмы.
        float scale{0.005f};
        // Число октав fBm: детализация рельефа (больше — мельче камушки).
        int octaves{5};
        // Рост частоты и падение амплитуды каждой следующей октавы.
        float lacunarity{2.0f};
        float gain{0.4f};
        // Полуразмах высот: итоговая высота = baseLevel + amplitude * шум,
        // то есть рельеф гуляет примерно в пределах ±amplitude.
        float amplitude{26.0f};
        // Средний уровень ландшафта (сдвиг всего рельефа по вертикали).
        float baseLevel{0.0f};
        // Seed шума: одинаковый seed — одинаковый ландшафт.
        std::uint32_t seed{PerlinNoise::kDefaultSeed};
        // География мира: континенты, океаны и уровень моря. По умолчанию
        // включена — без неё рельеф выглядит как бесконечные холмы «вокруг
        // нуля», а не как планета с водой и сушей.
        GeographyConfig geography{};
    };

    explicit TerrainGenerator();

    explicit TerrainGenerator(Config config);

    // Уровень моря текущей конфигурации (0, если география выключена).
    float seaLevel() const noexcept {
        return config_.geography.enabled ? config_.geography.seaLevel : 0.0f;
    }

    TerrainGenerator(const TerrainGenerator&) = default;
    TerrainGenerator& operator=(const TerrainGenerator&) = default;

    const Config& config() const noexcept { return config_; }

    // Новый seed для шума (запоминается в Config): следующий generate() даст
    // другой ландшафт, предыдущие карты высот при этом не меняются.
    void reseed(std::uint32_t seed);

    // Карта высот по текущей конфигурации.
    Heightmap generate() const;

    // Карта высот с overrides четырёх параметров: размеров сетки, частоты
    // базовой октавы и числа октав. Остальное (шаг сетки, амплитуда, seed)
    // берётся из Config — удобно подбирать рельеф, не создавая новый генератор.
    Heightmap generate(std::uint32_t width, std::uint32_t depth, float scale,
                       int octaves) const;

    // Mesh ландшафта по карте высот: вершина на узел (позиция, нормаль, uv),
    // два треугольника на ячейку, индексы uint32 (сетка 256x256 не помещается
    // в 16-битные индексы). Нормали считаются по сетке центральными
    // разностями и нормируются — они совпадают с нормалями поверхности
    // коллайдера, поэтому освещение стыкуется с физикой без швов.
    renderer::ModelData createMesh(const Heightmap& heightmap) const;

    // То же, но с раскраской вершин. painter вызывается после вычисления
    // позиции, нормали и UV, поэтому может переопределять и их, но обычно
    // заполняет только Vertex::color и Vertex::snowBias.
    renderer::ModelData createMesh(const Heightmap& heightmap,
                                   const VertexPainter& painter) const;

    // Статическое тело Jolt с коллайдером по той же карте высот.
    //
    // Используется JPH::HeightFieldShape (высотное поле), а не MeshShape:
    // он хранит по одному float на узел (с квантованием до 8 бит) вместо
    // вершин и списка треугольников, не требует материал на каждый треугольник
    // и быстрее отвечает на запросы столкновений за счёт блочного дерева.
    // Поверхность при этом та же самая — треугольники по диагоналям ячеек.
    //
    // Ограничение Jolt: высотное поле всегда квадратное (N x N), поэтому для
    // не квадратной сетки метод бросает исключение.
    physics::PhysicsWorld::BodyHandle createPhysicsBody(
        const Heightmap& heightmap, physics::PhysicsWorld& physicsWorld,
        const glm::vec3& position = glm::vec3{0.0f}) const;

private:
    // Общая реализация generate: сетка заданного размера с указанной частотой.
    Heightmap generateGrid(std::uint32_t width, std::uint32_t depth, float scale,
                           int octaves) const;

    // Земеподобный рельеф: континентальная маска (крупный шум) + детальный
    // fBm-рельеф -> высота относительно уровня моря. Возвращает сырые высоты
    // (до нормировки доли океана).
    void generateContinentalHeights(std::uint32_t width, std::uint32_t depth,
                                    float scale, int octaves,
                                    std::vector<float>& heights) const;

    Config config_;
    PerlinNoise noise_;
    // Отдельный слой шума для континентальной маски: независим от рельефа,
    // чтобы материки и холмы не были коррелированы.
    PerlinNoise continentNoise_;
};

}  // namespace world
