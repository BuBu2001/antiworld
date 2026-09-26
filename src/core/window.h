#pragma once

// GLFW_INCLUDE_VULKAN подключает Vulkan API после GLFW, чтобы работать с
// Vulkan-инстансами/поверхностями прямо из GLFW-заголовков.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>

namespace core {

// Обёртка над GLFW-окном.
// Владеет GLFWwindow, регистрирует колбэки событий и контролирует закрытие.
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    // Окно нельзя копировать (владение единственное).
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Сырой указатель GLFW — нужен Vulkan-слою и системе ввода.
    GLFWwindow* handle() const { return window_; }

    // Обрабатывает очередь событий GLFW (обязательно вызывать каждый кадр).
    void pollEvents();

    // true, если пользователь запросил закрытие окна (крестик, Alt+F4 и т.п.).
    bool shouldClose() const;

    // Размеры буфера кадра в логических/физических пикселях.
    int framebufferWidth() const;
    int framebufferHeight() const;

    // Сигнал о том, что окно было изменено (нужно пересоздать swapchain).
    bool framebufferResized() const { return framebufferResized_; }
    void resetFramebufferResized() { framebufferResized_ = false; }

private:
    // Статический мост из GLFW-колбэка в объект Window.
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    void initCallbacks();

    GLFWwindow* window_ = nullptr;  // созданное GLFW-окно (nullptr до создания)
    bool framebufferResized_ = false;  // окно меняло размер с момента последнего сброса
};

}  // namespace core