#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
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

        // Климат мира: время года, сезон, температура и влажность.
        world::Climate climate;

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
        world::WorldMap worldMap(chunkConfig.terrain, chunkConfig.seed);
        bool mapOpen = false;
        bool mapKeyWasDown = false;
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
        float telemetryTimer_ = 0.0f;
        bool treesSpawned = false;

        // Главный цикл рендера: обрабатываем события, рисуем кадр, повторяем.
        while (!window.shouldClose()) {
            const auto frameStart = std::chrono::steady_clock::now();
            window.pollEvents();

            // Выход по Escape.
            if (core::Input::isKeyPressed(GLFW_KEY_ESCAPE)) {
                break;
            }

            // M — карта мира. Переключение ПО ФРОНТУ нажатия, а не по
            // удержанию: isKeyPressed() истинно всё время, пока клавиша
            // зажата, и карта мигала бы открытой/закрытой каждый кадр.
            const bool mapKeyDown = core::Input::isKeyPressed(GLFW_KEY_M);
            if (mapKeyDown && !mapKeyWasDown) {
                if (vulkan.worldMapReady()) {
                    mapOpen = !mapOpen;
                    core::Logger::info(mapOpen ? "Карта мира: открыта" : "Карта мира: закрыта");
                } else {
                    core::Logger::info("Карта мира: ещё считается, нажми M позже");
                }
            }
            mapKeyWasDown = mapKeyDown;

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
            const int width = window.framebufferWidth();
            const int height = window.framebufferHeight();
            const float aspect = (height != 0)
                                     ? static_cast<float>(width) /
                                           static_cast<float>(height)
                                     : 1.0f;

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
            world.setEnvironment(env);

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
                // Карта заменяет 3D-кадр: сцена не рисуется, но мир продолжает
                // жить (стриминг, физика), поэтому маркер едет за игроком.
                renderer::MapUniformObject mapUniform{};
                mapUniform.player = glm::vec4(0.0f, 0.0f, 0.0035f, 0.0f);
                if (playerEntity != entt::null) {
                    const auto& tr =
                        world.registry().get<const ecs::Transform>(playerEntity);
                    const glm::vec2 uv = worldMap.uvOf(
                        static_cast<double>(tr.position.x) + chunks.origin().x,
                        static_cast<double>(tr.position.z) + chunks.origin().z);
                    mapUniform.player = glm::vec4(uv.x, uv.y, 0.0035f, 1.0f);
                }
                vulkan.drawWorldMap(mapUniform);
            } else {
                world.render(std::span<const renderer::DrawData>(instancedDraws.data(),
                                                                 instancedDraws.size()));
            }
            const double frameCpuMs = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - frameStart)
                                          .count();
            // Деревья расставляем один раз, когда вокруг игрока уже есть
            // загруженные чанки с рельефом: иначе heightAtGlobal вернёт NaN и
            // деревья окажутся в воздухе/под водой.
            if (!treesSpawned && chunks.loadedChunkCount() >= 9) {
                treesSpawned = true;
                const std::size_t placed = instanced.spawnTrees(
                    vulkan, 20000, camera.globalPosition(), 3000.0, &chunkHeightAt, &chunks,
                    2.0f, 20240517u);
                core::Logger::info("Растительность: " + std::to_string(placed) +
                                   " деревьев (instancing, 1 draw call на mesh)");
            }

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
                        std::to_string(treeCull.drawCalls) + " instanced draw call) cull=" +
                        std::to_string(cullMs) + "мс cpuFrame=" + std::to_string(frameCpuMs) +
                        "мс | ИГРОК: " + (playerController.state().grounded ? "на земле" : "в воздухе") +
                        " v=" + std::to_string(playerController.state().horizontalSpeed) +
                        "м/с" + (playerController.state().running ? " бег" : " шаг") +
                        " y-рельеф=" + std::to_string(playerAboveTerrain) + "м" +
                        " угол(движ,взгляд)=" + std::to_string(playerLookAngleDeg) + "°" +
                        " | агенты на рельефе: отклонение=" +
                        std::to_string(agentRestError) + " м" +
                        " yaw=" + std::to_string(camera.yaw() * 57.2958f) + "°" +
                        " pitch=" + std::to_string(camera.pitch() * 57.2958f) + "°" +
                        " | стриминг: " + chunks.debugLine());
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
