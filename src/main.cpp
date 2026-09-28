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
#include "physics/physics_world.h"
#include "renderer/instanced_renderer.h"
#include "renderer/model_loader.h"
#include "renderer/vulkan_base.h"
#include "world/chunk_manager.h"
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
        camera.setSpeed(120.0f);

        ecs::World world(camera, vulkan);
        physics::PhysicsWorld physicsWorld;
        physics::PhysicsSystem physicsSystem(physicsWorld);

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
        bool spawnResolved = false;

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
            if (!spawnResolved && chunks.loadedChunkCount() >= 1) {
                spawnResolved = true;
                const double step = chunkConfig.chunkSize;
                double spawnX = 0.0;
                double spawnZ = 0.0;
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
                    // Камера — на 60 м над сушей, взгляд горизонтально (иначе W
                    // уводит вниз, в рельеф).
                    const glm::dvec3 eye{spawnX, ground + 60.0, spawnZ};
                    camera.init(eye, glm::dvec3(spawnX, ground + 60.0, spawnZ - 400.0));
                    // Сразу подстраиваем origin под новую позицию, чтобы в
                    // этом же кадре стриминг подгрузил чанки МЕСТА спавна, а не
                    // места старта.
                    chunks.teleportOrigin(awdm::dvec3(spawnX, 0.0, spawnZ));
                    camera.setOrigin(glm::dvec3(chunks.origin().x, chunks.origin().y,
                                                 chunks.origin().z));
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

            world.update(dt);
            physicsSystem.update(world.registry(), dt);
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
            world.render(std::span<const renderer::DrawData>(instancedDraws.data(),
                                                             instancedDraws.size()));
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
                        "мс | агенты на рельефе: отклонение=" + std::to_string(agentRestError) + " м" +
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
