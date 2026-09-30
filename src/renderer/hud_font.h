#pragma once

#include <cstdint>
#include <vector>

namespace renderer {

// Растровый шрифт 5x7 для HUD.
//
// В проекте нет ни TTF, ни ImGui, ни готовых текстур с символами: единственный
// ассет — cube.obj. Поэтому шрифт задаётся прямо в коде и запекается в
// атлас на CPU при создании HUD.
//
// Атлас: сетка 16x4 ячеек по 6x8 пикселей (глиф 5x7 + пиксель запаса справа и
// снизу), итого 96x32 пикселя R8. Индекс ячейки = ASCII-код - 32, покрыт
// диапазон 32..95: пробел, пунктуация, цифры и заглавные буквы. Символы вне
// диапазона и незаполненные ячейки пустые.
//
// Глифы заданы СТРОКАМИ, а не битовыми масками: символ "#" — включённый
// пиксель, "." — выключенный. Так опечатку в глифе видно глазами прямо в
// исходнике, а в масках ошибку не отличить от задуманного рисунка.
struct HudFontAtlas {
    std::vector<uint8_t> pixels;  // R8, ширина*высота
    uint32_t width = 0;
    uint32_t height = 0;
};

// Размеры атласа. Должны совпадать с kAtlasW/kAtlasH в shaders/hud.frag.
inline constexpr uint32_t kHudAtlasWidth = 96;
inline constexpr uint32_t kHudAtlasHeight = 32;
inline constexpr uint32_t kHudAtlasCols = 16;
inline constexpr uint32_t kHudAtlasRows = 4;
inline constexpr uint32_t kHudCellW = 6;
inline constexpr uint32_t kHudCellH = 8;
inline constexpr uint32_t kHudGlyphW = 5;
inline constexpr uint32_t kHudGlyphH = 7;
// Символов в строке HUD: 16 на каждый uvec4 в UBO, четыре uvec4.
inline constexpr uint32_t kHudMaxChars = 64;

// Собирает атлас. Детерминированно, без аллокаций на каждый кадр.
HudFontAtlas buildHudFontAtlas();

}  // namespace renderer
