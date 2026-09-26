#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "renderer/vertex.h"

namespace renderer {

struct ModelData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

ModelData loadObj(const std::filesystem::path& path);

class ModelLoader {
public:
    static ModelData load(const std::filesystem::path& path);
    static ModelData loadObj(const std::filesystem::path& path);
};

}
