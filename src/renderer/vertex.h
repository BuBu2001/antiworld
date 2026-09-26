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
};

static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, normal) == 12);
static_assert(offsetof(Vertex, uv) == 24);
static_assert(offsetof(Vertex, color) == 32);
static_assert(offsetof(Vertex, snowBias) == 44);
// 48 байт — кратно 16, поэтому вершина выровнена как std140-вектор.
static_assert(sizeof(Vertex) == 48);

}
