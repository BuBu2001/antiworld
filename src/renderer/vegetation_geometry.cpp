#include "renderer/vegetation_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>

#include "core/double_math.h"

namespace renderer::vegetation {

namespace {

// Детерминированный ГПСЧ (splitmix64). Отдельный от <random> намеренно:
// нам нужно, чтобы геометрия растения зависела ТОЛЬКО от seed, а
// std::mt19937 на разных платформах даёт разные последовательности, и
// «одно и то же» дерево разъезжалось бы между сборками.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept
        : state_(seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull) {}

    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    float unit() noexcept {
        return static_cast<float>(next() >> 40) / 16777216.0f;
    }
    // [-1, 1)
    float sym() noexcept { return unit() * 2.0f - 1.0f; }
    float range(float lo, float hi) noexcept { return lo + (hi - lo) * unit(); }

private:
    std::uint64_t state_;
};

glm::vec3 mix3(const glm::vec3& a, const glm::vec3& b, float t) noexcept {
    return glm::vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                     a.z + (b.z - a.z) * t};
}

// Накопитель геометрии растения.
//
// Три замечания к устройству, каждое — про конкретный класс ошибок:
//
//  1. Нормали считаются ПО ТРЕУГОЛЬНИКАМ, а не усреднением по вершинам.
//     Усреднение требует общих вершин между гранями, а у листвы они и не
//     должны быть общими: каждый лепесток плоский, и усреднение склеило бы их
//     в «размазанную» поверхность, по которой свет течёт как по шару.
//  2. uv.y нормализуется ОДИН раз в finalize(), по фактической высоте
//     вершины. Считать его в каждом вызывающем коде — значит забыть про
//     uv.y у одной из фигур, и тогда эта фигура не гнётся или гнётся
//     неправильно.
//  3. Порядок обхода треугольников задаётся так, что нормаль смотрит НАРУЖУ
//     (CCW при включённом face culling). Внутренние грани у листвы не нужны,
//     поэтому экономия тут двусторонняя.
class Builder {
public:
    Builder(float plantHeight, float maxFlex) noexcept
        : plantHeight_(std::max(plantHeight, 1e-3f)), maxFlex_(maxFlex) {}

    void tri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float flex,
             const glm::vec3& color, float snowBias) {
        const glm::vec3 raw = glm::cross(b - a, c - a);
        const float len2 = glm::dot(raw, raw);
        // Вырожденный треугольник (совпавшие точки) даёт нулевой вектор, и
        // normalize() вернёт NaN, который распространит NaN по всему шейдеру
        // через вершину. Подставляем безопасную нормаль.
        const glm::vec3 n = (len2 > 1e-18f)
                                ? glm::vec3{raw.x / std::sqrt(len2), raw.y / std::sqrt(len2),
                                            raw.z / std::sqrt(len2)}
                                : glm::vec3{0.0f, 1.0f, 0.0f};
        const std::uint32_t base = static_cast<std::uint32_t>(model_.vertices.size());
        for (const glm::vec3& p : {a, b, c}) {
            Vertex v{};
            v.position[0] = p.x;
            v.position[1] = p.y;
            v.position[2] = p.z;
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            // Гибкость растёт кверху: у основания ствол жёсткий, у макушки
            // ветка гнётся сильнее всего. Домножаем на maxFlex_, потому что
            // трава должна гнуться целиком, а ствол дерева — почти никак.
            const float t = std::clamp(p.y / plantHeight_, 0.0f, 1.0f);
            v.uv[0] = std::clamp(flex * (0.35f + 0.65f * t), 0.0f, maxFlex_);
            v.uv[1] = t;
            // Кончики суше и светлее основания — так трава и хвоя выглядят
            // живыми без единой текстуры.
            v.color[0] = color.x;
            v.color[1] = color.y;
            v.color[2] = color.z;
            v.snowBias = snowBias;
            v.water = 0.0f;
            model_.vertices.push_back(v);
        }
        model_.indices.push_back(base);
        model_.indices.push_back(base + 1);
        model_.indices.push_back(base + 2);
    }

    void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
              const glm::vec3& d, float flex, const glm::vec3& color, float snowBias) {
        tri(a, b, c, flex, color, snowBias);
        tri(a, c, d, flex, color, snowBias);
    }

    ModelData finish() { return std::move(model_); }

private:
    ModelData model_;
    float plantHeight_;
    float maxFlex_;
};

// Кольцо из count точек на радиусе radius в плоскости y = height, с
// небольшим deterministic-разбросом по углу, чтобы ярусы не выглядели
// идеально круглыми (идеальная окружность на 8 сегментах читается как
// «вычислительная», а не как ветка).
std::array<glm::vec3, 12> ring(const glm::vec3& center, float radius, float height,
                               std::size_t count, Rng& rng, float jitter) {
    std::array<glm::vec3, 12> out{};
    for (std::size_t i = 0; i < count && i < out.size(); ++i) {
        const float base = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        const float angle = base + rng.sym() * jitter;
        const float r = radius * (1.0f + rng.sym() * jitter * 0.5f);
        out[i] = glm::vec3{center.x + std::cos(angle) * r, center.y + height,
                           center.z + std::sin(angle) * r};
    }
    return out;
}

// Усечённый конус (фрустум) от радиуса r0 на высоте y0 до радиуса r1 на
// высоте y1. Основание закрывается всегда: иначе у яруса ели снизу (а игрок
// ходит под деревьями) был бы виден просвет внутрь кроны.
void addFrustum(Builder& b, const glm::vec3& center, float y0, float r0, float y1, float r1,
                std::size_t count, Rng& rng, float flex, const glm::vec3& color,
                float snowBias, float jitter = 0.06f) {
    const auto low = ring(center, r0, y0, count, rng, jitter);
    const auto high = ring(center, r1, y1, count, rng, jitter);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t j = (i + 1) % count;
        // Порядок именно low[i] → high[i] → high[j] → low[j]. Кольца идут по
        // возрастанию угла, поэтому обход «снизу по часовой» дал бы нормаль,
        // направленную ВНУТРЬ конуса: при выключенном face culling (у нас он
        // выключен) такая грань видна, но освещается изнутри — ярус хвои
        // выглядел бы тёмным вывернутым «каркасом».
        b.quad(low[i], high[i], high[j], low[j], flex, color, snowBias);
    }
    if (r0 > 1e-4f) {
        const glm::vec3 hub = center + glm::vec3{0.0f, y0, 0.0f};
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t j = (i + 1) % count;
            // Обход снизу: нормаль смотрит вниз, наружу от конуса.
            b.tri(hub, low[i], low[j], flex, mix3(color, color * 0.75f, 0.5f), snowBias);
        }
    }
}

// Трубчатая ветвь: конус без крышек, от точки a к точке b. Ветви у лиственного
// и расходятся, поэтому трубка строится по двум точкам, а не по высоте.
void addBranch(Builder& b, const glm::vec3& a, const glm::vec3& c, float radius, std::size_t count,
               Rng& rng, float flex, const glm::vec3& color, float snowBias) {
    const glm::vec3 axis = c - a;
    const float len = glm::length(axis);
    if (len < 1e-4f) return;
    const glm::vec3 dir = axis / len;
    // Ортогональный базис к оси: up degeneracy, когда ветка вертикальна,
    // поэтому подбираем опорный вектор так, чтобы он не был параллелен оси.
    const glm::vec3 helper = (std::abs(dir.y) > 0.9f) ? glm::vec3{1.0f, 0.0f, 0.0f}
                                                      : glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 side = glm::normalize(glm::cross(dir, helper));
    const glm::vec3 up = glm::cross(side, dir);
    const float endRadius = radius * 0.45f;

    std::array<glm::vec3, 12> startRing{};
    std::array<glm::vec3, 12> endRing{};
    for (std::size_t i = 0; i < count; ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        const float wobble = 1.0f + rng.sym() * 0.10f;
        const glm::vec3 off = (side * std::cos(angle) + up * std::sin(angle)) * wobble;
        startRing[i] = a + off * radius;
        endRing[i] = c + off * endRadius;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t j = (i + 1) % count;
        // Как и в addFrustum: обход от текущего кольца к следующему, иначе
        // нормаль трубки смотрит внутрь и ветка освещается наоборот.
        b.quad(startRing[i], endRing[i], endRing[j], startRing[j], flex, color, snowBias);
    }
}

// Низкополигональный эллипсоид — крона куста или лиственного дерева.
// rings — число колец ПО КУПОЛУ (не считая полюсов), segments — по окружности.
void addBlob(Builder& b, const glm::vec3& center, const glm::vec3& radii, std::size_t segments,
             std::size_t rings, Rng& rng, float flex, const glm::vec3& color,
             const glm::vec3& tipColor, float snowBias) {
    if (segments < 3 || rings < 2) return;
    // Кольца идут от верхнего полюса к нижнему.
    std::vector<std::array<glm::vec3, 12>> rows;
    rows.reserve(rings + 1);
    for (std::size_t r = 0; r <= rings; ++r) {
        // phi от 0 (верх) до PI (низ), со смещением на полшага, чтобы верхний
        // полюс не был отдельной вершиной и не давал вырожденных треугольников.
        const float t = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = 3.14159265f * t;
        const float sy = std::cos(phi);
        const float sr = std::sin(phi);
        std::array<glm::vec3, 12> row{};
        for (std::size_t i = 0; i < segments && i < row.size(); ++i) {
            const float angle =
                6.2831853f * static_cast<float>(i) / static_cast<float>(segments) +
                (r % 2 == 0 ? 0.0f : 3.14159265f / static_cast<float>(segments));
            const float wob = 1.0f + rng.sym() * 0.12f;
            row[i] = center + glm::vec3{std::cos(angle) * sr * radii.x * wob,
                                        sy * radii.y, std::sin(angle) * sr * radii.z * wob};
        }
        rows.push_back(row);
    }
    const std::size_t topSegs = segments;
    for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
        for (std::size_t i = 0; i < topSegs && i < 12; ++i) {
            const std::size_t j = (i + 1) % topSegs;
            // Цвет по высоте кроны: верх светлее.
            const float upness = 0.5f * (1.0f - static_cast<float>(r) / static_cast<float>(rings));
            b.quad(rows[r][i], rows[r][j], rows[r + 1][j], rows[r + 1][i], flex,
                   mix3(color, tipColor, upness), snowBias);
        }
    }
}

// Трава: одна узкая blade с изгибом. Три квада, сужающихся к кончику, и
// изгиб в сторону, чтобы пучок не выглядел пучком прямых палок.
void addBlade(Builder& b, const glm::vec3& base, const glm::vec3& direction, float height,
              float width, float bend, float flex, const glm::vec3& color,
              const glm::vec3& tipColor, float snowBias, std::size_t segments = 3) {
    const glm::vec3 sideAxis = glm::normalize(glm::cross(direction, glm::vec3{0.0f, 0.0f, 1.0f}) +
                                               glm::vec3{1e-4f, 0.0f, 0.0f});
    const glm::vec3 bendAxis = glm::cross(sideAxis, glm::normalize(direction));
    std::array<glm::vec3, 12> left{};
    std::array<glm::vec3, 12> right{};
    for (std::size_t i = 0; i <= segments && i < left.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        // Положение вдоль blade: подъём + наклон. Квадрат t задаёт «уход» blade
        // в сторону, поэтому у основания она стоит вертикально.
        const float rise = t;
        const float lean = bend * t * t;
        const glm::vec3 p = base + direction * (height * rise) + bendAxis * (height * lean);
        // Ширина сужается к кончику, но не в ноль: нулевая ширина даёт
        // вырожденные треугольники на последнем сегменте.
        const float w = width * (1.0f - 0.75f * t) * 0.5f;
        left[i] = p - sideAxis * w;
        right[i] = p + sideAxis * w;
    }
    for (std::size_t i = 0; i < segments; ++i) {
        const float tMid = (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
        b.quad(left[i], right[i], right[i + 1], left[i + 1], flex,
               mix3(color, tipColor, tMid), snowBias);
    }
}

constexpr float kPi = 3.14159265f;

}  // namespace

SmallSpec coniferSpec(std::uint32_t seed) {
    Rng rng{0xC0FFEE00ull + seed};
    SmallSpec spec;
    spec.height = rng.range(7.0f, 15.0f);
    spec.spread = spec.height * rng.range(0.16f, 0.24f);
    // Хвоя холоднее зелёного: в неё попадает много синего неба.
    spec.color = {rng.range(0.08f, 0.14f), rng.range(0.20f, 0.28f), rng.range(0.12f, 0.18f)};
    spec.tipColor = {spec.color.r * 1.5f + 0.06f, spec.color.g * 1.35f, spec.color.b * 1.3f};
    spec.snowBias = 0.55f;  // хвоя держит снег заметно лучше листвы
    spec.maxFlex = 0.25f;   // ствол и толстые ветки почти не гнутся
    return spec;
}

SmallSpec broadleafSpec(std::uint32_t seed) {
    Rng rng{0xBEEF1234ull + seed};
    SmallSpec spec;
    spec.height = rng.range(6.0f, 12.0f);
    spec.spread = spec.height * rng.range(0.30f, 0.46f);
    spec.color = {rng.range(0.16f, 0.26f), rng.range(0.30f, 0.42f), rng.range(0.10f, 0.18f)};
    spec.tipColor = {spec.color.r * 1.6f + 0.08f, spec.color.g * 1.4f, spec.color.b * 1.5f};
    spec.snowBias = 0.25f;  // листва снег почти не держит
    spec.maxFlex = 0.45f;
    return spec;
}

SmallSpec shrubSpec(std::uint32_t seed) {
    Rng rng{0x5AD0BEEFull + seed};
    SmallSpec spec;
    spec.height = rng.range(0.7f, 1.8f);
    spec.spread = spec.height * rng.range(0.9f, 1.5f);
    spec.color = {rng.range(0.22f, 0.34f), rng.range(0.28f, 0.40f), rng.range(0.12f, 0.20f)};
    spec.tipColor = {spec.color.r * 1.4f, spec.color.g * 1.3f, spec.color.b * 1.4f};
    spec.snowBias = 0.15f;
    spec.maxFlex = 0.7f;
    return spec;
}

SmallSpec grassSpec(std::uint32_t seed) {
    Rng rng{0x6A5511ull + seed};
    SmallSpec spec;
    spec.height = rng.range(0.25f, 0.65f);
    spec.spread = spec.height * rng.range(0.5f, 1.1f);
    // Трава — самое светлое и самое жёлтое, что растёт: тонкий лист просвечивает.
    spec.color = {rng.range(0.20f, 0.30f), rng.range(0.34f, 0.46f), rng.range(0.10f, 0.18f)};
    spec.tipColor = {spec.color.r * 1.9f + 0.10f, spec.color.g * 1.6f, spec.color.b * 1.2f};
    spec.snowBias = 0.02f;  // снег на траве не лежит: слишком мало высоты
    spec.maxFlex = 1.0f;   // гнётся целиком
    return spec;
}

float trunkRadius(const SmallSpec& spec, bool broadleaf) {
    // Лиственный ствол заметно толще хвойного: он держит раскидистую крону.
    return spec.height * (broadleaf ? 0.030f : 0.018f);
}

float trunkTopHeight(const SmallSpec& spec, bool broadleaf) {
    return spec.height * (broadleaf ? 0.40f : 0.98f);
}

ModelData makeConifer(const SmallSpec& spec) {
    Rng rng{0x1234ABCDull};
    Builder b(spec.height, spec.maxFlex);
    const std::size_t seg = 7;
    const float trunkR = trunkRadius(spec, /*broadleaf=*/false);
    // Ствол на всю высоту: у хвойного он виден и внутри кроны, а без него
    // ярусы выглядят как парящие пирамиды.
    addFrustum(b, {0.0f, 0.0f, 0.0f}, 0.0f, trunkR, trunkTopHeight(spec, false), trunkR * 0.35f,
               seg, rng, 0.05f, spec.color * 0.5f, spec.snowBias, 0.0f);

    // Ярусы. Нижние шире и начинаются низко, верхние узкие и сидят выше;
    // именно сужение рядами, а не один шар, читается как хвоя.
    const std::size_t tiers = 5;
    for (std::size_t t = 0; t < tiers; ++t) {
        const float f = static_cast<float>(t) / static_cast<float>(tiers - 1);
        const float baseY = spec.height * (0.14f + 0.62f * f);
        const float tierH = spec.height * (0.30f - 0.13f * f);
        const float radius = spec.spread * (1.0f - 0.62f * f) * (1.0f + rng.sym() * 0.08f);
        const glm::vec3 tint = mix3(spec.color, spec.tipColor, 0.25f * f);
        addFrustum(b, {0.0f, 0.0f, 0.0f}, baseY, radius, baseY + tierH, radius * 0.14f, seg,
                   rng, 0.35f + 0.45f * f, tint, spec.snowBias);
    }
    // Макушка: один узкий конус, чтобы дерево не заканчивалось плоским срезом.
    addFrustum(b, {0.0f, 0.0f, 0.0f}, spec.height * 0.90f, spec.spread * 0.16f, spec.height,
               0.0f, seg, rng, 0.85f, spec.tipColor, spec.snowBias);
    return b.finish();
}

ModelData makeBroadleaf(const SmallSpec& spec) {
    Rng rng{0x99887766ull};
    Builder b(spec.height, spec.maxFlex);
    const std::size_t seg = 6;
    const float trunkR = trunkRadius(spec, /*broadleaf=*/true);
    const float forkY = spec.height * rng.range(0.34f, 0.46f);
    addFrustum(b, {0.0f, 0.0f, 0.0f}, 0.0f, trunkR, forkY, trunkR * 0.62f, seg, rng, 0.04f,
               spec.color * 0.45f, spec.snowBias, 0.0f);

    // Крона из нескольких перекрывающихся эллипсоидов вокруг общего центра: один
    // эллипсоид читается как мяч, а перекрытие — как листва.
    const glm::vec3 crown{0.0f, spec.height * 0.70f, 0.0f};
    const float crownR = spec.spread * 0.5f;
    const std::size_t blobs = 4;
    for (std::size_t i = 0; i < blobs; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(blobs) + rng.sym();
        const float off = crownR * rng.range(0.35f, 0.62f);
        const glm::vec3 center = crown + glm::vec3{std::cos(a) * off,
                                                   rng.range(-0.10f, 0.18f) * spec.height,
                                                   std::sin(a) * off};
        const float r = crownR * rng.range(0.58f, 0.86f);
        // Ветка к клону кроны: без неё крона висит в воздухе над стволом.
        addBranch(b, {0.0f, forkY, 0.0f}, center, trunkR * 0.55f, 5, rng, 0.35f,
                  spec.color * 0.5f, spec.snowBias);
        addBlob(b, center, {r * rng.range(0.85f, 1.15f), r * rng.range(0.7f, 0.95f), r},
                seg, 3, rng, 0.75f, spec.color, spec.tipColor, spec.snowBias);
    }
    return b.finish();
}

ModelData makeShrub(const SmallSpec& spec) {
    Rng rng{0x4D2A1B33ull};
    Builder b(spec.height, spec.maxFlex);
    const float r = spec.spread * 0.5f;
    // ОДИН купол, а не три шара.
    //
    // Раньше здесь было blobs = 3: три сферы, разнесённые на 0.3 радиуса.
    // Они почти не перекрывались, и куст читался как три отдельных шара —
    // то есть на вид «одно растение» из трёх отдельных объектов. Платили мы
    // за это и треугольниками, и числом объектов в сцене: 11 700 кустов по
    // 124 треугольника — это 1.46 млн треугольников, больше всех остальных
    // видов вместе, на самую мелкую растительность.
    //
    // Теперь форма одна и непрерывная: сплюснутый купол, сидящий на земле.
    // Сплюснутость по Y и смещение вниз убирают вид «парящего шара», а две
    // стволика у земли дают опору.
    addBlob(b, {0.0f, spec.height * 0.40f, 0.0f},
            {r * 0.95f, spec.height * 0.50f, r * 0.95f}, 8, 3, rng, 0.85f, spec.color,
            spec.tipColor, spec.snowBias);
    // Пара стволиков у земли, чтобы куст не выглядел парящим шаром.
    for (std::size_t i = 0; i < 2; ++i) {
        const float a = rng.range(0.0f, 2.0f * kPi);
        addBranch(b, {0.0f, 0.0f, 0.0f},
                  {std::cos(a) * r * 0.2f, spec.height * 0.35f, std::sin(a) * r * 0.2f},
                  spec.height * 0.035f, 4, rng, 0.2f, spec.color * 0.45f, spec.snowBias);
    }
    return b.finish();
}

ModelData makeGrassTuft(const SmallSpec& spec) {
    Rng rng{0x7777ABCDull};
    Builder b(spec.height, spec.maxFlex);
    // Пять blades разной высоты и направления: одинаковые выглядят как
    // расчёсанная щетина, разные — как пучок.
    constexpr std::size_t kBlades = 5;
    for (std::size_t i = 0; i < kBlades; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kBlades) +
                        rng.sym() * 0.5f;
        const float lean = rng.range(0.15f, 0.45f);
        const glm::vec3 dir = glm::vec3{std::cos(a) * lean, 1.0f, std::sin(a) * lean};
        const glm::vec3 base{std::cos(a) * spec.spread * 0.16f, 0.0f,
                             std::sin(a) * spec.spread * 0.16f};
        addBlade(b, base, dir, spec.height * rng.range(0.65f, 1.0f),
                 spec.spread * rng.range(0.10f, 0.17f), rng.range(0.25f, 0.6f), 0.9f, spec.color,
                 spec.tipColor, spec.snowBias);
    }
    return b.finish();
}

Bounds boundsOf(const ModelData& model) {
    Bounds box;
    box.min = glm::vec3{1e30f, 1e30f, 1e30f};
    box.max = glm::vec3{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : model.vertices) {
        const glm::vec3 p{v.position[0], v.position[1], v.position[2]};
        box.min = glm::min(box.min, p);
        box.max = glm::max(box.max, p);
    }
    if (model.vertices.empty()) {
        box.min = glm::vec3{0.0f};
        box.max = glm::vec3{0.0f};
    }
    return box;
}

}  // namespace renderer::vegetation
