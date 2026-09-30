#version 450

// HUD: счётчик FPS в левом верхнем углу.
//
// Рисуется ПОСЛЕ геометрии сцены и неба, отдельным пайплайном с
// альфа-смешиванием и без depth test, поэтому перекрывает карту, не записывая
// в неё глубину. Фрагментный шейдер сам решает, что попало в его пиксель:
// номер столбца даёт символ строки, номер строки — линию, а внутри ячейки
// атлас даёт глиф.
//
// Атлас шрифта генерируется на CPU (HudPass): сетка 16x4 ячеек по 6x8
// пикселей, в каждой ячейке глиф 5x7 и один пиксель запаса справа и снизу.
// Индекс ячейки = код символа - 32, то есть покрыт printable ASCII 32..95:
// пробел, цифры, заглавные буквы и пунктуация. Строчные приводятся к
// верхнему регистру на стороне CPU, символы вне диапазона пустые (в атласе нули).
//
// Сглаживание ручное, 2x2 сэмпла на пиксель: ближний фильтр не размазывает
// соседние ячейки, а обход всех 35 текселей глифа дорог. Четыре сэмпла дают
// читаемый край при цене, незаметной на сотне пикселей.

layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

// std140: vec4 занимает 16 байт, uvec4[16] — 256 байт. Итого 288 байт UBO.
layout(std140, binding = 1) uniform HudUniforms {
    // x — ширина окна в пикселях, y — высота, z — размер глифа в экранных
    // пикселях (один тексель шрифта), w — число символов в строке.
    vec4 viewport;
    // xy — отступ от левого верхнего угла окна до панели HUD, в пикселях.
    // Скролл панели задаётся снаружи (см. HudPass::setScroll).
    vec4 origin;
    // До 64 символов строки, по одному ASCII-коду на uint.
    uvec4 text[16];
} hud;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// Размеры атласа в текселях. Должны совпадать с константами
// HudPass::kAtlasWidth/kAtlasHeight, иначе символы поедут по диагонали.
const float kAtlasW = 96.0;
const float kAtlasH = 32.0;

// Ячейка: глиф 5x7 плюс по одному пикселю запаса справа и снизу.
const float kCellW = 6.0;
const float kCellH = 8.0;
const float kGlyphW = 5.0;
const float kGlyphH = 7.0;

const uint kAtlasCols = 16u;

// Символ строки по его номеру. В строке до 64 символов, по 16 на каждый uvec4.
uint charAt(float charIndex) {
    const uint idx = uint(charIndex);
    uint ch = 32u;  // пробел
    if (idx < 16u) {
        ch = hud.text[0][idx];
    } else if (idx < 32u) {
        ch = hud.text[1][idx - 16u];
    } else if (idx < 48u) {
        ch = hud.text[2][idx - 32u];
    } else {
        ch = hud.text[3][idx - 48u];
    }
    // Вне printable ASCII символ считаем пустым.
    if (ch < 32u || ch > 95u) {
        return 32u;
    }
    return ch;
}

// Покрытие глифа в точке px — координаты внутри панели, в пикселях от её
// левого верхнего угла, ось Y вниз.
float glyphCoverage(vec2 px, float scale) {
    const float advance = kCellW * scale;
    const float lineHeight = kCellH * scale;
    const float charIndex = floor(px.x / advance);
    if (charIndex < 0.0 || charIndex >= hud.viewport.w) {
        return 0.0;
    }
    const float lineIndex = floor(px.y / lineHeight);
    if (lineIndex < 0.0 || lineIndex >= 1.0) {
        return 0.0;
    }

    const uint cell = charAt(charIndex) - 32u;
    const int col = int(cell % kAtlasCols);
    const int row = int(cell / kAtlasCols);

    // Положение внутри ячейки, в текселях атласа.
    const vec2 local =
        vec2(px.x - charIndex * advance, px.y - lineIndex * lineHeight) / scale;
    if (local.x < 0.0 || local.y < 0.0 || local.x >= kGlyphW || local.y >= kGlyphH) {
        return 0.0;  // пиксель запаса или соседняя ячейка
    }

    const vec2 texelOrigin = vec2(float(col) * kCellW, float(row) * kCellH);
    float hits = 0.0;
    for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
            // Смещение внутри пикселя: берём тексель, в который попадает
            // сэмпл, и сравниваем его с центром глифа.
            const vec2 sub = (vec2(float(sx), float(sy)) + 0.5) * 0.5;
            const vec2 g = local + sub;
            if (g.x >= kGlyphW || g.y >= kGlyphH) {
                continue;
            }
            const vec2 texel = texelOrigin + floor(g) + 0.5;
            if (texture(fontAtlas, texel / vec2(kAtlasW, kAtlasH)).r > 0.5) {
                hits += 1.0;
            }
        }
    }
    return hits * 0.25;
}

void main() {
    const float scale = hud.viewport.z;
    // Панель: ряд ячеек плюс отступ вокруг текста.
    const vec2 panelSize = vec2(hud.viewport.w * kCellW * scale + 8.0 * scale,
                                kCellH * scale + 6.0 * scale);

    // gl_FragCoord растёт вверх, а HUD прижат к верхней кромке окна.
    const vec2 px = vec2(gl_FragCoord.x - hud.origin.x,
                         hud.viewport.y - gl_FragCoord.y - hud.origin.y);

    if (px.x < 0.0 || px.y < 0.0 || px.x >= panelSize.x || px.y >= panelSize.y) {
        discard;
    }

    // Текст со сдвигом внутри панели и тень на 1.5 пикселя вниз-вправо:
    // иначе белый текст не читается ни на снегу, ни на светлом песке.
    const vec2 inset = vec2(3.0 * scale, 2.0 * scale);
    const float text = glyphCoverage(px - inset, scale);
    const float shadow = glyphCoverage(px - inset - vec2(1.5, 1.5), scale);

    // Подложка под текстом — полупрозрачная тёмная панель.
    const float kPanelAlpha = 0.38;
    vec4 color = vec4(vec3(0.0), kPanelAlpha);
    if (shadow > 0.0) {
        color = vec4(vec3(0.0), max(kPanelAlpha, 0.85 * shadow));
    }
    if (text > 0.0) {
        color = vec4(vec3(1.0), max(kPanelAlpha, 0.35 + 0.65 * text));
    }
    outColor = color;
}
