#include "core/window.h"

#include <stdexcept>

#include "core/input.h"
#include "core/logger.h"

namespace core {

namespace {
// Счётчик активных окон: glfwInit/glfwTerminate должны вызываться ровно один
// раз на всё приложение (glfwTerminate при живом втором окне убил бы GLFW для
// всего процесса, а повторный glfwInit — ошибка).
int g_glfwRefCount = 0;
}  // namespace

Window::Window(int width, int height, const std::string& title) {
    // Инициализируем GLFW при создании первого окна.
    if (g_glfwRefCount == 0) {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Window: не удалось инициализировать GLFW");
        }
    }

    // Окно будет использовать Vulkan, поэтому клиентский API отключаем.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Изменяемое окно — чтобы задействовать пересоздание swapchain при resize.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        if (--g_glfwRefCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Window: glfwCreateWindow вернул nullptr");
    }
    ++g_glfwRefCount;

    initCallbacks();
    // Система ввода читает состояние напрямую через GLFWwindow.
    Input::bind(window_);

    core::Logger::info("Window: создано окно " + std::to_string(width) + "x" +
                       std::to_string(height) + " \"" + title + "\"");
}

Window::~Window() {
    if (window_ != nullptr) {
        Input::bind(nullptr);  // отвязываем окно: иначе Input останется с висящим указателем
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    // Завершаем GLFW только когда закрыто последнее окно.
    if (--g_glfwRefCount == 0) {
        glfwTerminate();
    }
}

void Window::pollEvents() {
    // Обрабатывает все ожидающие события и вызывает зарегистрированные колбэки.
    glfwPollEvents();
    // Новый кадр ввода: дельты мыши считаются «на кадр», поэтому флаги
    // consumption сбрасываются именно здесь (Input читается после pollEvents).
    Input::startFrame();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

int Window::framebufferWidth() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return width;
}

int Window::framebufferHeight() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return height;
}

void Window::initCallbacks() {
    // Сохраняем указатель на this, чтобы достать его в static-колбэках GLFW.
    glfwSetWindowUserPointer(window_, this);

    glfwSetFramebufferSizeCallback(window_, &Window::framebufferResizeCallback);
    // Прокрутка колеса не нужна окну — передаём состояние системе ввода.
    glfwSetScrollCallback(window_, [](GLFWwindow*, double xoffset, double yoffset) {
        Input::onScroll(xoffset, yoffset);
    });
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
        return;
    }
    // true даже при сворачивании (0x0) — VulkanBase учтёт это при пересоздании.
    self->framebufferResized_ = true;
    (void)width;
    (void)height;
}

}  // namespace core