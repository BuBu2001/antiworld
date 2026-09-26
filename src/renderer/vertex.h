#pragma once

#include <cstddef>

namespace renderer {

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    // Альбедо вершины. Для ландшафта это смешанный цвет биомов (см.
    // world::BiomeBlend), для остальных моделей — белый, то есть «цвет задаёт
    // источник модели». Значения по умолчанию позволяют грузить геометрию без
    // расчёта цвета: value-инициализация Vertex оставляет её белой.
    float color[3]{1.0f, 1.0f, 1.0f};
    // Склонность вершины к снегу, 0..1 (world::BiomeBlend::snowBias).
    // Ноль — снег не лежит никогда (пустыня, равнина), единица — круглый год
    // (тундра, вершины). Сезон подмешивает шейдер, поэтому смена времени года
    // не требует перезагрузки вершинного буфера.
    float snowBias{0.0f};
    // Флаг географии: 1 — узел карты высот лежит под уровнем моря (вода),
    // 0 — суша. Заполняется при генерации mesh; шейдер по нему рисует
    // океанскую гладь и отключает снег/биомы под водой.
    float water{0.0f};
};

static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, normal) == 12);
static_assert(offsetof(Vertex, uv) == 24);
static_assert(offsetof(Vertex, color) == 32);
static_assert(offsetof(Vertex, snowBias) == 44);
static_assert(offsetof(Vertex, water) == 48);
// 52 байта: все поля — float-ы по 4 байта, естественное выравнивание Vertex
// равно 4, поэтому stride равен sizeof и годится для вершинного буфера Vulkan.
static_assert(sizeof(Vertex) == 52);

}
