#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

#include <glm/glm.hpp>

#include "core/camera.h"
#include "core/input.h"
#include "core/logger.h"
#include "core/window.h"
#include "ecs/components.h"
#include "ecs/world.h"
#include "physics/physics_system.h"
#include "physics/player_controller.h"
#include "physics/physics_world.h"
#include "renderer/instanced_renderer.h"
#include "renderer/model_loader.h"
#include "renderer/vulkan_base.h"
#include "world/chunk_manager.h"
#include "world/world_map.h"
#include "world/climate.h"
#include "world/climatology.h"
#include "world/vegetation_scatter.h"

#ifndef ANTIWORLD_ASSETS_DIR
#define ANTIWORLD_ASSETS_DIR "assets"
#endif

namespace {

// Сэмплер высоты рельефа для InstancedRenderer::spawnTrees. Сделан отдельной
// функцией, а не лямбдой: спавн принимает УКАЗАТЕЛЬ на функцию, а у
// captureless-лямбды оператор преобразования в указатель константный, и тип
// не совпадает с HeightSampler.
float chunkHeightAt(double globalX, double globalZ, void* userData) {
    auto* manager = static_cast<world::ChunkManager*>(userData);
    const float height = manager->heightAtGlobal(globalX, globalZ);
    // NaN = «чанк не загружен»: спавн пропустит точку вместо того, чтобы
    // поставить дерево в воздухе или под водой.
    return std::isfinite(height) ? height : std::numeric_limits<float>::quiet_NaN();
}

static_assert(
    std::is_same_v<decltype(&chunkHeightAt), renderer::InstancedRenderer::HeightSampler>,
    "chunkHeightAt должен совпадать с InstancedRenderer::HeightSampler");

// Сэмплер климата и рельефа для world::VegetationScatter.
//
// Контекст собран в структуру, а не в лямбду с захватом: SiteSampler — это
// указатель на функцию с void*, и тип лямбды с захватом в него не
// преобразуется. Кроме того, все потребуемые рассеву источники (чанки, климат)
// лежат рядом по времени жизни, и одна структура делает зависимости
// явными: перестановка строк в main не должна ломать сэмплер.
struct SiteContext {
    world::ChunkManager* chunks{nullptr};
    const world::Climate* climate{nullptr};
};

world::SiteSample sampleSite(double globalX, double globalZ, void* userData) {
    auto* context = static_cast<SiteContext*>(userData);
    world::SiteSample site;
    const float height = context->chunks->heightAtGlobal(globalX, globalZ);
    // NaN = чанк не загружен. Возвращаем NaN и дальше (site.valid()), а не 0:
    // нулевая высота означала бы «суша у моря», и рассев посадил бы лес в
    // пустоте над провалившимся чанком.
    if (!std::isfinite(height)) {
        site.height = std::numeric_limits<float>::quiet_NaN();
        return site;
    }
    site.height = height;

    // Климат спрашиваем в МИРОВЫХ координатах, а не локальных: температура
    // зависит от широты материка, и сдвиг floating origin не должен двигать
    // климатические пояса вместе с игроком.
    const float x = static_cast<float>(globalX);
    const float z = static_cast<float>(globalZ);
    const world::Climate& climate = *context->climate;
    site.temperature = climate.temperatureAt(x, z);
    site.precipitation = climate.precipitationAt(x, z);
    site.humidity = climate.humidityAt(x, z);
    site.continentality = climate.continentalityAt(x, z);
    site.ice = std::max(climate.permanentIceAt(x, z), climate.seaIceAt(x, z));
    // Сухость ПОЧВЫ, а не воздуха: это отдельное свойство, и в модели мира её
    // ближе всего описывает материковость (в глубине континента осадков
    // меньше) на пару с годовой влажностью. Берём максимум, чтобы и сухой
    // воздух, и сухая почва вели к кустарнику, а не к лесу.
    site.dryness = std::clamp(1.0f - site.humidity, 0.0f, 1.0f) * 0.5f +
                   std::clamp(site.continentality, 0.0f, 1.0f) * 0.5f;
    // Уровень моря — ПОЛЕ, поэтому сравниваем с локальным, а не средним.
    site.aboveSea = height - climate.seaLevelAt(x, z);
    return site;
}

// Один знак после запятой. std::to_string для double печатает шесть знаков и
// научную нотацию, а в сводке мира это нечитаемо.
std::string formatOne(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    return stream.str();
}

// Процент одним знаком: «38.9», без знака процента (его ставит вызывающий).
std::string formatPercent(double percent) { return formatOne(percent); }

}  // namespace

int main() {
    core::Logger::info("AntiWorld: запуск");

    try {
        const std::filesystem::path modelPath =
            std::filesystem::path(ANTIWORLD_ASSETS_DIR) / "cube.obj";
        const renderer::ModelData model = renderer::ModelLoader::load(modelPath);
        core::Logger::info("AntiWorld: модель загружена из " + modelPath.string());

        // Создаём окно и инициализируем базовый слой Vulkan.
        // 0/0 — размер основного монитора (полное разрешение экрана).
        core::Window window(0, 0, "AntiWorld");
        renderer::VulkanBase vulkan;
        vulkan.init(window);
        vulkan.uploadMesh(model);

        core::Camera camera;
        // Мир 22 585 км: ближняя плоскость 1 м (иначе при перемещении сквозь
        // рельеф z-fighting), дальняя — с запасом над view distance, иначе
        // дальние чанки срезаются. near=1/far=20000 даёт приемлемую
        // точность глубины для 24-битного буфера.
        camera.setClipPlanes(1.0f, 20000.0f);

        ecs::World world(camera, vulkan);
        physics::PhysicsWorld physicsWorld;
        physics::PhysicsSystem physicsSystem(physicsWorld);
        physics::PlayerController playerController;

        // === Мир: чанковый стриминг вместо ландшафта 256x256 м ===
        //
        // ChunkManager владеет бесконечным миром: генерирует чанки в фоновом
        // пуле, грузит их в GPU/Jolt/ECS в главном потоке порциями
        // (uploadsPerFrame — защита от фризов) и выгружает всё, что вышло
        // за viewDistance. Он же пересчитывает origin (плавающее начало
        // координат) под игрока — камере нужно лишь подхватить новый origin.
        //
        // Порядок объявления важен: ChunkManager должен умереть РАНЬШЕ
        // renderer/physics/ECS, иначе его деструктор не сможет выгрузить
        // меши и коллайдеры.
        world::ChunkManager::Config chunkConfig;
        chunkConfig.viewDistance = 6144.0;  // ~6 чанков радиусом

        // Климатология: статические поля мира (температура, осадки, влажность,
        // материальность, лёд, уровень моря), посчитанные ОДИН раз по рельефу
        // на сетке 512x512 — по той же функции высот и с тем же seed, что и сам
        // мир, иначе карта покажет материки там, где их нет.
        //
        // Порядок объявления важен ВТОРОЙ раз: unique_ptr объявлен раньше
        // climate, поэтому разрушается позже, и Climate ни разу не увидит
        // освобождённую память. Спинлок-цикла здесь нет намеренно: поля
        // статичны, а карта мира (world::WorldMap) читает их же ниже.
        auto climatology =
            world::Climatology::build(chunkConfig.terrain, chunkConfig.seed);
        core::Logger::info("Climate: суша " +
                           formatPercent(climatology->landFraction() * 100.0) + "%, средняя " +
                           formatOne(climatology->meanTemperature()) + " C, ледники " +
                           formatPercent(climatology->iceAreaFraction() * 100.0) + "% суши, " +
                           "уровень моря: средний " +
                           formatOne(climatology->seaLevelMean()) + " м, разброс " +
                           formatOne(climatology->seaLevelRange()) + " м");

        // Климат мира: время года, сезон, температура и влажность. С климатологией
        // пространственная часть берётся из поля, а сам Climate отвечает за
        // календарь, солнце и суточные колебания.
        world::Climate climate;
        climate.setClimatology(climatology.get());
        climate.setBaseSeaLevel(chunkConfig.terrain.geography.seaLevel);

        world::ChunkManager chunks(vulkan, physicsWorld, world.registry(), &climate,
                                   chunkConfig);
        const double kViewDistance = chunkConfig.viewDistance;

        // Стартовая позиция камеры. Глобальные координаты (double) — именно
        // в них ChunkManager считает нужные чанки.
        //
        // Точка (0,0) может оказаться в океане (см. поиск суши ниже), поэтому
        // сначала ставим камеру на безопасную высоту над уровнем моря и ждём
        // первых загруженных чанков, после чего переносим на сушу.
        camera.init(glm::dvec3(0.0, 600.0, 0.0), glm::dvec3(0.0, 600.0, -100.0));
        bool spawnPointResolved = false;
        bool spawnPointPending = false;
        double spawnX = 0.0;
        double spawnZ = 0.0;

        // === Растительность: 20 000 деревьев одним draw call ===
        //
        // InstancedRenderer держит пул объектов, сам отсекает их по AABB
        // относительно frustum и складывает выжившие в DrawData; drawFrame()
        // затем сливает все инстансы одной mesh в ОДИН vkCmdDrawIndexed.
        // Деревья ставятся по сетке с джиттером над рельефом — высота берётся
        // из уже загруженного чанка, под водой деревья не растут.
        renderer::InstancedRenderer instanced;
        // Culling-плоскости кадра (локальные координаты) и буфер DrawData для
        // инстансов. Живут вне цикла, чтобы ни culling, ни добавление draw call'ов
        // не аллоцировали память в горячем пути.
        renderer::Frustum frustum;
        std::vector<renderer::DrawData> instancedDraws;


        // Габариты игрока. Jolt-капсула: halfHeight — половина цилиндра,
        // поэтому полная высота = 2 * (halfHeight + radius) = kPlayerHeight.
        constexpr float kPlayerRadius = 0.35f;
        constexpr float kPlayerHeight = 1.8f;
        // Глаза: чуть ниже макушки, как у человека.
        constexpr float kPlayerEyeHeight = 1.62f;

        constexpr std::size_t kAgentCount = 100;
        constexpr std::size_t kGridWidth = 10;
        constexpr float kGridSpacing = 8.0f;
        constexpr float kTwoPi = 6.28318530718f;
        // Середина сетки агентов относительно точки спавна.
        constexpr float kGridOffset = -0.5f * static_cast<float>(kGridWidth - 1) * kGridSpacing;

        // Агенты создаются ОДИН раз и вокруг найденной суши, а не вокруг (0,0).
        // Раньше они ставились по сетке в начале координат, но спавн игрока
        // уехал на сушу за десятки километров: там, где стояли агенты, не было
        // ни одного чанка, значит и коллайдера, и тела падали вечно — тест
        // физики проверял пустоту.
        const auto spawnAgents = [&](const awdm::dvec3& centerGlobal) {
            for (std::size_t index = 0; index < kAgentCount; ++index) {
                const float column = static_cast<float>(index % kGridWidth);
                const float row = static_cast<float>(index / kGridWidth);
                const float angle = kTwoPi * static_cast<float>(index) /
                                    static_cast<float>(kAgentCount);
                // Ставим высоко и позволяем физике уронить на рельеф: высота
                // земли под точкой ещё не подтверждена загруженным чанком, а
                // коллайдеры появятся раньше, чем тела долетят.
                const double worldX = centerGlobal.x + kGridOffset + column * kGridSpacing;
                const double worldZ = centerGlobal.z + kGridOffset + row * kGridSpacing;
                const ecs::Transform transform{
                    {static_cast<float>(worldX - chunks.origin().x), 400.0f,
                     static_cast<float>(worldZ - chunks.origin().z)},
                    {},
                    {0.45f, 0.45f, 0.45f}};
                const ecs::Velocity velocity{
                    {std::cos(angle) * 0.4f, 0.0f, std::sin(angle) * 0.4f}};

                const entt::entity entity = world.createEntity();
                world.registry().emplace<ecs::Transform>(entity, transform);
                world.registry().emplace<ecs::MeshRenderer>(entity,
                                                           ecs::MeshRenderer{vulkan.mesh()});
                world.registry().emplace<ecs::Velocity>(entity, velocity);
                world.registry().emplace<ecs::RigidBody>(
                    entity,
                    ecs::RigidBody{physicsWorld.createDynamicBox(transform.scale,
                                                                 transform.position)});
                world.registry().emplace<ecs::Agent>(entity);
            }
        };
        bool agentsSpawned = false;

        // Сущность игрока. Пуста (entt::null), пока суша не найдена.
        entt::entity playerEntity = entt::null;

        // --- Карта мира (клавиша M) ---
        //
        // Считается в фоне сразу при старте: 4096x2048 = 8.4 млн пикселей,
        // каждый — две суммы fBm, это ~2-3 с. Пока считается, игра живёт как
        // обычно, а M просто ничего не делает. Пиксели загружаются в текстуру
        // ОДИН раз, когда генерация закончилась.
        const auto startTime = std::chrono::steady_clock::now();
        world::WorldMap worldMap(chunkConfig.terrain, chunkConfig.seed, climatology.get());
        bool mapOpen = false;
        // Смещение обзора планеты относительно игрока (радианы). Маркер игрока
        // при этом не уезжает с карты: смещается только точка, вставленная в
        // центр диска, поэтому глобус доводится мышью в любую сторону.
        float mapSpinLon = 0.0f;
        float mapSpinLat = 0.0f;
        bool mapTextureUploaded = false;
        worldMap.start();
        // Предыдущая локальная позиция тела: по разнице с текущей считается
        // вектор движения для проверки направления.
        glm::vec3 prevPlayerLocal{0.0f};

        // Сдвиг floating origin обрабатывает ChunkManager для ЧАНКОВ, но тела,
        // созданные вне чанков (игрок, агенты), о нём не знают: их локальные
        // координаты остались бы от старого origin, и после сдвига они уехали
        // бы на километры вместе со «сдвинутой» землёй. Здесь ловим изменение
        // origin и переносим все такие тела тем же сдвигом.
        awdm::dvec3 lastOrigin = chunks.origin();
        const auto rebaseDynamicBodies = [&]() {
            const awdm::dvec3 origin = chunks.origin();
            if (origin == lastOrigin) {
                return;
            }
            const glm::vec3 delta = glm::vec3(
                static_cast<float>(origin.x - lastOrigin.x),
                static_cast<float>(origin.y - lastOrigin.y),
                static_cast<float>(origin.z - lastOrigin.z));
            // Только игрок и агенты: тела чанков ChunkManager пересчитал сам.
            const auto shift = [&](const entt::entity e) {
                auto& transform = world.registry().get<ecs::Transform>(e);
                const auto& body = world.registry().get<ecs::RigidBody>(e);
                transform.position -= delta;
                if (physicsWorld.isBodyValid(body.handle)) {
                    physicsWorld.setBodyTransform(
                        body.handle, transform.position,
                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                }
            };
            for (const entt::entity e :
                 world.registry().view<ecs::Transform, ecs::RigidBody, ecs::Player>()) {
                shift(e);
            }
            for (const entt::entity e :
                 world.registry().view<ecs::Transform, ecs::RigidBody, ecs::Agent>()) {
                shift(e);
            }
            lastOrigin = origin;
        };

        // Камера следует за телом игрока, а не летает сама. Точка наблюдения —
        // на высоте глаз; ориентацию (yaw/pitch) задаёт мышь в camera.update().
        const auto cameraFollowsPlayer = [&]() {
            if (playerEntity == entt::null) {
                // Тела ещё нет (ждём загрузки чанка спавна) — держим камеру
                // над точкой спавна, иначе стриминг уведёт её обратно в
                // мировое начало, а потом origin откатится следом.
                if (spawnPointPending) {
                    camera.setGlobalPosition(
                        glm::dvec3(spawnX, camera.globalPosition().y, spawnZ));
                }
                return;
            }
            const auto view = world.registry().view<const ecs::Transform, const ecs::Player>();
            for (const entt::entity e : view) {
                const auto& transform = world.registry().get<const ecs::Transform>(e);
                const double gx = static_cast<double>(transform.position.x) + chunks.origin().x;
                const double gz = static_cast<double>(transform.position.z) + chunks.origin().z;
                // Смещение от центра капсулы к глазам.
                const double gy = static_cast<double>(transform.position.y) +
                                  (kPlayerEyeHeight - 0.5 * kPlayerHeight);
                camera.setGlobalPosition(glm::dvec3(gx, gy, gz));
                break;
            }
        };

        // Все чанки, которые накроет сетка агентов, должны быть загружены:
        // иначе часть тел окажется в воздухе без коллайдера. Сетка симметрична
        // относительно точки спавна, поэтому достаточно проверить четыре её
        // угла — они задают границы прямоугольника, внутри которого все узлы.
        const auto agentNeighbourhoodLoaded = [&](const awdm::dvec3& centerGlobal,
                                                   double chunkSize) {
            const double s = chunkSize;
            const double cx = std::floor(centerGlobal.x / s);
            const double cz = std::floor(centerGlobal.z / s);
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const awdm::ChunkCoord coord{static_cast<std::int64_t>(cx) + dx,
                                                 static_cast<std::int64_t>(cz) + dz};
                    if (!chunks.isLoaded(coord)) {
                        return false;
                    }
                }
            }
            return true;
        };

        double lastTime = glfwGetTime();
        // Секунды с начала запуска: единый источник времени для анимации
        // облаков и травы. Отдельные таймеры в разных шейдерах разошлись бы
        // по фазе, и ветер в траве шёл бы вразнобой с движением облаков.
        float worldTimeSeconds = 0.0f;
        float telemetryTimer_ = 0.0f;
        // Сглаженный FPS для счётчика в углу. По одиночному dt цифры дёргались
        // бы на 89..102 и читались бы как помехи, а не как измерение.
        float hudFps_ = 0.0f;

        // --- Климатозависимая растительность вокруг игрока ---
        //
        // Рассев перестраивается, когда игрок ушёл достаточно далеко от
        // последней ПЕРЕБЫТОЙ точки, а не каждый кадр. Ключевой момент —
        // якорь С ПЕРЕБИТОМ (voxel'ный сдвиг): точки рассева привязаны к сетке
        // мировых координат, поэтому участок, попавший в оба рассева, даёт
        // одни и те же растения. Без якоря соседние участки стыковались бы
        // разными деревьями, и лес «пересаживался» бы у игрока на глазах.
        world::VegetationScatter scatter;
        SiteContext siteContext{&chunks, &climate};
        // Шаг якоря: заметно меньше радиуса рассева, чтобы участки
        // перекрывались, и заметно больше размера чанка, чтобы перестроение
        // случалось не каждый чанк.
        constexpr double kScatterCellMeters = 2.0;
        constexpr double kScatterRadiusMeters = 260.0;
        // Столкновения соседних якорей не дают центру «прыгать» на полсетки и
        // перестраивать лес чаще, чем нужно.
        constexpr double kRescatterStepMeters = 48.0;
        awdm::dvec3 scatterAnchor{0.0, 0.0, 0.0};
        // Индексы ячейки якоря, а не метры. Сравнение с kRescatterStepMeters
        // по расстоянию было самосрабатывающим: якорь округляется к сетке
        // 48 м, поэтому сразу после перестроения игрок может оказаться в углу
        // своей ячейки, то есть в 48*sqrt(2) ≈ 68 м от якоря. Это больше
        // kRescatterStepMeters, значит условие «перестроить» было истинно уже
        // на следующем кадре: рассев пересобирался сотни раз вместо одного,
        // и каждый раз кадр стоял ~39 мс. Сравнение индексов ячеек убирает
        // это ровно и без гистерезиса.
        long long scatterCellX = 0;
        long long scatterCellZ = 0;
        bool scatterDone = false;
        double vegetationScatterMs = 0.0;

        // --- Коллизии стволов ---
        //
        // Тела создаются ТОЛЬКО рядом с игроком, а не для всех деревьев в
        // рассеве. Причина конкретная: физический мир создан с лимитом тел
        // (maxBodies), и он общий с рельефом. Полтысячи-десятки тысяч стволов
        // его бы вычерпали, а игрок всё равно не чувствует столкновение дальше
        // нескольких метров. Радиус заведомо больше дальности обзора — как
        // только игрок отойдёт, дерево попадёт в следующую пересборку.
        constexpr double kTreeColliderRadiusMeters = 64.0;
        std::vector<physics::PhysicsWorld::BodyHandle> treeBodies;

        // Ограничение кадра. Без него видеокарта НИКОГДА не простаивает: в
        // режиме MAILBOX синхронизации с монитором нет, и рендер упирается
        // только в скорость, которую тянет GPU, — то есть 100–150 fps и
        // постоянная 100% загрузка. Именно это греет карту, а не сложность
        // сцены: замер показал, что при ОТКЛЮЧЕННОЙ растительности карта
        // грелась даже сильнее (81–83 °C против 80–81), потому что кадров
        // в секунду становилось больше.
        //
        // Ставим 90: выше монитор не показывает, поэтому лишние кадры не
        // видны, но полностью оплачиваются нагревом. Ограничение НЕ на
        // glfwSwapInterval, потому что Mailbox вертикальную синхронизацию
        // отключает, а здесь нужен именно предсказуемый предел нагрузки.
        constexpr double kFrameBudgetSeconds = 1.0 / 90.0;
        std::chrono::steady_clock::time_point nextFrameAt = std::chrono::steady_clock::now();

        // Главный цикл рендера: обрабатываем события, рисуем кадр, повторяем.
        while (!window.shouldClose()) {
            const auto frameStart = std::chrono::steady_clock::now();
            worldTimeSeconds = static_cast<float>(glfwGetTime());
            window.pollEvents();

            // Выход по Escape.
            if (core::Input::isKeyPressed(GLFW_KEY_ESCAPE)) {
                break;
            }

            // M — карта мира. Фронт нажатия из key callback'а, а не опрос
            // isKeyPressed(): удержание M мигало бы картой каждый кадр.
            if (core::Input::consumeKeyPress(GLFW_KEY_M)) {
                if (vulkan.worldMapReady()) {
                    mapOpen = !mapOpen;
                    core::Logger::info(mapOpen ? "Карта мира: открыта"
                                               : "Карта мира: закрыта");
                } else {
                    core::Logger::info("Карта мира: ещё считается, нажми M позже");
                }
            }

            // Текстура карты загружается один раз — 32 МБ пикселей переливать
            // каждый кадр незачем.
            if (!mapTextureUploaded && worldMap.ready()) {
                vulkan.createWorldMap(worldMap.pixels().data(), worldMap.config().width,
                                       worldMap.config().height, worldMap.pixels().size());
                mapTextureUploaded = true;
                core::Logger::info("Карта мира готова: суша=" +
                                   std::to_string(worldMap.landFraction() * 100.0).substr(0, 5) +
                                   "%, расчёт " + std::to_string(worldMap.generationMs() / 1000.0).substr(0, 4) +
                                   " с");
            }

            // Кадр: dt для движения, aspect для перспективной проекции.
            const double now = glfwGetTime();
            const float dt = static_cast<float>(now - lastTime);
            lastTime = now;
            // Сглаживание экспоненциальное: реагирует на просадку за ~10 кадров
            // и не мигает на единичном длинном кадре.
            if (dt > 0.0f) {
                const float instant = 1.0f / dt;
                hudFps_ = (hudFps_ <= 0.0f) ? instant : hudFps_ + (instant - hudFps_) * 0.1f;
            }

            const int width = window.framebufferWidth();
            const int height = window.framebufferHeight();
            const float aspect = (height != 0)
                                     ? static_cast<float>(width) /
                                           static_cast<float>(height)
                                     : 1.0f;

            // На карте ЛКМ крутит планету, а не камеру.
            camera.setLookEnabled(!mapOpen);
            camera.update(dt, aspect);

            // === Стриминг мира ===
            //
            // update() внутри: сдвигает origin к игроку, дренирует готовые
            // чанки из пула, грузит до uploadsPerFrame чанков в GPU/Jolt/ECS,
            // ставит новые задачи генерации и выгружает всё, что вышло за
            // viewDistance. Каждый шаг ограничен бюджетом, поэтому кадр не
            // «залипает» на генерации.
            const glm::dvec3 camGlobal = camera.globalPosition();
            chunks.update(camGlobal, kViewDistance);
            // Камера подхватывает НОВЫЙ origin: её локальная позиция снова
            // рядом с нулём, а глобальная (double) остаётся точной. Если это
            // не сделать, сцена уедет на величину сдвига origin.
            camera.setOrigin(glm::dvec3(chunks.origin().x, chunks.origin().y,
                                         chunks.origin().z));

            // Климат: сезон, температура, положение солнца. Цвет вершин при
            // этом не пересчитывается — снег и освещение считаются в шейдере
            // из environment, поэтому год прокручивается без пересборки mesh.
            climate.update(dt);

            // Спавн на сушу — один раз, как только загрузился первый чанк.
            // Точка (0,0) на мире с ~61% океана чаще всего под водой, и игрок
            // стартовал бы, глядя в пустоту.
            //
            // Ищем по probeHeightAt(), а не по heightAtGlobal(): проба не
            // требует загруженного чанка, поэтому можно искать сушу далеко за
            // пределами viewDistance (6 км) и телепортироваться туда сразу.
            //
            // Фаза 2: тело создаётся отдельным шагом, когда чанк спавна уже
            // ЗАГРУЖЕН. Раньше капсула появлялась в том же кадре, что и
            // teleportOrigin, то есть под ней ещё не было ни mesh, ни
            // коллайдера, и первые кадры она падала в пустоту.
            if (spawnPointPending && playerEntity == entt::null) {
                const double h = chunks.heightAtGlobal(spawnX, spawnZ);
                if (std::isfinite(h)) {
                    spawnPointPending = false;
                    // Ставим капсулу на РЕАЛЬНУЮ высоту загруженного рельефа
                    // (heightAtGlobal), а не на пробу: probeHeightAt считает
                    // непрерывную функцию и на берегу может отличаться от
                    // дискретной сетки чанка. Источник истины — коллайдер,
                    // на котором игрок потом и стоит.
                    const float halfHeight = 0.5f * kPlayerHeight - kPlayerRadius;
                    const glm::vec3 bodyCenter{
                        static_cast<float>(spawnX - chunks.origin().x),
                        static_cast<float>(h + 0.5 * kPlayerHeight + 0.05),
                        static_cast<float>(spawnZ - chunks.origin().z)};
                    playerEntity = world.createEntity();
                    world.registry().emplace<ecs::Transform>(
                        playerEntity, ecs::Transform{bodyCenter, {}, {1.0f, 1.0f, 1.0f}});
                    // Тело НЕ рисуем: камера от первого лица стоит на высоте
                    // глаз (1.62 м) внутри капсулы высотой 1.8 м, и собственный
                    // меш закрывал бы весь обзор изнутри. Физика полноценная.
                    world.registry().emplace<ecs::RigidBody>(
                        playerEntity,
                        ecs::RigidBody{physicsWorld.createDynamicCapsule(
                            halfHeight, kPlayerRadius, bodyCenter)});
                    ecs::Player playerParams{};
                    playerParams.radius = kPlayerRadius;
                    playerParams.height = kPlayerHeight;
                    world.registry().emplace<ecs::Player>(playerEntity, playerParams);
                    core::Logger::info("Игрок: капсула поставлена на сушу, высота=" +
                                       std::to_string(h) + " м");
                }
            }

            if (!spawnPointPending && !spawnPointResolved && chunks.loadedChunkCount() >= 1) {
                spawnPointResolved = true;
                const double step = chunkConfig.chunkSize;
                double ground = 0.0;
                bool found = false;
                // Расширяющиеся кольца: 0 — точка старта, дальше по 1 чанку.
                for (int ring = 0; ring <= 600 && !found; ++ring) {
                    for (int a = 0; a < (ring == 0 ? 1 : 16); ++a) {
                        const double angle = 6.283185307179586 * a / 16.0;
                        const double cx = ring * step * std::cos(angle);
                        const double cz = ring * step * std::sin(angle);
                        const double h = chunks.probeHeightAt(cx, cz);
                        // Порог 8 м: не хотим встать на песчаную кромку, где
                        // половина соседних точек в воде.
                        if (std::isfinite(h) && h > 8.0) {
                            spawnX = cx;
                            spawnZ = cz;
                            ground = h;
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    // Сразу подстраиваем origin под точку спавна, чтобы в этом
                    // же кадре стриминг подгрузил чанки МЕСТА спавна, а не
                    // места старта. Локальные координаты тела считаем уже от
                    // нового origin, иначе капсула уехала бы на десятки км.
                    chunks.teleportOrigin(awdm::dvec3(spawnX, 0.0, spawnZ));
                    camera.setOrigin(glm::dvec3(chunks.origin().x, chunks.origin().y,
                                                 chunks.origin().z));

                    // Ключевой момент: origin телепортирован ЗАРАНЕЕ, чем
                    // появится тело. rebaseDynamicBodies() считает изменение
                    // origin обычным сдвигом и переносит все существующие тела
                    // на -delta. Если не обновить lastOrigin здесь, то в этом
                    // же кадре rebase отменит телепорт: сдвинет только что
                    // созданного игрока назад на 57926 м, и тот окажется в
                    // мировом начале (в океане). Именно это и было причиной
                    // «спавна в море».
                    lastOrigin = chunks.origin();

                    spawnPointPending = true;

                    // Камера — на высоте глаз над пробой, взгляд горизонтально
                    // (иначе W уводит в рельеф). Точную высоту подтвердим в
                    // фазе 2, когда чанк загрузится.
                    const double eyeY = ground + kPlayerEyeHeight;
                    camera.init(glm::dvec3(spawnX, eyeY, spawnZ),
                                 glm::dvec3(spawnX, eyeY, spawnZ - 400.0));
                    // Агенты — рядом с игроком, иначе коллайдеров под ними нет.
                    // Но сетка 10×10 с шагом 8 м (размах 72 м) может вылезти за
                    // границу чанка, и тогда часть тел окажется над соседним,
                    // ещё не загруженным чанком — и будет падать вечно. Поэтому
                    // ждём, пока загружены все чанки сетки, и только потом ставим
                    // тела: с этого момента земля под ними гарантированно есть.
                    if (!agentsSpawned &&
                        agentNeighbourhoodLoaded(awdm::dvec3(spawnX, 0.0, spawnZ),
                                                 chunkConfig.chunkSize)) {
                        agentsSpawned = true;
                        spawnAgents(awdm::dvec3(spawnX, 0.0, spawnZ));
                        core::Logger::info("Агенты созданы на загруженной земле: " +
                                           std::to_string(kAgentCount) + " тел");
                    }
                    core::Logger::info("Спавн на суше: X=" + std::to_string(spawnX) +
                                       " Z=" + std::to_string(spawnZ) +
                                       " высота=" + std::to_string(ground) + " м");
                } else {
                    core::Logger::warn(
                        "Спавн: суша не найдена в радиусе 600 чанков, старт над океаном");
                }
            }

            // Порядок ниже критичен для тел, живущих вне чанков:
            //   1) ChunkManager сдвигает origin для чанков,
            //   2) rebaseDynamicBodies переносит игрока и агентов тем же сдвигом,
            //   3) контроллер задаёт скорость, Jolt делает шаг,
            //   4) камера встаёт на позицию тела.
            rebaseDynamicBodies();

            world.update(dt);
            // Контроллер ЗАДАЁТ скорость тела, потом идёт шаг симуляции, потом
            // позиция тела возвращается в ECS. Камера ведётся от тела — то есть
            // игрок стоит на рельефе под гравитацией, а не висит в воздухе.
            playerController.update(world.registry(), physicsWorld, dt,
                                  camera.front(), camera.right(), !mapOpen);
            physicsSystem.update(world.registry(), dt);
            cameraFollowsPlayer();
            // Свет и морозность сезона — в UBO этого кадра. Раньше это делал
            // world::Terrain::environment(); с переходом на чанковый мир
            // FrameEnvironment собирается прямо из климата, а уровень моря
            // берётся из географии (он общий для всего мира).
            renderer::FrameEnvironment env;
            env.sunDirection = climate.sunDirection();
            env.sunIntensity = climate.sunIntensity();
            env.ambient = climate.ambient();
            // Морозность сезона: зимой снег ложится ниже по порогу альбедо.
            // Значение берётся из климата (он же считает его для рельефа),
            // чтобы вода/снег в шейдере и биомы в вершинах не расходились.
            env.frost = climate.frost();
            env.seaLevel = 0.0f;  // geo.seaLevel по умолчанию
            env.hasWater = 1.0f;  // география включена: океан в мире есть
            // Небо и растительность смотрят в ту же сцену, что и ландшафт, и
            // обязаны знать о ней то же самое: где камера (для параллакса
            // облаков), какое время суток (для сумерек и звёзд), где сдвиг
            // floating origin (без него проекция на слой облаков уехала бы на
            // десятки километров).
            env.cameraPosition = glm::vec3(camera.globalPosition());
            env.dayTime = climate.timeOfDay();
            env.timeSeconds = worldTimeSeconds;
            // Ветер и облачность пока не выведены в настройки мира; берём
            // умеренные значения — заметный, но не мешающий ветер.
            env.windStrength = 0.5f;
            env.cloudCover = 0.42f;
            env.worldOrigin = glm::vec3(chunks.origin());
            world.setEnvironment(env);

            // Растительность живёт не «один раз на запуск», а вокруг игрока:
            // перестраиваем её, когда игрок отошёл от якоря дальше, чем на
            // kRescatterStepMeters. Ждём 9 загруженных чанков, иначе
            // heightAtGlobal вернёт NaN и лес окажется в воздухе/под водой.
            const awdm::dvec3 playerGlobal = camera.globalPosition();
            // Индекс ячейки сетки перестроения, в которой стоит игрок.
            // Смена ячейки — единственный повод перестраивать рассев.
            const long long playerCellX =
                static_cast<long long>(std::floor(playerGlobal.x / kRescatterStepMeters));
            const long long playerCellZ =
                static_cast<long long>(std::floor(playerGlobal.z / kRescatterStepMeters));
            const bool needsRescatter =
                !scatterDone || playerCellX != scatterCellX || playerCellZ != scatterCellZ;
            if (needsRescatter && chunks.loadedChunkCount() >= 9) {
                const auto tScatter0 = std::chrono::steady_clock::now();
                // Якорь С ПЕРЕБИТОМ: округление вниз до сетки кратно
                // kRescatterStepMeters. Тогда и игрок, и якорь лежат на одной
                // сетке, и перестроение не сдвигает участок — растения на
                // стыке двух рассевов совпадают.
                scatterCellX = playerCellX;
                scatterCellZ = playerCellZ;
                scatterAnchor.x = static_cast<double>(scatterCellX) * kRescatterStepMeters;
                scatterAnchor.z = static_cast<double>(scatterCellZ) * kRescatterStepMeters;
                // Полная пересборка пула: проще и надёжнее, чем удалять
                // объекты по индексам. Радиус 260 м и шаг 48 м означают, что
                // участки сильно перекрываются, поэтому на стыке лес не
                // «худеет», а ставится заново тем же самым.
                instanced.clearObjects();
                world::VegetationScatter::ScatterRequest request;
                request.centerGlobal = scatterAnchor;
                request.radiusMeters = kScatterRadiusMeters;
                request.cellSizeMeters = kScatterCellMeters;
                const std::size_t placed = scatter.scatter(request, &sampleSite, &siteContext,
                                                            vulkan, instanced);
                vegetationScatterMs = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - tScatter0)
                                          .count();
                scatterDone = true;
                core::Logger::info("Растительность: " + std::to_string(placed) +
                                   " объектов вокруг якоря, коллизий стволов " +
                                   std::to_string(treeBodies.size()) + ", " +
                                   formatOne(vegetationScatterMs) + " мс");

                // Старые стволы удаляем ДО создания новых: иначе тела
                // накапливались бы с каждой пересборкой, и лимит физического
                // мира исчерпался бы через несколько десятков метров пути.
                for (const physics::PhysicsWorld::BodyHandle handle : treeBodies) {
                    if (physicsWorld.isBodyValid(handle)) physicsWorld.removeBody(handle);
                }
                treeBodies.clear();
                // Коллизия строится в МИРОВЫХ координатах, как и тела рельефа:
                // Jolt-мир не знает про floating origin, сдвиг делает матрица
                // камеры. Поэтому baseGlobal уходит в физику без вычитания
                // chunks.origin() — иначе стволы оказались бы на километры
                // в стороне от своих деревьев.
                for (const world::VegetationScatter::TreeCollider& trunk :
                     scatter.treeColliders()) {
                    const double dx = trunk.baseGlobal.x - playerGlobal.x;
                    const double dz = trunk.baseGlobal.z - playerGlobal.z;
                    if (dx * dx + dz * dz >
                        kTreeColliderRadiusMeters * kTreeColliderRadiusMeters) {
                        continue;
                    }
                    // Центр цилиндра — середина ствола, а не его основание:
                    // тело создаётся относительно центра формы.
                    const float halfHeight = trunk.height * 0.5f;
                    treeBodies.push_back(physicsWorld.createStaticCylinder(
                        trunk.radius, halfHeight,
                        glm::vec3(static_cast<float>(trunk.baseGlobal.x),
                                  static_cast<float>(trunk.baseGlobal.y) + halfHeight,
                                  static_cast<float>(trunk.baseGlobal.z))));
                }
            }

            // --- Инстансная растительность: culling + сбор DrawData ---
            //
            // Frustum строится в ЛОКАЛЬНЫХ координатах (те же, что у view-
            // матрицы камеры). Инстансы хранят глобальную позицию в double и
            // сами переводятся в локальные при отрисовке, поэтому сдвиг
            // floating origin не требует пересоздания пула.
            frustum.updateFromViewProjection(camera.projection() * camera.view(),
                                             camera.globalPosition());
            const auto tCull0 = std::chrono::steady_clock::now();
            // collectDraws() ДОПИСЫВАЕТ в out (так счётчик drawCalls внутри
            // считает только свои группы). Буфер обязан быть очищен вызывающим
            // каждый кадр: иначе он растёт бесконечно — culling замедляется
            // кадр за кадром, а в drawFrame уходят дубли прошлых кадров.
            instancedDraws.clear();
            const renderer::InstancedRenderer::CullStats treeCull =
                instanced.collectDraws(frustum, chunks.origin(), instancedDraws);
            const double cullMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           tCull0)
                    .count();
            if (mapOpen) {
                // === Вращение планеты зажатой ЛКМ ===
                // Камера на карте не крутится (camera.setLookEnabled(false)),
                // но её дельта курсора всё равно снята в этом кадре и лежит в
                // lastMouseDelta(), поэтому берём её здесь и крутим глобус.
                // Тянуть вправо -> поверхность едет вправо -> в центре диска
                // встаёт точка ЗАПАДНЕЕ игрока, поэтому долгота уменьшается.
                // Тянуть вниз (dY>0, Y вниз) -> поверхность едет вниз -> в
                // центре точка СЕВЕРНЕЕ игрока, поэтому широта растёт.
                if (core::Input::isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
                    const glm::vec2 d = camera.lastMouseDelta();
                    // Весь экран по ширине ~= 1600 px -> примерно полный оборот.
                    const float k = kTwoPi / 1600.0f;  // ширина экрана ~= полный оборот
                    mapSpinLon -= d.x * k;
                    mapSpinLat = std::clamp(mapSpinLat + d.y * k,
                                            -1.5533f, 1.5533f);  // ±89°
                    // Долготу держим в [-pi, pi], иначе за сутки игры угол
                    // накапливает тысячи радиан и теряет точность float.
                    mapSpinLon = std::remainder(mapSpinLon, kTwoPi);
                }
                // Карта заменяет 3D-кадр: сцена не рисуется, но мир продолжает
                // жить (стриминг, физика), поэтому маркер едет за игроком.
                renderer::MapUniformObject mapUniform{};
                mapUniform.player = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                mapUniform.params.z = mapSpinLon;
                mapUniform.params.w = mapSpinLat;
                // Время — только для пульсации маркера на сфере.
                mapUniform.params.y = static_cast<float>(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime)
                        .count());
                if (playerEntity != entt::null) {
                    const auto& tr =
                        world.registry().get<const ecs::Transform>(playerEntity);
                    const glm::vec2 uv = worldMap.uvOf(
                        static_cast<double>(tr.position.x) + chunks.origin().x,
                        static_cast<double>(tr.position.z) + chunks.origin().z);
                    mapUniform.player = glm::vec4(uv.x, uv.y, 0.0f, 1.0f);
                }
                vulkan.drawWorldMap(mapUniform);
            } else {
                // Счётчик FPS в левом верхнем углу. На карте мира он не нужен,
                // поэтому текст задаётся только перед сценой.
                vulkan.setHudText("FPS " + std::to_string(static_cast<int>(std::lround(hudFps_))));
                // Тот же frustum, что и для растительности выше: он уже
                // построен в локальном фрейме кадра, а чанки лежат в нём же.
                world.render(frustum, std::span<const renderer::DrawData>(instancedDraws.data(),
                                                                          instancedDraws.size()));
            }
            const double frameCpuMs = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - frameStart)
                                          .count();
            // Проверка физики: агенты должны стоять на рельефе, то есть
            // y = высота меша + полуразмер тела. Считаем отклонение, а не
            // абсолютную Y: так видно и «висит в воздухе», и «провалился».
            // Насколько низ тела игрока относительно нарисованного рельефа.
            // Ноль = стоит на земле, большое отрицательное = провалился сквозь.
            double playerAboveTerrain = 0.0;
            for (const entt::entity e :
                 world.registry().view<const ecs::Transform, const ecs::Player>()) {
                const auto& tr = world.registry().get<const ecs::Transform>(e);
                const double h = chunks.heightAtGlobal(
                    static_cast<double>(tr.position.x) + chunks.origin().x,
                    static_cast<double>(tr.position.z) + chunks.origin().z);
                if (std::isfinite(h)) {
                    playerAboveTerrain = static_cast<double>(tr.position.y) -
                                         0.5 * kPlayerHeight - h;
                }
                break;
            }

            // Насколько направление движения совпадает со взглядом. W должен
            // вести ровно туда, куда смотрит камера, поэтому при движении угол
            // близок к нулю. Раньше базис был собран с перепутанными X и Z, и
            // игрок бежал в сторону, отличную от взгляда, на ЛЮБОМ yaw.
            double playerLookAngleDeg = 0.0;
            {
                const auto view = world.registry().view<const ecs::Transform, const ecs::Player>();
                for (const entt::entity e : view) {
                    const auto& tr = world.registry().get<const ecs::Transform>(e);
                    const glm::vec3 step = tr.position - prevPlayerLocal;
                    prevPlayerLocal = tr.position;
                    const float len = std::sqrt(step.x * step.x + step.z * step.z);
                    if (len > 1.0e-4f) {
                        const glm::vec3 moveDir = glm::vec3(step.x / len, 0.0f, step.z / len);
                        const glm::vec3 look = camera.front();
                        const float flat = std::sqrt(look.x * look.x + look.z * look.z);
                        if (flat > 1.0e-4f) {
                            const glm::vec3 lookDir = glm::vec3(look.x / flat, 0.0f, look.z / flat);
                            playerLookAngleDeg = std::acos(
                                std::clamp(glm::dot(moveDir, lookDir), -1.0f, 1.0f)) *
                                                57.2957795f;
                        }
                    }
                    break;
                }
            }

            double agentRestError = 0.0;
            {
                std::size_t n = 0;
                for (const entt::entity e :
                     world.registry().view<const ecs::Transform, const ecs::Agent>()) {
                    const auto& tr = world.registry().get<const ecs::Transform>(e);
                    const double h = chunks.heightAtGlobal(
                        static_cast<double>(tr.position.x) + chunks.origin().x,
                        static_cast<double>(tr.position.z) + chunks.origin().z);
                    if (!std::isfinite(h)) continue;
                    agentRestError += (static_cast<double>(tr.position.y) - h -
                                        static_cast<double>(tr.scale.y));
                    ++n;
                }
                if (n != 0) agentRestError /= static_cast<double>(n);
            }

            // Диагностика кадра: раз в 2 секунды пишем в лог позицию камеры
            // (ГЛОБАЛЬНУЮ, в double — именно она показывает точность на
            // дистанции в тысячи км), состояние стриминга и origin.
            telemetryTimer_ += dt;
            if (telemetryTimer_ >= 2.0f) {
                telemetryTimer_ = 0.0f;
                const glm::dvec3 eyeGlobal = camera.globalPosition();
                const world::StreamingStats st = chunks.stats();
                const bool focused =
                    glfwGetWindowAttrib(window.handle(), GLFW_FOCUSED) == GLFW_TRUE;
                core::Logger::info(
                    "Кадр: eyeGlobal=(" + std::to_string(eyeGlobal.x) + "," +
                        std::to_string(eyeGlobal.y) + "," + std::to_string(eyeGlobal.z) +
                        ") origin=(" + std::to_string(chunks.origin().x) + "," +
                        std::to_string(chunks.origin().z) + ") local=" +
                        std::to_string(camera.position().x) + "," +
                        std::to_string(camera.position().y) + "," +
                        std::to_string(camera.position().z) +
                        " земляПодКамерой=" +
                        std::to_string(chunks.heightAtGlobal(eyeGlobal.x, eyeGlobal.z)) +
                        " aspect=" + std::to_string(aspect) + " drawCalls=" +
                        std::to_string(vulkan.lastFrameDrawCalls()) + " fps=" +
                        std::to_string(static_cast<int>(1.0f / (dt > 0.0f ? dt : 1.0f))) +
                        " фокус=" + (focused ? "ДА" : "НЕТ") +
                        " | деревья: " + std::to_string(treeCull.visible) + "/" +
                        std::to_string(treeCull.total) + " (" +
                        std::to_string(treeCull.drawCalls) + " instanced draw call, scatter " +
                        formatOne(vegetationScatterMs) + " мс) cull=" +
                        std::to_string(cullMs) + "мс cpuFrame=" + std::to_string(frameCpuMs) +
                        "мс | ИГРОК: " + (playerController.state().grounded ? "на земле" : "в воздухе") +
                        " v=" + std::to_string(playerController.state().horizontalSpeed) +
                        "м/с" + (playerController.state().running ? " бег" : " шаг") +
                        " y-рельеф=" + std::to_string(playerAboveTerrain) + "м" +
                        " угол(движ,взгляд)=" +                         std::to_string(playerLookAngleDeg) + "°" +
                        " | чанки в кадре: " +
                        std::to_string(world.chunkCullStats().submitted) + " видимых, " +
                        std::to_string(world.chunkCullStats().culled) + " отсеяно" +
                        " | агенты на рельефе: отклонение=" +
                        std::to_string(agentRestError) + " м" +
                        " yaw=" + std::to_string(camera.yaw() * 57.2958f) + "°" +
                        " pitch=" + std::to_string(camera.pitch() * 57.2958f) + "°" +
                        " | стриминг: " + chunks.debugLine());
            }

            // Держим темп не выше бюджета кадра. Спим только остаток: если
            // кадр сам по себе длиннее бюджета (тяжёлая пересборка рассева,
            // подгрузка чанков), ничего не ждём — иначе ограничитель
            // превратился бы в источник рывков.
            nextFrameAt += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kFrameBudgetSeconds));
            const auto frameDeadline = std::chrono::steady_clock::now();
            if (nextFrameAt < frameDeadline) {
                // Кадр не уложился: сдвигаем точку отсчёта, иначе ограничитель
                // копил бы долг и потом «догонял» его пачкой бесплатных кадров.
                nextFrameAt = frameDeadline;
            } else {
                std::this_thread::sleep_for(nextFrameAt - frameDeadline);
            }
        }

        core::Logger::info(
            "AntiWorld: выход из цикла рендера, прошло " +
            std::to_string(static_cast<int>(climate.elapsedYears())) + " игровых лет, " +
            std::string(world::seasonName(climate.season())));
    } catch (const std::exception& error) {
        core::Logger::error(std::string("AntiWorld: фатальная ошибка: ") + error.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
