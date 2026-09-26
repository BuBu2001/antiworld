#include "world/terrain.h"

#include <iomanip>
#include <sstream>
#include <string>

#include "core/logger.h"
#include "ecs/components.h"

namespace world {

namespace {

// std::to_string для float печатает «255.000000» — в сводке о ландшафте
// такой шум не нужен, размеры и высоты округляем до одного знака.
std::string format(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    return stream.str();
}

// Сколько узлов ландшафта попадает в каждый биом — только для сводки в лог.
void countBiomes(const Heightmap& heightmap, const BiomeParams& params, const Climate& climate) {
    std::size_t counts[kBiomeCount] = {0, 0, 0, 0};
    const std::vector<float>& heights = heightmap.heights();
    for (std::uint32_t z = 0; z < heightmap.depth(); ++z) {
        for (std::uint32_t x = 0; x < heightmap.width(); ++x) {
            const float worldX =
                static_cast<float>(x) * heightmap.cellSize() - 0.5f * heightmap.sizeX();
            const float worldZ =
                static_cast<float>(z) * heightmap.cellSize() - 0.5f * heightmap.sizeZ();
            const Biome biome =
                getBiome(heights[heightmap.indexOf(x, z)],
                         climate.annualMeanTemperatureAt(worldX, worldZ),
                         climate.annualMeanHumidityAt(worldX, worldZ), params);
            ++counts[static_cast<std::size_t>(biome)];
        }
    }
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        const std::size_t count = counts[index];
        if (count == 0) {
            continue;
        }
        const auto percent = static_cast<int>(100.0 * static_cast<double>(count) /
                                              static_cast<double>(heights.size()));
        core::Logger::info("Terrain: " + std::string(biomeName(static_cast<Biome>(index))) +
                           " — " + std::to_string(count) + " узлов (" +
                           std::to_string(percent) + "%)");
    }
}

}  // namespace

Terrain::Terrain(renderer::VulkanBase& renderer, physics::PhysicsWorld& physicsWorld,
                 entt::registry& registry, Climate& climate,
                 TerrainGenerator::Config config, BiomeParams biomeParams)
    : renderer_(renderer),
      physicsWorld_(physicsWorld),
      registry_(registry),
      climate_(&climate),
      generator_(config),
      biomeParams_(biomeParams) {
    // Конструктор не должен оставлять «сирот» (mesh в renderer, тело в физике,
    // entity в реестре) при исключении на любом из шагов: guard вызывает
    // destroy() при раскрутке стека. Деструктор при неудачной конструкции не
    // выполняется, поэтому без этого ресурс утекал бы.
    struct ScopeGuard {
        Terrain& self;
        ~ScopeGuard() {
            if (!committed) {
                self.destroy();
            }
        }
        bool committed = false;
    } guard{*this};

    // 1) Карта высот — единственный источник истины для mesh, коллайдера и
    //    температуры (падение с высотой).
    heightmap_ = generator_.generate();

    // 2) Высотные пороги биомов подгоняем под фактический рельеф: размах шума
    //    не равен amplitude генератора, и абсолютный порог в 16 единиц на этой
    //    карте просто недостижим — гор не было бы вовсе.
    fitBiomeHeights(biomeParams_, heightmap_.minHeight(), heightmap_.maxHeight());

    // 3) Климат учитывает рельеф: без этого шага температура была бы одинаковой
    //    на вершине и в долине, и горы не отличились бы от низин по биому.
    //    Сэмплер держит указатель на карту высот, поэтому destroy() обязан его
    //    снять — иначе после уничтожения Terrain климат держал бы ссылку на
    //    освобождённую память.
    climate_->setWorldSize(heightmap_.sizeX(), heightmap_.sizeZ());
    climate_->setHeightSampler(
        [this](float worldX, float worldZ) { return heightmap_.sample(worldX, worldZ); });

    // 4) Mesh ландшафта в renderer. Цвета берутся из биомов и пишутся в вершины
    //    (Vertex::color), шейдер берёт их как альбедо, а материал оставлен
    //    белым: сейчас цвет целиком в вершинах, и Material его не умножает.
    const TerrainGenerator::VertexPainter painter = [this](float worldX, float worldZ, float height,
                                                           renderer::Vertex& vertex) {
        paintVertex(worldX, worldZ, height, vertex);
    };
    const renderer::ModelData model = generator_.createMesh(heightmap_, painter);
    mesh_ = renderer_.createMesh(model, renderer::Material{glm::vec3{1.0f, 1.0f, 1.0f}});

    // 5) Статический коллайдер по той же карте высот: тело неподвижно, но
    //    динамические тела (агенты) на нём стоят и скатываются по склонам.
    body_ = generator_.createPhysicsBody(heightmap_, physicsWorld_);

    // 6) Entity для рендера: Transform единичный, потому что вершины mesh
    //    уже в мировых координатах, а RigidBody привязывает entity к телу.
    entity_ = registry_.create();
    registry_.emplace<ecs::Transform>(entity_,
                                      ecs::Transform{glm::vec3{0.0f}, {}, glm::vec3{1.0f}});
    registry_.emplace<ecs::MeshRenderer>(entity_, ecs::MeshRenderer{mesh_});
    registry_.emplace<ecs::RigidBody>(entity_, ecs::RigidBody{body_});

    core::Logger::info(
        "Terrain: ландшафт " + std::to_string(heightmap_.width()) + "x" +
        std::to_string(heightmap_.depth()) + " узлов, " + format(heightmap_.sizeX()) +
        "x" + format(heightmap_.sizeZ()) + " ед., высота [" +
        format(heightmap_.minHeight()) + ".." + format(heightmap_.maxHeight()) + "], " +
        std::to_string(model.indices.size() / 3) + " треугольников");
    core::Logger::info("Terrain: " + std::string(seasonName(climate_->season())) + ", средняя " +
                       format(climate_->temperature()) + " C, год " +
                       format(climate_->yearLength()) + " с");
    core::Logger::info("Terrain: горы от высоты " + format(biomeParams_.mountainStart) + " до " +
                       format(biomeParams_.mountainEnd) + " ед.");
    // Океан печатаем отдельно от биомов: доли ниже считаются по ВСЕМ узлам,
    // включая дно, поэтому без уровня моря и доли воды их нельзя читать.
    core::Logger::info("Terrain: море на уровне " + format(heightmap_.seaLevel()) +
                       " ед., воды " +
                       format(100.0f * heightmap_.waterFraction()) + "% площади");
    countBiomes(heightmap_, biomeParams_, *climate_);
    guard.committed = true;  // конструкция успешна — ресурсы за нами
}

Terrain::~Terrain() {
    destroy();
}

void Terrain::destroy() {
    // Климат живёт дольше Terrain, поэтому первым делом снимаем с него сэмплер
    // высот: после этого обращаться к карте высот через Climate нельзя.
    if (climate_ != nullptr) {
        climate_->setHeightSampler(nullptr);
        climate_ = nullptr;
    }
    // Порядок освобождения обратный созданию: сначала entity из реестра (иначе
    // RenderSystem попытается нарисовать уже освобождённую mesh), затем тело
    // из физики и mesh из renderer.
    if (entity_ != entt::null) {
        registry_.destroy(entity_);
        entity_ = entt::null;
    }
    if (!body_.IsInvalid()) {
        physicsWorld_.removeBody(body_);
        body_ = physics::PhysicsWorld::BodyHandle{};
    }
    if (mesh_ != nullptr) {
        renderer_.destroyMesh(mesh_);
        mesh_ = nullptr;
    }
}

void Terrain::update(float deltaTime) {
    if (climate_ != nullptr) {
        climate_->update(deltaTime);
    }
}

renderer::FrameEnvironment Terrain::environment() const {
    if (climate_ == nullptr) {
        return renderer::FrameEnvironment{};
    }
    renderer::FrameEnvironment env;
    env.sunDirection = climate_->sunDirection();
    env.sunIntensity = climate_->sunIntensity();
    env.ambient = climate_->ambient();
    env.frost = climate_->frost();
    // География мира: уровень моря и наличие океана приходят из карты высот.
    // Шейдер рисует водную гладь ровно на этой высоте, поэтому значение —
    // факт мира, а не настройка рендера.
    env.seaLevel = heightmap_.seaLevel();
    env.hasWater = heightmap_.waterFraction() > 0.0f ? 1.0f : 0.0f;
    return env;
}

BiomeBlend Terrain::biomeBlendAt(float worldX, float worldZ) const {
    return sampleBiome(heightAt(worldX, worldZ),
                       climate_->annualMeanTemperatureAt(worldX, worldZ),
                       climate_->annualMeanHumidityAt(worldX, worldZ), biomeParams_);
}

void Terrain::paintVertex(float worldX, float worldZ, float height,
                          renderer::Vertex& vertex) const {
    // Цвет и снежность считаются от среднегодовых температуры и влажности: иначе
    // смена сезона перекрашивала бы лес в тундру, а vertex buffer пришлось бы
    // перезагружать каждый кадр.
    const BiomeBlend blend = sampleBiome(height,
                                         climate_->annualMeanTemperatureAt(worldX, worldZ),
                                         climate_->annualMeanHumidityAt(worldX, worldZ),
                                         biomeParams_);
    const glm::vec3 color = blend.color();
    vertex.color[0] = color.r;
    vertex.color[1] = color.g;
    vertex.color[2] = color.b;
    vertex.snowBias = blend.snowBias();
}

}  // namespace world
