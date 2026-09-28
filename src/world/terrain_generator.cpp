#include "world/terrain_generator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

namespace world {

namespace {

// Размер блока JPH::HeightFieldShape (сторона блока треугольников, степенью
// двойки в диапазоне [2, 8]). 8 даёт на сетке 256x256 блок 32x32: broad phase
// отбрасывает больше треугольников одним запросом, память на дерево меньше.
constexpr JPH::uint kHeightFieldBlockSize = 8;

bool isPositiveFinite(float value) {
    return std::isfinite(value) && value > 0.0f;
}

}  // namespace

// --- Heightmap ---

Heightmap::Heightmap(std::uint32_t width, std::uint32_t depth, float cellSize,
                     std::vector<float> heights)
    : width_(width), depth_(depth), cellSize_(cellSize), heights_(std::move(heights)) {
    if (width < 2 || depth < 2) {
        throw std::invalid_argument("Heightmap: сетка должна быть не меньше 2x2 узлов");
    }
    if (!isPositiveFinite(cellSize)) {
        throw std::invalid_argument("Heightmap: шаг сетки должен быть конечным и положительным");
    }
    if (heights_.size() != static_cast<std::size_t>(width) * depth) {
        throw std::invalid_argument("Heightmap: число высот не совпадает с размером сетки");
    }

    const auto [minimum, maximum] = std::minmax_element(heights_.begin(), heights_.end());
    if (!std::all_of(heights_.begin(), heights_.end(),
                     [](float height) { return std::isfinite(height); })) {
        throw std::invalid_argument("Heightmap: высоты содержат нечисловые значения");
    }
    minHeight_ = *minimum;
    maxHeight_ = *maximum;
}

float Heightmap::sizeX() const noexcept {
    return static_cast<float>(width_ - 1) * cellSize_;
}

float Heightmap::sizeZ() const noexcept {
    return static_cast<float>(depth_ - 1) * cellSize_;
}

float Heightmap::heightAt(std::uint32_t x, std::uint32_t z) const {
    if (x >= width_ || z >= depth_) {
        throw std::out_of_range("Heightmap: индекс узла вне сетки");
    }
    return heights_[indexOf(x, z)];
}

float TerrainGenerator::sampleHeightAt(const Config& config, std::uint32_t seed,
                                      double wx, double wz, double wrapX) {
    const GeographyConfig& geo = config.geography;
    PerlinNoise relief(seed);
    PerlinNoise continents(seed ^ 0x5DEECE66Du);

    // Протяжённость, по которой замыкается мир. Карта передаёт свой extent
    // (wrapX), 3D-мир берёт extent из конфига. Если они равны — а они равны
    // по умолчанию — то карта и мир считают ОДНО И ТО ЖЕ поле шума побитово.
    // Если разойдутся, карта начнёт показывать рельеф, которого нет.
    const double span = wrapX > 0.0 ? wrapX : config.worldExtent;

    // Период в ЦЕЛЫХ ячейках решётки. llround, а не усечение: с усечением
    // период был бы на ячейку короче и шов сместился бы на целую ячейку
    // рельефа (~200 м на октаву базовой частоты).
    const auto periodCells = [span](double scale) -> std::int64_t {
        if (span <= 0.0) {
            return 0;
        }
        const double cells = span * scale;
        const std::int64_t rounded = static_cast<std::int64_t>(std::llround(cells));
        return rounded > 0 ? rounded : 0;
    };
    const std::int64_t reliefPeriod = periodCells(config.scale);
    const std::int64_t continentPeriod = periodCells(geo.continentScale);

    // Частоты берутся ИЗ КОНФИГА, а не подгоняются под период. Подгонка
    // (scale = periodCells/wrapX) формально снимала бы остаток периода, но
    // карта и 3D-мир считали бы тогда РАЗНОЕ поле шума: игрок, идя к материку
    // с карты, пришёл бы в океан. Поэтому частоты в конфиге обязаны быть целым
    // числом ячеек на wrapX — это проверяет конструктор WorldMap.

    // Детальный рельеф (fBm) в [-1, 1]. Координаты в double: при глобальных
    // 2.26e7 м float32 давал бы шаг ~2 м, то есть рельеф превращался бы в
    // лестницу из плато.
    // Координата X считается как (x/extent) * periodCells, а НЕ как x*scale.
    // Обе формы дают одно поле шума (разница — последний бит double), но
    // только первая даёт РОВНЫЙ шов: на краю wx = ±extent/2 получается ровно
    // ±periodCells/2, и при чётном periodCells это целое число, а значит
    // дробные части решётки с двух сторон совпадают побитово. При x*scale тот
    // же край давал 564.5 с точностью до 1e-5 ячейки, и высота скакала.
    const double coordX =
        reliefPeriod > 0 ? (wx / span) * static_cast<double>(reliefPeriod)
                         : wx * config.scale;
    const float fbm = relief.fbm2D(coordX, wz * config.scale, config.octaves,
                                   config.lacunarity, config.gain, reliefPeriod);
    const double detail = config.baseLevel + static_cast<double>(config.amplitude) * fbm;

    if (!geo.enabled) {
        return static_cast<float>(detail);
    }

    // Маска «материк/океан» — ГЛОБАЛЬНАЯ функция точки, без нормализации
    // внутри чанка. Раньше доля воды задавалась квантилем по ВНУТРИ чанка,
    // из-за чего каждый чанк был одинаково «океаническим» (материков не
    // существовало) и берег рвался на границах.
    const double coastBias = static_cast<double>(geo.coastBias);
    // Маска нормализуется так же, как рельеф: (x/extent) * continentPeriod.
    const double coordXContinent =
        continentPeriod > 0 ? (wx / span) * static_cast<double>(continentPeriod)
                            : wx * geo.continentScale;
    const double mask = continents.fbm2D(coordXContinent, wz * geo.continentScale,
                                         3, 2.0f, 0.5f, continentPeriod);
    const double sharp = 1.0 / static_cast<double>(std::max(0.05f, geo.coastSharpness));

    if (mask >= coastBias) {
        // Суша: от берега к вершине материка. Деталь рельефа гасится у берега,
        // иначе шум выталкивает пляж выше уровня моря и «озёра» появляются
        // внутри суши.
        const double t = std::pow(
            std::clamp((mask - coastBias) / std::max(1e-6, 1.0 - coastBias), 0.0, 1.0), sharp);
        const double shoreFade = std::min(1.0, t * 3.0);
        return static_cast<float>(geo.seaLevel + t * geo.maxLandHeight +
                                  detail * shoreFade);
    }
    // Океан: чем глубже, тем ниже.
    const double t = std::pow(
        std::clamp((coastBias - mask) / std::max(1e-6, 1.0 + coastBias), 0.0, 1.0), sharp);
    return static_cast<float>(geo.seaLevel - t * geo.oceanDepth);
}

float Heightmap::sample(float worldX, float worldZ) const {
    if (empty()) {
        return 0.0f;
    }

    // Мировые координаты -> координаты сетки (узел 0 в начале сетки).
    const float gridX = worldX / cellSize_ + 0.5f * static_cast<float>(width_ - 1);
    const float gridZ = worldZ / cellSize_ + 0.5f * static_cast<float>(depth_ - 1);

    // За пределами ландшафта берём ближайший край: запрос под ногами объекта
    // не должен бросать исключение, даже если объект уехал за границу.
    const float clampedX = std::clamp(gridX, 0.0f, static_cast<float>(width_ - 1));
    const float clampedZ = std::clamp(gridZ, 0.0f, static_cast<float>(depth_ - 1));

    // Целая часть — индексы четырёх узлов вокруг точки, дробная — веса.
    const auto x0 = static_cast<std::uint32_t>(std::floor(clampedX));
    const auto z0 = static_cast<std::uint32_t>(std::floor(clampedZ));
    const std::uint32_t x1 = std::min(x0 + 1, width_ - 1);
    const std::uint32_t z1 = std::min(z0 + 1, depth_ - 1);
    const float fractionX = clampedX - static_cast<float>(x0);
    const float fractionZ = clampedZ - static_cast<float>(z0);

    // Билинейная интерполяция: поверхность получается непрерывной, в отличие от
    // выборки ближайшего узла со ступеньками.
    const float h00 = heights_[indexOf(x0, z0)];
    const float h10 = heights_[indexOf(x1, z0)];
    const float h01 = heights_[indexOf(x0, z1)];
    const float h11 = heights_[indexOf(x1, z1)];
    const float alongX0 = h00 + (h10 - h00) * fractionX;
    const float alongX1 = h01 + (h11 - h01) * fractionX;
    return alongX0 + (alongX1 - alongX0) * fractionZ;
}

// --- TerrainGenerator ---

TerrainGenerator::TerrainGenerator()
    : TerrainGenerator(Config{}) {}

TerrainGenerator::TerrainGenerator(Config config)
    : config_(config), noise_(config.seed),
      continentNoise_(config.seed + config.geography.continentSeedOffset) {}

Heightmap TerrainGenerator::generate() const {
    return generateGrid(config_.width, config_.depth, config_.scale, config_.octaves);
}

void TerrainGenerator::reseed(std::uint32_t seed) {
    config_.seed = seed;
    noise_.reseed(seed);
    continentNoise_.reseed(seed + config_.geography.continentSeedOffset);
}

Heightmap TerrainGenerator::generate(std::uint32_t width, std::uint32_t depth, float scale,
                                     int octaves) const {
    return generateGrid(width, depth, scale, octaves);
}

Heightmap TerrainGenerator::generateGrid(std::uint32_t width, std::uint32_t depth,
                                        float scale, int octaves) const {
    if (width < 2 || depth < 2) {
        throw std::invalid_argument("TerrainGenerator: сетка должна быть не меньше 2x2 узлов");
    }
    if (!isPositiveFinite(scale)) {
        throw std::invalid_argument("TerrainGenerator: масштаб шума должен быть положительным");
    }
    if (octaves < 1) {
        throw std::invalid_argument("TerrainGenerator: нужно хотя бы одно число октав");
    }
    // lacunarity/gain из Config могли быть заданы некорректно: fbm2D молча
    // возвращает 0 при неконечных параметрах, а отрицательные значения дают
    // инвертированный/расходящийся шум. Валидируем здесь, чтобы ошибка была
    // явной и указывала на источник — конфигурацию генератора.
    if (!isPositiveFinite(config_.lacunarity)) {
        throw std::invalid_argument("TerrainGenerator: lacunarity должен быть положительным и конечным");
    }
    if (!std::isfinite(config_.gain) || config_.gain < 0.0f) {
        throw std::invalid_argument("TerrainGenerator: gain должен быть конечным и неотрицательным");
    }
    if (!isPositiveFinite(config_.cellSize)) {
        throw std::invalid_argument("TerrainGenerator: шаг сетки должен быть положительным");
    }
    if (!std::isfinite(config_.amplitude) || !std::isfinite(config_.baseLevel)) {
        throw std::invalid_argument("TerrainGenerator: амплитуда и уровень должны быть конечными");
    }

    // Смещение сетки относительно начала координат: ландшафт центрируется в
    // нуле, поэтому координаты шума не зависят от размера сетки — при том же
    // seed меньший ландшафт остаётся куском большего.
    const float offsetX = -0.5f * static_cast<float>(width - 1) * config_.cellSize;
    const float offsetZ = -0.5f * static_cast<float>(depth - 1) * config_.cellSize;

    std::vector<float> heights(static_cast<std::size_t>(width) * depth);

    if (config_.geography.enabled && config_.geography.oceanDepth > 0.0f) {
        // Земеподобный рельеф: континенты/океаны + суша над уровнем моря.
        generateContinentalHeights(width, depth, scale, octaves, heights);
    } else {
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const float worldX = static_cast<float>(x) * config_.cellSize + offsetX;
                const float worldZ = static_cast<float>(z) * config_.cellSize + offsetZ;
                const float noise = noise_.fbm2D(worldX * scale, worldZ * scale, octaves,
                                                 config_.lacunarity, config_.gain);
                heights[static_cast<std::size_t>(z) * width + x] = config_.baseLevel +
                                                                 config_.amplitude * noise;
            }
        }
    }

    // География не должна ломать физику: Jolt HeightFieldShape квантует высоты
    // в 8 бит по диапазону [minHeight, maxHeight], поэтому extreme глубины
    // допустимы, но бесконечности — нет (валидация в конструкторе Heightmap).
    Heightmap result(width, depth, config_.cellSize, std::move(heights));
    if (config_.geography.enabled) {
        // Уровень моря и фактическая доля воды — часть географии мира. Их
        // хранит сама карта высот, чтобы рендер (водная гладь), биомы («под
        // водой — не лес») и физика (агенты не спавнятся в океане) сверялись
        // с одним и тем же значением, а не с догадками.
        result.setSeaLevel(config_.geography.seaLevel);
        std::size_t underwater = 0;
        for (float height : result.heights()) {
            if (height < result.seaLevel()) {
                ++underwater;
            }
        }
        result.setWaterFraction(static_cast<float>(underwater) /
                                static_cast<float>(result.heights().size()));
    }
    return result;
}

void TerrainGenerator::generateContinentalHeights(std::uint32_t width, std::uint32_t depth,
                                                  float scale, int octaves,
                                                  std::vector<float>& heights) const {
    const GeographyConfig& geo = config_.geography;
    const std::size_t count = static_cast<std::size_t>(width) * depth;

    // Смещение сетки к центру — та же формула, что и в обычном рельефе, чтобы
    // координаты шума не зависели от размера сетки.
    const float offsetX = -0.5f * static_cast<float>(width - 1) * config_.cellSize;
    const float offsetZ = -0.5f * static_cast<float>(depth - 1) * config_.cellSize;

    // Шаг 1: сырая континентальная маска [-1, 1] (крупный шум — «материки»)
    // и детальный fBm-рельеф той же сетки.
    std::vector<float> mask(count);
    std::vector<float> detail(count);
    for (std::uint32_t z = 0; z < depth; ++z) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float worldX = static_cast<float>(x) * config_.cellSize + offsetX;
            const float worldZ = static_cast<float>(z) * config_.cellSize + offsetZ;
            const std::size_t index = static_cast<std::size_t>(z) * width + x;
            mask[index] = continentNoise_.fbm2D(worldX * geo.continentScale,
                                                worldZ * geo.continentScale, 4, 2.0f, 0.5f);
            detail[index] = noise_.fbm2D(worldX * scale, worldZ * scale, octaves,
                                         config_.lacunarity, config_.gain);
        }
    }

    // Шаг 2: подбор уровня моря так, чтобы доля воды была равна
    // targetOceanFraction. Сортировка копии маски даёт перцентиль: вода там,
    // где mask <= seaThreshold. Детерминированно при том же seed.
    const float fraction = std::clamp(geo.targetOceanFraction, 0.05f, 0.95f);
    std::vector<float> sortedMask = mask;
    std::sort(sortedMask.begin(), sortedMask.end());
    const auto percentileIndex =
        static_cast<std::size_t>(fraction * static_cast<float>(sortedMask.size()));
    const float seaThreshold = sortedMask[std::min(percentileIndex, sortedMask.size() - 1)];

    // Шаг 3: высота каждой точки. Нормируем маску к [0..1] относительно
    // порога: ниже порога — глубина (ocean), выше — суша (land). S-curve
    // coastSharpness делает берег различимым: узкая полоса мелководья и
    // сразу глубокий океан / пологая суша.
    const float oceanRange = std::max(1e-3f, seaThreshold - sortedMask.front());
    const float landRange = std::max(1e-3f, sortedMask.back() - seaThreshold);
    const float sharpness = std::max(0.25f, geo.coastSharpness);

    // Шаг 4: итоговые высоты. Уровень моря — это geo.seaLevel; вода там, где
    // mask <= seaThreshold. Детальная составляющая добавляется к обеим зонам,
    // но на берегу (depthT/landT ~ 0) её вклад мал, поэтому граница вода/суша
    // в карте высот остаётся согласованной с маской.
    for (std::size_t i = 0; i < count; ++i) {
        const float m = mask[i];
        float height;
        if (m <= seaThreshold) {
            const float depthT = std::pow((seaThreshold - m) / oceanRange, sharpness);
            height = geo.seaLevel - geo.oceanDepth * depthT;
            // Мелкие неровности дна океана (в 3 раза слабее, чем на суше).
            height += 0.3f * config_.amplitude * detail[i] * (1.0f - depthT);
        } else {
            const float landT = std::pow((m - seaThreshold) / landRange, 1.0f / sharpness);
            // Суша: плавный подъём от 0 до maxLandHeight с детальным рельефом;
            // горы получаются там, где landT близок к 1 и detail положителен.
            height = geo.seaLevel + landT * geo.maxLandHeight * (0.55f + 0.45f * detail[i]) +
                     0.25f * config_.amplitude * detail[i];
        }
        heights[i] = height;
    }
}

renderer::ModelData TerrainGenerator::createMesh(const Heightmap& heightmap) const {
    // Без painter вершины остаются белыми (значения по умолчанию в
    // renderer::Vertex) и без снега — годятся для отладочной геометрии.
    static const VertexPainter kNoPaint = [](float, float, float, renderer::Vertex&) {};
    return createMesh(heightmap, kNoPaint);
}

renderer::ModelData TerrainGenerator::createMesh(const Heightmap& heightmap,
                                                 const VertexPainter& painter) const {
    if (heightmap.empty()) {
        throw std::invalid_argument("TerrainGenerator: нельзя создать mesh пустой карты высот");
    }

    const std::uint32_t width = heightmap.width();
    const std::uint32_t depth = heightmap.depth();
    const float cellSize = heightmap.cellSize();
    const std::vector<float>& heights = heightmap.heights();

    // Смещение клетки, чтобы ландшафт стоял по центру над началом координат.
    const float offsetX = -0.5f * heightmap.sizeX();
    const float offsetZ = -0.5f * heightmap.sizeZ();

    renderer::ModelData model;
    model.vertices.resize(static_cast<std::size_t>(width) * depth);
    model.indices.reserve(static_cast<std::size_t>(width - 1) * (depth - 1) * 6);

    // Высота узла с индексами за пределами сетки заменяется высотой ближайшего
    // существующего: на краях ландшафта разности становятся односторонними.
    const auto heightClamped = [&heights, width, depth](int x, int z) {
        const auto cx = static_cast<std::uint32_t>(std::clamp(x, 0, static_cast<int>(width) - 1));
        const auto cz = static_cast<std::uint32_t>(std::clamp(z, 0, static_cast<int>(depth) - 1));
        return heights[static_cast<std::size_t>(cz) * width + cx];
    };

    for (std::uint32_t z = 0; z < depth; ++z) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(z) * width + x;
            renderer::Vertex& vertex = model.vertices[index];

            vertex.position[0] = static_cast<float>(x) * cellSize + offsetX;
            vertex.position[1] = heights[index];
            vertex.position[2] = static_cast<float>(z) * cellSize + offsetZ;

            // Нормаль из центральных разностей: у поверхности y = h(x, z)
            // вектор (-dh/dx, 1, -dh/dz) направлен вверх. Усреднение по
            // четырём соседям сглаживает шум градиента, поэтому освещение
            // стыкуется с шероховатостью рельефа без «ступенек».
            const float slopeX =
                (heightClamped(static_cast<int>(x) + 1, static_cast<int>(z)) -
                 heightClamped(static_cast<int>(x) - 1, static_cast<int>(z))) /
                (2.0f * cellSize);
            const float slopeZ =
                (heightClamped(static_cast<int>(x), static_cast<int>(z) + 1) -
                 heightClamped(static_cast<int>(x), static_cast<int>(z) - 1)) /
                (2.0f * cellSize);
            const glm::vec3 normal = glm::normalize(glm::vec3(-slopeX, 1.0f, -slopeZ));
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;

            // UV нормированы по всей поверхности (0..1 по X и Z).
            vertex.uv[0] = static_cast<float>(x) / static_cast<float>(width - 1);
            vertex.uv[1] = static_cast<float>(z) / static_cast<float>(depth - 1);

            // Флаг воды — часть географии: узел под уровнем моря это океан,
            // а не «низкий холм». Заполняется ДО painter, чтобы биомы могли
            // по нему судить о береговой линии; шейдер рисует по нему гладь.
            vertex.water = heights[index] < heightmap.seaLevel() ? 1.0f : 0.0f;

            // Раскраска снаружи (биомы/климат): вызывается последним, чтобы
            // painter видел готовую геометрию и мог на неё опираться.
            if (painter) {
                painter(vertex.position[0], vertex.position[2], vertex.position[1], vertex);
            }
        }
    }

    // Индексированная сетка: на каждую ячейку два треугольника. Индексы
    // uint32, потому что сетка 256x256 уже целиком занимает диапазон uint16
    // (максимальный индекс 65535), а размер задаётся конфигом.
    for (std::uint32_t z = 0; z + 1 < depth; ++z) {
        for (std::uint32_t x = 0; x + 1 < width; ++x) {
            const auto i00 = static_cast<std::uint32_t>(static_cast<std::size_t>(z) * width + x);
            const std::uint32_t i10 = i00 + 1;
            const std::uint32_t i01 = i00 + width;
            const std::uint32_t i11 = i01 + 1;
            model.indices.insert(model.indices.end(), {i00, i01, i10, i10, i01, i11});
        }
    }

    return model;
}

physics::PhysicsWorld::BodyHandle TerrainGenerator::createPhysicsBody(
    const Heightmap& heightmap, physics::PhysicsWorld& physicsWorld,
    const glm::vec3& position) const {
    if (heightmap.empty()) {
        throw std::invalid_argument(
            "TerrainGenerator: нельзя создать коллайдер пустой карты высот");
    }

    const std::uint32_t samples = heightmap.width();
    if (heightmap.depth() != samples) {
        // Высотное поле Jolt всегда квадратное (N x N).
        throw std::invalid_argument(
            "TerrainGenerator: коллайдер-высотное поле требует квадратной сетки");
    }

    // Образец (x, y) высотного поля стоит в точке
    // mOffset + mScale * (x, height, y): индекс y растёт вдоль +Z, то есть
    // так же, как индекс z в createMesh, — поэтому поверхности совпадают.
    // Высоты копируются в настройки Jolt, поэтому указатель на данные
    // карты высот переживать вызов не обязан.
    JPH::HeightFieldShapeSettings settings(
        heightmap.heights().data(),
        JPH::Vec3(-0.5f * heightmap.sizeX(), 0.0f, -0.5f * heightmap.sizeZ()),
        JPH::Vec3(heightmap.cellSize(), 1.0f, heightmap.cellSize()),
        static_cast<JPH::uint>(samples));
    settings.mBlockSize = kHeightFieldBlockSize;

    const JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("TerrainGenerator: Jolt не смог создать высотное поле: " +
                                 std::string(shapeResult.GetError().c_str()));
    }

    // Тело Jolt само хранит ссылку на форму, поэтому результат Create()
    // достаточно живого до конца вызова.
    return physicsWorld.createStaticShape(*shapeResult.Get().GetPtr(), position);
}

}  // namespace world
