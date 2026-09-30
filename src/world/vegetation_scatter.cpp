#include "world/vegetation_scatter.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/logger.h"

namespace world {

namespace {

// Хеш ячейки -> [0,1). Тот же splitmix64, что и в геометрии растений:
// устойчив к целочисленным координатам и не зависит от платформы, поэтому
// «тот же лес» получается и на другой машине.
float cellRandom(std::int64_t x, std::int64_t z, std::uint32_t salt) noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(z) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<std::uint64_t>(salt) * 0x165667B19E3779F9ull;
    h += 0xD1B54A32D192ED03ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<float>(h >> 40) / 16777216.0f;
}

float smoothStep(float edge0, float edge1, float x) noexcept {
    if (std::abs(edge1 - edge0) < 1e-6f) return x < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f);
}

}  // namespace

VegetationScatter::VegetationScatter() : config_(Config{}) {}

VegetationScatter::VegetationScatter(const Config& config) : config_(config) {}

float VegetationScatter::speciesScale(Species s, float r) noexcept {
    // Разброс масштаба внутри вида. Траве нужен широкий диапазон (15 см и
    // 60 см — это разные травы, а не одно растение), дереву — узкий: разброс
    // 0.5x рядом с двухметровой елью дал бы тридцатиметровую сосну, и первая
    // выглядела бы кустом.
    switch (s) {
        case Species::Grass:
            return 0.65f + 0.85f * r;
        case Species::Shrub:
            return 0.70f + 0.80f * r;
        case Species::Conifer:
        case Species::Broadleaf:
            return 0.72f + 0.55f * r;
        case Species::None:
        default:
            return 1.0f;
    }
}

glm::vec3 VegetationScatter::tintFor(Species s, const SiteSample& site, float r) noexcept {
    // Оттенок зависит от климата, и это не украшение: хвоя на севере темнее и
    // синее (меньше света, влажнее), листва в засухе желтеет, степная трава
    // выгорает. Плюс индивидуальный разброс, чтобы соседние растения не были
    // одного тона — глаз сразу замечает регулярность.
    const float cool = smoothStep(8.0f, -8.0f, site.temperature);
    const float dry = smoothStep(0.25f, 0.75f, site.dryness);
    const float vary = 0.86f + 0.28f * r;
    glm::vec3 tint{1.0f};
    switch (s) {
        case Species::Conifer:
            tint = glm::vec3{1.0f - 0.22f * cool, 1.0f - 0.10f * cool, 1.0f + 0.18f * cool};
            break;
        case Species::Broadleaf:
            tint = glm::vec3{1.0f + 0.30f * dry, 1.0f + 0.10f * dry, 1.0f - 0.25f * dry};
            break;
        case Species::Shrub:
            tint = glm::vec3{1.0f + 0.12f * dry, 1.0f + 0.02f * dry, 1.0f - 0.10f * dry};
            break;
        case Species::Grass:
            tint = glm::vec3{1.0f + 0.35f * dry, 1.0f + 0.18f * dry, 1.0f - 0.30f * dry};
            break;
        case Species::None:
        default:
            return glm::vec3{1.0f};
    }
    return tint * vary;
}

VegetationScatter::Species VegetationScatter::speciesAt(const SiteSample& site,
                                                        float roll) const {
    // Лёд — не «редкий вид», а отсутствие вида: сколько бы ни было тепло, на
    // леднике растений нет. Проверяем первым и жёстко.
    if (site.ice > 0.35f) return Species::None;
    if (site.aboveSea < config_.minAboveSea) return Species::None;

    // Холод последовательно превращает лес в тундру: сначала исчезает
    // лиственное, потом хвойное, потом куст, потом трава. Пороги подобраны так,
    // чтобы при +5 ℃ ещё был смешанный лес, при 0 ℃ только хвоя, при -10 ℃
    // кустарник, при -16 ℃ голая тундра.
    const float coldTree = smoothStep(-6.0f, 2.0f, site.temperature);
    const float coldShrub = smoothStep(-15.0f, -4.0f, site.temperature);
    const float coldGrass = smoothStep(-22.0f, -8.0f, site.temperature);

    // Сухость различает лес и пустыню: ОДНОЙ жары мало, жарко и влажно — это
    // как раз лес. Влажность 0.18 — уже сухо.
    const float wet = smoothStep(0.16f, 0.52f, site.humidity);
    // Осадки важнее влажности: дождевой лес растёт там, где влажно, даже если
    // прямо сейчас дождя нет.
    const float rain = smoothStep(0.12f, 0.45f, site.precipitation);
    const float moist = std::max(wet, rain * 0.85f);

    // Хвойное — где холодно И мокро, лиственное — где тепло И мокро. Разница
    // температур и есть то, что реально различает тайгу и дубраву.
    const float coniferWeight = coldTree * moist;
    const float broadleafWeight = smoothStep(0.0f, 9.0f, site.temperature) * moist * rain;

    // Бедная и каменистая почва не держит ни воду, ни корни: сухость почвы
    // отсекает деревья, оставляя редкий куст. Именно так выглядит каменистая
    // пустошь — не «лес посреди пустыни», а кустарник.
    const float soil = 1.0f - smoothStep(0.55f, 0.90f, site.dryness);
    // Материковость: в глубине континента влажность ниже и сезонность выше,
    // поэтому растительность беднее. Небольшое влияние, но в сумме с сухостью
    // даёт правильную картину «степь в глубине материка».
    const float interior = 1.0f - 0.35f * smoothStep(0.35f, 0.9f, site.continentality);

    const float conifer = coniferWeight * soil * interior;
    const float broadleaf = broadleafWeight * soil * interior;
    const float tree = (conifer + broadleaf) * config_.treeDensity;
    const float shrub =
        coldShrub * (0.30f + 0.70f * moist) * config_.shrubDensity * (0.6f + 0.4f * soil);
    const float grass = coldGrass * (0.25f + 0.75f * moist) * config_.grassDensity * interior;

    // Вид выбирается по ВЕСАМ, и конфиг здесь читается как множитель, а не как
    // вероятность. Поэтому treeDensity = 0.10 при grassDensity = 0.68 означает
    // «деревья примерно в семь раз реже травы», а не «деревьев почти нет»:
    // если трава вообще не растёт (холодно и сухо), оставшиеся веса
    // нормируются, и тогда деревья получают всё оставшееся место. Именно это
    // нужно: сухая тундра должна быть пустой, а не превращаться в лес.
    const float total = tree + shrub + grass;
    if (total < 1e-6f) return Species::None;
    const float pick = std::clamp(roll, 0.0f, 1.0f) * total;
    if (pick < tree) {
        // Внутри «деревьев» решает не только климат, но и масса: хвои в
        // тайге втрое больше, чем листвы.
        return (pick < conifer) ? Species::Conifer : Species::Broadleaf;
    }
    if (pick < tree + shrub) return Species::Shrub;
    return Species::Grass;
}

const std::vector<std::uint16_t>& VegetationScatter::ensurePrimitives(
    renderer::VulkanBase& renderer, renderer::InstancedRenderer& target) {
    if (primitivesReady_) return primitives_;
    const std::uint32_t variants = std::max(1u, config_.variantsPerSpecies);
    primitives_.assign(kSpeciesCount * variants, renderer::InstancedRenderer::kInvalidPrimitive);
    for (std::uint32_t variant = 0; variant < variants; ++variant) {
        // Seed варианта выведен из geometrySeed, поэтому при пересборке мира
        // (новый VulkanBase) виды выглядят ровно так же, как в прошлый раз:
        // иначе после перезапуска лес вокруг игрока менялся бы до неузнаваемости.
        const std::uint32_t seed = config_.geometrySeed + variant * 7919u;
        for (std::size_t s = 1; s < kSpeciesCount; ++s) {
            const Species species = static_cast<Species>(s);
            const renderer::ModelData geometry = makeGeometry(species, specFor(species, seed));
            // Порядок регистрации фиксирован (вид → вариант), поэтому индекс
            // примитива = (s-1)*variants + variant всегда верен.
            primitives_[(s - 1) * variants + variant] = target.addPrimitive(renderer, geometry);
        }
    }
    primitivesReady_ = true;
    core::Logger::info("VegetationScatter: загружено " + std::to_string(primitives_.size()) +
                       " примитивов (" + std::to_string(variants) + " варианта на вид)");
    return primitives_;
}

renderer::ModelData VegetationScatter::makeGeometry(
    Species species, const renderer::vegetation::SmallSpec& spec) {
    switch (species) {
        case Species::Conifer:
            return renderer::vegetation::makeConifer(spec);
        case Species::Broadleaf:
            return renderer::vegetation::makeBroadleaf(spec);
        case Species::Shrub:
            return renderer::vegetation::makeShrub(spec);
        case Species::Grass:
            return renderer::vegetation::makeGrassTuft(spec);
        case Species::None:
        default:
            return renderer::ModelData{};
    }
}

renderer::vegetation::SmallSpec VegetationScatter::specFor(Species species,
                                                            std::uint32_t seed) {
    switch (species) {
        case Species::Conifer:
            return renderer::vegetation::coniferSpec(seed);
        case Species::Broadleaf:
            return renderer::vegetation::broadleafSpec(seed);
        case Species::Shrub:
            return renderer::vegetation::shrubSpec(seed);
        case Species::Grass:
            return renderer::vegetation::grassSpec(seed);
        case Species::None:
        default:
            return renderer::vegetation::SmallSpec{};
    }
}

float VegetationScatter::coverageAt(const SiteSample& site) const {
    if (site.ice > 0.35f) return 0.0f;
    if (site.aboveSea < config_.minAboveSea) return 0.0f;

    // Тепло в УЗКОМ диапазоне. Слишком холодно — тундра, слишком жарко —
    // пустыня; и то и другое должно быть голым. Одна только «не холодно»
    // дало бы густой лес на экваторе, что неправда: там жарко и сухо.
    const float warm = smoothStep(-12.0f, 4.0f, site.temperature);
    const float notHot = 1.0f - smoothStep(26.0f, 40.0f, site.temperature);
    const float wet = smoothStep(0.16f, 0.52f, site.humidity);
    const float rain = smoothStep(0.12f, 0.45f, site.precipitation);
    const float moist = std::max(wet, rain * 0.85f);
    const float soil = 1.0f - smoothStep(0.55f, 0.90f, site.dryness);
    const float vigor = warm * notHot * moist * soil;
    return std::clamp(vigor * config_.coverageGain, 0.0f, 1.0f);
}

float VegetationScatter::maxDistanceFor(Species species) const noexcept {
    switch (species) {
        case Species::Conifer:
        case Species::Broadleaf:
            return config_.treeMaxDistanceMeters;
        case Species::Shrub:
            return config_.shrubMaxDistanceMeters;
        case Species::Grass:
            return config_.grassMaxDistanceMeters;
        case Species::None:
            break;
    }
    return 0.0f;
}

std::size_t VegetationScatter::scatter(const ScatterRequest& request, SiteSampler sampler,
                                       void* userData, renderer::VulkanBase& renderer,
                                       renderer::InstancedRenderer& target) {
    if (!sampler) return 0;
    const std::vector<std::uint16_t>& prim = ensurePrimitives(renderer, target);
    const std::uint32_t variants = std::max(1u, config_.variantsPerSpecies);

    const double cell = std::max(request.cellSizeMeters, 0.5);
    const double radius = request.radiusMeters;
    const std::int64_t span = static_cast<std::int64_t>(std::ceil(radius / cell));
    const std::int64_t cx = static_cast<std::int64_t>(std::floor(request.centerGlobal.x / cell));
    const std::int64_t cz = static_cast<std::int64_t>(std::floor(request.centerGlobal.z / cell));

    treeColliders_.clear();
    std::size_t placed = 0;
    std::size_t rejected = 0;
    std::size_t droppedByDistance = 0;
    std::array<std::size_t, kSpeciesCount> perSpecies{};

    for (std::int64_t iz = -span; iz <= span; ++iz) {
        for (std::int64_t ix = -span; ix <= span; ++ix) {
            if (placed >= request.maxObjects) {
                iz = span;  // выход из обоих циклов: достигнут потолок
                break;
            }
            const std::int64_t gx = cx + ix;
            const std::int64_t gz = cz + iz;
            // Розыгрыш идёт от детерминированного хеша координат ячейки, и
            // КАЖДЫЙ вопрос получает СВОЮ соль: занятость, вид, вариант, yaw,
            // масштаб и оттенок. Общая соль была бы ошибкой: один и тот же
            // roll выбирал бы и вид, и вариант, и в итоге вариант 0 всегда
            // достался бы виду, выбранному первым, — а форма растений
            // разъезжалась бы при любом изменении весов.
            const double wx = (static_cast<double>(gx) + 0.5) * cell +
                              (cellRandom(gx, gz, 0x1111u) - 0.5) * cell;
            const double wz = (static_cast<double>(gz) + 0.5) * cell +
                              (cellRandom(gx, gz, 0x2222u) - 0.5) * cell;
            const double dx = wx - request.centerGlobal.x;
            const double dz = wz - request.centerGlobal.z;
            // Круглая область, а не квадрат: в углах квадрата расстояние
            // в 1.41 раза больше, и плотность растений «проседала» бы углами
            // заметной дугой.
            if (dx * dx + dz * dz > radius * radius) continue;

            const SiteSample site = sampler(wx, wz, userData);
            if (!site.valid()) {
                ++rejected;  // чанк не загружен: точка неизвестна
                continue;
            }

            // ЗАНЯТОСТЬ решается первым и отдельным розыгрышем: без неё
            // каждая ячейка получала бы растение и земля читалась бы как
            // равномерная решётка, а не как растительность.
            if (cellRandom(gx, gz, 0x7A3Bu) >= coverageAt(site)) {
                ++rejected;  // место не тянет даже одного растения
                continue;
            }

            const Species species = speciesAt(site, cellRandom(gx, gz, 0x9E37u));
            if (species == Species::None) {
                ++rejected;
                continue;
            }
            // Дальность вида. Трава дальше своего предела не ставится вовсе:
            // это главный источник экономии, она и без того составляет
            // около двух третей всех объектов. Сравнение идёт по квадратам,
            // чтобы не брать корень в каждой ячейке; корень считается только
            // внутри полосы вымирания, где он действительно нужен.
            const float maxDist = maxDistanceFor(species);
            if (maxDist < radius) {
                const float maxDist2 = maxDist * maxDist;
                if (dx * dx + dz * dz >= maxDist2) {
                    ++droppedByDistance;
                    continue;
                }
                // Плавное прореживание к пределу: жёсткий обрыв читался бы
                // кольцом, а здесь вид просто становится реже и гаснет.
                const float fadeStart = maxDist * config_.fadeStartFraction;
                const float fadeStart2 = fadeStart * fadeStart;
                if (dx * dx + dz * dz > fadeStart2) {
                    const double dist = std::sqrt(dx * dx + dz * dz);
                    const float keep = 1.0f -
                        static_cast<float>((dist - fadeStart) / (maxDist - fadeStart));
                    // Своя соль: та же, что у проверки занятости, дала бы
                    // корреляцию — прореживание зависело бы от исходного ролла.
                    if (cellRandom(gx, gz, 0x5A5Au) >= keep) {
                        ++droppedByDistance;
                        continue;
                    }
                }
            }

            const std::size_t base = (static_cast<std::size_t>(species) - 1) * variants;
            const std::size_t variant = static_cast<std::size_t>(
                cellRandom(gx, gz, 0x2C1Bu) * static_cast<float>(variants)) % variants;
            const std::uint16_t primitiveIndex = prim[base + variant];
            if (primitiveIndex == renderer::InstancedRenderer::kInvalidPrimitive) continue;

            renderer::InstancedObject object;
            object.globalPosition = glm::dvec3{wx, static_cast<double>(site.height), wz};
            object.yawRadians = cellRandom(gx, gz, 0x4444u) * 6.2831853f;
            object.scale = speciesScale(species, cellRandom(gx, gz, 0x3333u));
            object.colorTint = tintFor(species, site, cellRandom(gx, gz, 0x5555u));
            object.primitive = primitiveIndex;
            object.radius = target.primitiveRadius(primitiveIndex) * object.scale;
            target.addObject(object);
            // Коллайдер ствола — только для деревьев. Куст и трава
            // проходимы: иначе игрок застревал бы в каждом пучке, и ходьба по
            // лесу превращалась бы в фортирование.
            //
            // Радиус и высота домножаются на object.scale — он же масштабирует
            // ВИДИМУЮ геометрию, поэтому коллизия совпадает с тем, что видно.
            if (species == Species::Conifer || species == Species::Broadleaf) {
                const std::uint32_t seed =
                    config_.geometrySeed + static_cast<std::uint32_t>(variant) * 7919u;
                const renderer::vegetation::SmallSpec spec = specFor(species, seed);
                const bool broadleaf = species == Species::Broadleaf;
                const float trunkR = renderer::vegetation::trunkRadius(spec, broadleaf) *
                                     object.scale;
                treeColliders_.push_back({object.globalPosition, std::max(trunkR, kMinTrunkRadius),
                                          renderer::vegetation::trunkTopHeight(spec, broadleaf) *
                                              object.scale});
            }
            ++placed;
            ++perSpecies[static_cast<std::size_t>(species)];
        }
    }

    core::Logger::info(
        "VegetationScatter: " + std::to_string(placed) + " объектов (хвоя " +
        std::to_string(perSpecies[1]) + ", листва " + std::to_string(perSpecies[2]) + ", куст " +
        std::to_string(perSpecies[3]) + ", трава " + std::to_string(perSpecies[4]) +
        "), отброшено " + std::to_string(rejected) + ", отсеяно по дальности " +
        std::to_string(droppedByDistance) + ", примитивов " +
        std::to_string(prim.size()));
    return placed;
}

}  // namespace world
