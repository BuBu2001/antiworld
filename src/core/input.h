#pragma once

// GLFW_INCLUDE_VULKAN подключает Vulkan API после GLFW.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace core {

// Простая система ввода (клавиатура, мышь, колесо прокрутки).
// Header-only: состояние читается напрямую из GLFW после glfwPollEvents(),
// плюс небольшое накопленное состояние для колеса мыши (его в GLFW нельзя
// опросить). Window::bind() привязывает окно перед использованием.
class Input {
public:
    // Привязывает окно, из которого будет читаться ввод.
    static void bind(GLFWwindow* window);

    // Клавиатура: нажата ли клавиша сейчас (GLFW_KEY_*).
    static bool isKeyPressed(int key);
    static bool isKeyReleased(int key);

    // Мышь: нажата ли кнопка (GLFW_MOUSE_BUTTON_*) и позиция курсора.
    static bool isMouseButtonPressed(int button);

    // Позиция курсора в координатах окна (левый верхний угол — (0,0)).
    static void mousePosition(double& x, double& y);

    // Смещение курсора относительно предыдущего момента вызова mouseDelta*().
    static double mouseDeltaX();
    static double mouseDeltaY();

    // Накопленное смещение колеса прокрутки. Возвращает true, если прокрутка
    // была с момента прошлого вызова, и забирает накопленное значение.
    static bool consumeScroll(double& x, double& y);

    // Вызывается из колбэка прокрутки окна (см. Window::initCallbacks).
    static void onScroll(double xoffset, double yoffset);

    // Начало нового кадра: сбрасывает флаги «дельта уже снята в этом кадре».
private:
    // Статическое состояние (окно и накопленные значения).
    static inline GLFWwindow* window_ = nullptr;
    static inline double lastMouseX_ = 0.0;
    static inline double lastMouseY_ = 0.0;
    static inline double scrollX_ = 0.0;
    static inline double scrollY_ = 0.0;
};

// --- Реализация (inline, header-only) ---

inline void Input::bind(GLFWwindow* window) {
    window_ = window;
    // Инициализируем последнюю позицию курсора реальными координатами:
    // иначе первый вызов mouseDeltaX/Y() после старта (или первого клика)
    // выдаёт гигантский дельта-сдвиг от (0,0) и камера «дёргается».
    if (window != nullptr) {
        glfwGetCursorPos(window, &lastMouseX_, &lastMouseY_);
    }
}

inline bool Input::isKeyPressed(int key) {
    return glfwGetKey(window_, key) == GLFW_PRESS;
}

inline bool Input::isKeyReleased(int key) {
    return glfwGetKey(window_, key) == GLFW_RELEASE;
}

inline bool Input::isMouseButtonPressed(int button) {
    return glfwGetMouseButton(window_, button) == GLFW_PRESS;
}

inline void Input::mousePosition(double& x, double& y) {
    glfwGetCursorPos(window_, &x, &y);
}

inline double Input::mouseDeltaX() {
    double x = 0.0;
    double y = 0.0;
    mousePosition(x, y);
    const double delta = x - lastMouseX_;
    // Каждая ось обновляет ТОЛЬКО свою базу. Если здесь же писать lastMouseY_,
    // то следующий mouseDeltaY() посчитает y - lastMouseY_ = y - y = 0, и
    // вертикальный обзор умрёт навсегда: pitch нельзя было ни поднять, ни
    // опустить, камера «прилипала» к горизонту.
    lastMouseX_ = x;
    return delta;
}

inline double Input::mouseDeltaY() {
    double x = 0.0;
    double y = 0.0;
    mousePosition(x, y);
    const double delta = y - lastMouseY_;
    // Симметрично mouseDeltaX(): база по Y обновляется только здесь.
    lastMouseY_ = y;
    return delta;
}

inline bool Input::consumeScroll(double& x, double& y) {
    x = scrollX_;
    y = scrollY_;
    scrollX_ = 0.0;
    scrollY_ = 0.0;
    return x != 0.0 || y != 0.0;
}

inline void Input::onScroll(double xoffset, double yoffset) {
    scrollX_ += xoffset;
    scrollY_ += yoffset;
}

}  // namespace core