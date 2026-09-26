#include "renderer/shader.h"

#include <fstream>
#include <stdexcept>

#include "core/logger.h"

namespace renderer {

namespace {

// Проверка результата вызова Vulkan; при ошибке — исключение.
void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error (") + std::to_string(result) +
                                 ") в " + expr + " [" + file + ":" + std::to_string(line) +
                                 "]");
    }
}

}  // namespace

#define VK_CHECK(expr) checkVk((expr), #expr, __FILE__, __LINE__)

std::string shaderPath(const std::string& name) {
    return std::string(SHADERS_ROOT_DIR) + "/" + name;
}

std::vector<char> Shader::readFile(const std::string& filepath) {
    // Читаем с конца, чтобы сразу узнать размер файла.
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Shader: не удалось открыть файл: " + filepath);
    }

    const std::streamsize size = file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

Shader::Shader(VkDevice device, const std::string& filepath) : device_(device) {
    const std::vector<char> code = readFile(filepath);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VK_CHECK(vkCreateShaderModule(device_, &createInfo, nullptr, &module_));
    core::Logger::info("Shader: загружен модуль из \"" + filepath + "\"");
}

Shader::~Shader() {
    destroy();
}

Shader::Shader(Shader&& other) noexcept
    : device_(other.device_), module_(other.module_) {
    other.device_ = VK_NULL_HANDLE;
    other.module_ = VK_NULL_HANDLE;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        module_ = other.module_;
        other.device_ = VK_NULL_HANDLE;
        other.module_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Shader::destroy() {
    if (module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, module_, nullptr);
        module_ = VK_NULL_HANDLE;
    }
}

}  // namespace renderer