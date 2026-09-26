#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include <glm/glm.hpp>

#include "core/camera.h"
#include "core/input.h"
#include "core/logger.h"
#include "core/window.h"
#include "ecs/components.h"
#include "ecs/world.h"
#include "physics/physics_system.h"
#include "physics/physics_world.h"
#include "renderer/model_loader.h"
#include "renderer/vulkan_base.h"
#include "world/climate.h"
#include "world/terrain.h"

#ifndef ANTIWORLD_ASSETS_DIR
#define ANTIWORLD_ASSETS_DIR "assets"
#endif

int main() {
    core::Logger::info("AntiWorld: запуск");

    try {
        const std::filesystem::path modelPath =
            std::filesystem::path(ANTIWORLD_ASSETS_DIR) / "cube.obj";
        const renderer::ModelData model = renderer::ModelLoader::load(modelPath);
        core::Logger::info("AntiWorld: модель загружена из " + modelPath.string());

        // Создаём окно и инициализируем базовый слой Vulkan.
        core::Window window(1280, 720, "AntiWorld");
        renderer::VulkanBase vulkan;
        vulkan.init(window);
        vulkan.uploadMesh(model);

        core::Camera camera;
        // Ландшафт простирается на 255 ед. по каждой оси, поэтому дальняя
        // плоскость отсечения по умолчанию (100 ед.) срезала бы его край.
        camera.setClipPlanes(0.5f, 600.0f);
        camera.setSpeed(40.0f);

        ecs::World world(camera, vulkan);
        physics::PhysicsWorld physicsWorld;
        physics::PhysicsSystem physicsSystem(physicsWorld);

        // Климат мира: время года, сезон, температура и влажность. Объявлен
        // раньше terrain, потому что terrain берёт из него температуру для
        // раскраски биомов и живёт только до конца main().
        // Год по умолчанию — 10 минут (600 с), то есть сезон длится 2.5 минуты.
        world::Climate climate;

        // Ландшафт 256x256 узлов: карта высот по фрактальному шуму Перлина,
        // индексированный mesh в renderer, раскрашенный по биомам, и статический
        // коллайдер в физике. Объявлен после renderer, физики, ECS и климата,
        // чтобы деструктор terrain отработал раньше их уничтожения.
        world::Terrain terrain(vulkan, physicsWorld, world.registry(), climate);

        // Стартовая позиция камеры — над сушей, а не произвольно: с океаном
        // ~62% площади точка (0,150) часто оказывалась посреди воды, и игрок
        // видел «странное» — пустоту вместо мира. Ищем по концентрическим
        // кольцам первую точку суши достаточно высоко над уровнем моря и
        // ставим камеру над ней; viewProjection считается каждый кадр из
        // позиции камеры, так что порядок объявления camera/terrain здесь
        // не важен — ищем уже после создания ландшафта.
        {
            glm::vec3 spawn{0.0f, 70.0f, 150.0f};
            const float sea = terrain.heightmap().seaLevel();
            for (float radius = 0.0f; radius < 110.0f; radius += 7.0f) {
                bool found = false;
                for (int a = 0; a < 16 && !found; ++a) {
                    const float angle = 6.2831853f * static_cast<float>(a) / 16.0f;
                    const float wx = radius * std::cos(angle);
                    const float wz = radius * std::sin(angle);
                    const float h = terrain.heightAt(wx, wz);
                    if (h > sea + 4.0f) {
                        spawn = {wx, h + 40.0f, wz + 60.0f};
                        found = true;
                    }
                }
                if (found) break;
            }
            camera.init(spawn, glm::vec3(spawn.x, sea, spawn.z - 40.0f));
        }


        constexpr std::size_t kAgentCount = 100;
        constexpr std::size_t kGridWidth = 10;
        constexpr float kGridSpacing = 8.0f;
        constexpr float kTwoPi = 6.28318530718f;
        // Середина сетки агентов: агенты стоят над началом координат, где
        // ландшафт центрирован.
        constexpr float kGridOffset = -0.5f * static_cast<float>(kGridWidth - 1) * kGridSpacing;

        for (std::size_t index = 0; index < kAgentCount; ++index) {
            const float column = static_cast<float>(index % kGridWidth);
            const float row = static_cast<float>(index / kGridWidth);
            const float angle = kTwoPi * static_cast<float>(index) /
                                static_cast<float>(kAgentCount);
            // Высота берётся из карты высот, иначе часть агентов появилась бы
            // внутри холмов (а остальные — высоко над рельефом).
            const float worldX = kGridOffset + column * kGridSpacing;
            const float worldZ = kGridOffset + row * kGridSpacing;
            const ecs::Transform transform{
                {worldX, terrain.heightAt(worldX, worldZ) + 1.5f, worldZ},
                {},
                {0.45f, 0.45f, 0.45f}};
            const ecs::Velocity velocity{
                {std::cos(angle) * 0.4f, 0.0f, std::sin(angle) * 0.4f}};

            const entt::entity entity = world.createEntity();
            world.registry().emplace<ecs::Transform>(entity, transform);
            world.registry().emplace<ecs::MeshRenderer>(entity, ecs::MeshRenderer{vulkan.mesh()});
            world.registry().emplace<ecs::Velocity>(entity, velocity);
            world.registry().emplace<ecs::RigidBody>(
                entity,
                ecs::RigidBody{physicsWorld.createDynamicBox(
                    transform.scale,
                    transform.position
                )}
            );
            world.registry().emplace<ecs::Agent>(entity);
        }

        double lastTime = glfwGetTime();

        // Главный цикл рендера: обрабатываем события, рисуем кадр, повторяем.
        while (!window.shouldClose()) {
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

            // Климат: сезон, температура, положение солнца. Цвет вершин при
            // этом не пересчитывается — снег и освещение считаются в шейдере
            // из environment, поэтому год прокручивается без пересборки mesh.
            terrain.update(dt);

            world.update(dt);
            physicsSystem.update(world.registry(), dt);
            // Свет и морозность сезона — в UBO этого кадра.
            world.setEnvironment(terrain.environment());
            world.render();
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