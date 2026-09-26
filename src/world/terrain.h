#pragma once

// Готовый ландшафт в сцене: карта высот + mesh в renderer + статическое тело
// Jolt + entity в ECS, раскрашенный по биомам.
//
// Terrain ничего не наследует и не знает про рендер internals: он собирает
// три части (TerrainGenerator, renderer::VulkanBase, physics::PhysicsWorld)
// и регистрирует результат в реестре ECS. Деструктор разбирает всё в обратном
// порядке, поэтому в функции main() объекты должны объявляться так, чтобы
// Terrain жил не дольше renderer, физики и ecs::World.
//
// Про биомы и климат. Terrain — единственное место, где встречаются высота
// рельефа, карта высот и Climate, поэтому вся раскраска живёт здесь:
//
//   1) После генерации карты высот Terrain отдаёт Climate сэмплер высот и
//      размеры мира — от этого температура начинает зависеть от рельефа
//      (вершины холоднее низин) и от положения на карте;
//   2) Цвет каждой вершины берётся из world::sampleBiome по СРЕДНЕГОДОВОЙ
//      температуре и влажности, поэтому он не меняется вместе с сезоном;
//   3) Смена сезона не трогает mesh вообще: обновляется только UBO (frost от
//      Climate), и снег считает уже шейдер. Благодаря этому год прокручивается
//      за секунды без пересборки вершинного буфера.

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "physics/physics_world.h"
#include "renderer/vulkan_base.h"
#include "world/biome.h"
#include "world/climate.h"
#include "world/terrain_generator.h"

namespace world {

class Terrain {
public:
    // Генерирует ландшафт по config и добавляет его в сцену: mesh в renderer,
    // коллайдер в физику и entity (Transform + MeshRenderer + RigidBody) в
    // реестр ECS. Ландшафт центрирован в начале координат, поэтому Transform
    // у entity единичный — вершины mesh уже в мировых координатах.
    //
    // climate — внешний объект, живущий дольше Terrain: он двигает время года
    // (Terrain::update), отдаёт температуру для раскраски и освещение для
    // шейдера. Terrain не владеет им, но настраивает (сэмплер высот, размеры
    // мира) и сбрасывает сэмплер в деструкторе, чтобы в Climate не осталось
    // ссылки на освобождённую карту высот.
    Terrain(renderer::VulkanBase& renderer, physics::PhysicsWorld& physicsWorld,
            entt::registry& registry, Climate& climate,
            TerrainGenerator::Config config = TerrainGenerator::Config{},
            BiomeParams biomeParams = BiomeParams{});

    ~Terrain();

    // Ландшафт владеет GPU- и физическими ресурсами, копирование запрещено.
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    const TerrainGenerator& generator() const noexcept { return generator_; }
    const Heightmap& heightmap() const noexcept { return heightmap_; }
    const renderer::Mesh* mesh() const noexcept { return mesh_; }
    physics::PhysicsWorld::BodyHandle body() const noexcept { return body_; }
    entt::entity entity() const noexcept { return entity_; }
    const BiomeParams& biomeParams() const noexcept { return biomeParams_; }

    // Климат ландшафта (не владеет). Через него же идёт время года.
    Climate& climate() const noexcept { return *climate_; }

    // Продвигает климат на deltaTime секунд: сезон, температура, положение
    // солнца. Вызывается каждый кадр из главного цикла.
    void update(float deltaTime);

    // Свет и климат кадра для шейдера. Собирается из Climate, поэтому кадр
    // всегда отражает текущий сезон и время суток.
    renderer::FrameEnvironment environment() const;

    // Высота поверхности в точке (worldX, worldZ) — билинейно по карте высот.
    // Нужна, чтобы ставить объекты на рельеф, а не на условный ноль.
    float heightAt(float worldX, float worldZ) const {
        return heightmap_.sample(worldX, worldZ);
    }

    // Смесь биомов в точке (worldX, worldZ): веса, цвет и склонность к снегу.
    // Температура берётся среднегодовая, поэтому ответ не зависит от сезона —
    // для правил (какой биом здесь растёт) это то, что нужно, а для раскраски
    // на текущий момент хватает season-aware снега из шейдера.
    BiomeBlend biomeBlendAt(float worldX, float worldZ) const;
    // Доминирующий биом в точке — короткая запись для правил и логов.
    Biome biomeAt(float worldX, float worldZ) const {
        return biomeBlendAt(worldX, worldZ).dominant();
    }

private:
    // Убирает entity из реестра, тело из физики и mesh из renderer.
    // Порядок важен: пока entity в реестре, RenderSystem видит MeshRenderer.
    void destroy();

    // Раскраска одной вершины по биому: вызывается генератором при createMesh.
    void paintVertex(float worldX, float worldZ, float height,
                     renderer::Vertex& vertex) const;

    renderer::VulkanBase& renderer_;
    physics::PhysicsWorld& physicsWorld_;
    entt::registry& registry_;

    // Не владеет: объявлен раньше Terrain в main() и живёт дольше.
    Climate* climate_;
    TerrainGenerator generator_;
    Heightmap heightmap_;
    BiomeParams biomeParams_;
    // Mesh принадлежит renderer, тело — physics::PhysicsWorld: Terrain хранит
    // только указатели и освобождает их в деструкторе.
    const renderer::Mesh* mesh_{nullptr};
    physics::PhysicsWorld::BodyHandle body_;
    entt::entity entity_{entt::null};
};

}  // namespace world
