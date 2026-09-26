#include "renderer/model_loader.h"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace renderer {

namespace {

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;

struct FaceReference {
    std::size_t position = 0;
    std::optional<std::size_t> texture;
    std::optional<std::size_t> normal;
};

struct Face {
    std::vector<FaceReference> vertices;
};

struct VertexKey {
    std::int64_t position = -1;
    std::int64_t texture = -1;
    std::int64_t normal = -1;

    bool operator==(const VertexKey&) const = default;
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const noexcept {
        std::size_t result = std::hash<std::int64_t>{}(key.position);
        result ^= std::hash<std::int64_t>{}(key.texture) + 0x9e3779b9u + (result << 6u) +
                  (result >> 2u);
        result ^= std::hash<std::int64_t>{}(key.normal) + 0x9e3779b9u + (result << 6u) +
                  (result >> 2u);
        return result;
    }
};

[[noreturn]] void fail(const std::filesystem::path& path, std::size_t lineNumber,
                       const std::string& message) {
    throw std::runtime_error("OBJ loader: " + path.string() + ":" +
                             std::to_string(lineNumber) + ": " + message);
}

float parseFloat(const std::string& value, const std::filesystem::path& path,
                 std::size_t lineNumber, const char* field) {
    try {
        std::size_t consumed = 0;
        const float result = std::stof(value, &consumed);
        if (consumed != value.size() || !std::isfinite(result)) {
            fail(path, lineNumber, std::string("некорректное значение ") + field);
        }
        return result;
    } catch (const std::exception&) {
        fail(path, lineNumber, std::string("некорректное значение ") + field + ": " + value);
    }
}

std::size_t parseIndex(const std::string& value, std::size_t count,
                       const std::filesystem::path& path, std::size_t lineNumber,
                       const char* field) {
    if (value.empty()) {
        fail(path, lineNumber, std::string("пустой индекс ") + field);
    }

    long long parsed = 0;
    try {
        std::size_t consumed = 0;
        parsed = std::stoll(value, &consumed);
        if (consumed != value.size() || parsed == 0) {
            fail(path, lineNumber, std::string("некорректный индекс ") + field + ": " + value);
        }
    } catch (const std::exception&) {
        fail(path, lineNumber, std::string("некорректный индекс ") + field + ": " + value);
    }

    if (parsed > 0) {
        const auto positive = static_cast<unsigned long long>(parsed);
        if (positive > count) {
            fail(path, lineNumber, std::string("индекс ") + field + " выходит за границы");
        }
        return static_cast<std::size_t>(positive - 1);
    }

    const auto magnitude = static_cast<unsigned long long>(-(parsed + 1)) + 1;
    if (magnitude > count) {
        fail(path, lineNumber, std::string("отрицательный индекс ") + field +
                                  " выходит за границы");
    }
    return count - static_cast<std::size_t>(magnitude);
}

FaceReference parseFaceReference(const std::string& token, std::size_t positionCount,
                                 std::size_t textureCount, std::size_t normalCount,
                                 const std::filesystem::path& path,
                                 std::size_t lineNumber) {
    FaceReference reference;
    const std::size_t firstSlash = token.find('/');
    const std::string positionText = token.substr(0, firstSlash);
    reference.position = parseIndex(positionText, positionCount, path, lineNumber, "позиции");

    if (firstSlash == std::string::npos) {
        return reference;
    }

    const std::size_t secondSlash = token.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) {
        if (firstSlash + 1 == token.size()) {
            fail(path, lineNumber, "индекс UV не указан после '/'");
        }
        reference.texture = parseIndex(token.substr(firstSlash + 1), textureCount, path,
                                       lineNumber, "UV");
        return reference;
    }

    if (secondSlash != firstSlash + 1) {
        reference.texture = parseIndex(token.substr(firstSlash + 1, secondSlash - firstSlash - 1),
                                       textureCount, path, lineNumber, "UV");
    }

    if (secondSlash + 1 == token.size()) {
        fail(path, lineNumber, "индекс нормали не указан после '/'");
    }
    reference.normal = parseIndex(token.substr(secondSlash + 1), normalCount, path,
                                  lineNumber, "нормали");
    return reference;
}

Vec3 faceNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
    const float abx = b[0] - a[0];
    const float aby = b[1] - a[1];
    const float abz = b[2] - a[2];
    const float acx = c[0] - a[0];
    const float acy = c[1] - a[1];
    const float acz = c[2] - a[2];
    const float nx = aby * acz - abz * acy;
    const float ny = abz * acx - abx * acz;
    const float nz = abx * acy - aby * acx;
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (length <= std::numeric_limits<float>::epsilon()) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {nx / length, ny / length, nz / length};
}

}  // namespace

ModelData loadObj(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("OBJ loader: не удалось открыть файл: " + path.string());
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> textures;
    std::vector<Face> faces;
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        std::istringstream input(line);
        std::string type;
        if (!(input >> type)) {
            continue;
        }

        if (type == "v" || type == "vn") {
            std::string x;
            std::string y;
            std::string z;
            if (!(input >> x >> y >> z)) {
                fail(path, lineNumber, "ожидаются три компоненты");
            }
            const Vec3 value = {parseFloat(x, path, lineNumber, type.c_str()),
                                parseFloat(y, path, lineNumber, type.c_str()),
                                parseFloat(z, path, lineNumber, type.c_str())};
            if (type == "v") {
                positions.push_back(value);
            } else {
                normals.push_back(value);
            }
        } else if (type == "vt") {
            std::string x;
            std::string y;
            if (!(input >> x >> y)) {
                fail(path, lineNumber, "ожидаются координаты UV");
            }
            textures.push_back({parseFloat(x, path, lineNumber, "UV"),
                                parseFloat(y, path, lineNumber, "UV")});
        } else if (type == "f") {
            std::vector<std::string> tokens;
            std::string token;
            while (input >> token) {
                tokens.push_back(token);
            }
            if (tokens.size() < 3) {
                fail(path, lineNumber, "грань должна содержать хотя бы три вершины");
            }

            Face face;
            face.vertices.reserve(tokens.size());
            for (const std::string& faceToken : tokens) {
                face.vertices.push_back(parseFaceReference(
                    faceToken, positions.size(), textures.size(), normals.size(), path,
                    lineNumber));
            }
            faces.push_back(std::move(face));
        }
    }

    if (positions.empty()) {
        throw std::runtime_error("OBJ loader: в файле нет позиций: " + path.string());
    }
    if (faces.empty()) {
        throw std::runtime_error("OBJ loader: в файле нет граней: " + path.string());
    }

    std::vector<Vec3> generatedNormals;
    generatedNormals.reserve(faces.size());
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const Face& face = faces[faceIndex];
        const Vec3& a = positions[face.vertices[0].position];
        const Vec3& b = positions[face.vertices[1].position];
        const Vec3& c = positions[face.vertices[2].position];
        generatedNormals.push_back(faceNormal(a, b, c));
    }

    ModelData model;
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;
    vertexMap.reserve(faces.size() * 4);

    auto addVertex = [&](const FaceReference& reference, std::size_t faceIndex) {
        const Vec3& position = positions[reference.position];
        const Vec3& normal = reference.normal ? normals[*reference.normal]
                                              : generatedNormals[faceIndex];
        const Vec2& texture = reference.texture ? textures[*reference.texture]
                                                : Vec2{0.0f, 0.0f};
        const VertexKey key{
            static_cast<std::int64_t>(reference.position),
            reference.texture ? static_cast<std::int64_t>(*reference.texture) : -1,
            reference.normal ? static_cast<std::int64_t>(*reference.normal) : -1};

        if (const auto existing = vertexMap.find(key); existing != vertexMap.end()) {
            return existing->second;
        }

        if (model.vertices.size() >= std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("OBJ loader: слишком много вершин");
        }
        Vertex vertex{};
        for (std::size_t i = 0; i < 3; ++i) {
            vertex.position[i] = position[i];
            vertex.normal[i] = normal[i];
        }
        vertex.uv[0] = texture[0];
        vertex.uv[1] = texture[1];

        const auto index = static_cast<uint32_t>(model.vertices.size());
        model.vertices.push_back(vertex);
        vertexMap.emplace(key, index);
        return index;
    };

    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const Face& face = faces[faceIndex];
        for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i) {
            model.indices.push_back(addVertex(face.vertices[0], faceIndex));
            model.indices.push_back(addVertex(face.vertices[i], faceIndex));
            model.indices.push_back(addVertex(face.vertices[i + 1], faceIndex));
        }
    }

    if (model.vertices.empty() || model.indices.empty()) {
        throw std::runtime_error("OBJ loader: модель не содержит геометрии: " + path.string());
    }
    return model;
}

ModelData ModelLoader::load(const std::filesystem::path& path) {
    return ::renderer::loadObj(path);
}

ModelData ModelLoader::loadObj(const std::filesystem::path& path) {
    return ::renderer::loadObj(path);
}

}
