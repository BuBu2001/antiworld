#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace renderer {

// Загрузка скомпилированного шейдера (.spv) из файла и создание VkShaderModule.
class Shader {
public:
    Shader() = default;
    // Читает бинарный SPIR-V из filepath и создаёт модуль на device.
    Shader(VkDevice device, const std::string& filepath);
    ~Shader();

    // Владение уникальным Vulkan-ресурсом — копирование запрещено.
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // Уничтожает модуль; повторный вызов безопасен.
    void destroy();

    VkShaderModule handle() const { return module_; }

private:
    static std::vector<char> readFile(const std::string& filepath);

    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

// Полный путь к скомпилированному шейдеру.
// SHADERS_ROOT_DIR задаётся на этапе сборки (см. src/renderer/CMakeLists.txt).
std::string shaderPath(const std::string& name);

}  // namespace renderer