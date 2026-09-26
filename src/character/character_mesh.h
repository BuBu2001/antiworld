#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "character/body_parts.h"
#include "character/skeleton.h"
#include "renderer/model_loader.h"

namespace character {

// Процедурная генерация меша персонажа: тело строится из параметрических
// примитивов (сегментированные капсулы конечностей, сфероид головы с чертами
// лица, боксы туловища со скруглением), каждая вершина несёт индекс кости и
// вес — то есть меш сразу скиннут на скелет и анимируется позой костей.
//
// Это «реалистичный» уровень детализации для процедурной графики: пальцы с
// ногтевыми фалангами, нос, уши, глаза, брови, рот, шея, грудные мышцы,
// таз, колени, пятки и носки стоп. Для фотореализма нужен authored-ассет,
// но генерация по этому же скелету позволяет менять рост/пропорции на лету.
class CharacterMeshGenerator {
public:
    struct SkinVertex {
        renderer::Vertex vertex;  // позиция, нормаль, цвет уже заполнены
        int bone0{0};             // кость влияния (одна на жёсткий сегмент)
        float weight{1.0f};       // зарезервировано под блендинг (всегда 1)
    };

    struct SkinnedModel {
        std::vector<SkinVertex> vertices;
        std::vector<uint32_t> indices;
        // Матрицы inverse-bind того же скелета, что использовался при
        // генерации: skinMatrix[i] = animated(bind_i) * inverseBind_i.
        std::array<glm::mat4, Skeleton::kBoneCount> inverseBind{};
    };

    static SkinnedModel generate(const Skeleton& skeleton, const Palette& palette);

    // Плоский вариант для существующего рендера без индексов костей: тот же
    // меш, но разложенный в обычный ModelData (позиция уже в bind-pose).
    static renderer::ModelData generateStatic(const Skeleton& skeleton,
                                              const Palette& palette);
};

}  // namespace character
