#pragma once

// GLFW_INCLUDE_VULKAN подключает Vulkan API после GLFW (методы Camera читают
// ввод через core::Input, которому нужен GLFW-контекст).
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/input.h"
#include "core/window.h"

namespace core {

// FPS-камера от первого лица: движение WASD (плюс Q/E вверх/вниз), обзор —
// мышью (зажатая ЛКМ или скрытый курсор), скорость — колесо прокрутки.
//
// Каждый кадр вызывается update(dt, aspect): читает ввод из core::Input,
// пересчитывает view-проекцию с учётом Vulkan (ось Y вниз, NDC Z в [0,1]).
// Матрицы доступны через view()/projection() и уходят в UBO (см. renderer).
//
// Header-only (inline), как и core::Input. Копирование запрещено — камера
// привязана к одному окну и владеет состоянием.
class Camera {
public:
    Camera() = default;
    ~Camera() = default;

    // Копировать камеру нельзя (привязана к окну, хранит накопленное состояние).
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Стартовая позиция и точка, на которую смотрим.
    void init(const glm::vec3& position, const glm::vec3& target);

    // Обновление каждый кадр: движение (WASD+Q/E), обзор (мышь), скорость
    // (колесо). dt — время кадра (сек), aspect — ширина/высота окна.
    void update(float dt, float aspect);

    // Матрицы для UBO (Vulkan-ready: перевёрнутая Y-проекция, Z в [0,1]).
    const glm::mat4& view() const { return view_; }
    const glm::mat4& projection() const { return projection_; }
    // Единичная модель — объект (треугольник) статичен в центре сцены.
    static glm::mat4 model() { return glm::mat4(1.0f); }
    // Позиция глаза камеры в мировых координатах.
    const glm::vec3& position() const { return position_; }

    // Настройки (меняются между кадрами).
    void setSpeed(float metersPerSecond) { speed_ = metersPerSecond; }
    void setSensitivity(float radiansPerPixel) { sensitivity_ = radiansPerPixel; }
    void setFovDegrees(float fovDegrees) { fov_ = glm::radians(fovDegrees); }

    // Ближняя и дальняя плоскости отсечения. Дальняя по умолчанию (100 ед.)
    // годится для небольших сцен, но ландшафт 255x255 ед. в неё не влезает —
    // тогда её нужно увеличить, иначе дальний край ландшафта срежется.
    // Ближняя плоскость влияет на точность глубины: чем дальше диапазон,
    // тем хуже распределение глубин, поэтому near стоит подбирать под сцену.
    void setClipPlanes(float nearPlane, float farPlane);

    float nearPlane() const { return nearPlane_; }
    float farPlane() const { return farPlane_; }

private:
    // Пересчитывает view-матрицу из позиции/углов поворота камеры.
    void recomputeView();

    // Позиция камеры и направление взгляда в мировых координатах.
    glm::vec3 position_{0.0f, 0.0f, 3.0f};
    glm::vec3 front_{0.0f, 0.0f, -1.0f};
    glm::vec3 up_{0.0f, 1.0f, 0.0f};
    glm::vec3 right_{1.0f, 0.0f, 0.0f};

    // Углы ориентации (радианы): yaw — вокруг вертикальной оси (влево/вправо),
    // pitch — вокруг горизонтальной (вверх/вниз, ограничен, чтобы не перевернуться).
    float yaw_{glm::radians(-90.0f)};
    float pitch_{0.0f};

    // Чувствительность мыши (рад/пиксель), скорость движения (м/с),
    // поле зрения (радианы) — настраиваются через set*().
    float sensitivity_{0.002f};
    float speed_{5.0f};
    float fov_{glm::radians(60.0f)};

    // Плоскости отсечения (см. setClipPlanes).
    float nearPlane_{0.1f};
    float farPlane_{100.0f};

    // Матрицы: view (из позиции+углов) и projection (перспективная для Vulkan).
    glm::mat4 view_{1.0f};
    glm::mat4 projection_{1.0f};
};

// --- Реализация (inline, header-only) ---

inline void Camera::init(const glm::vec3& position, const glm::vec3& target) {
    position_ = position;
    front_ = glm::normalize(target - position);

    // Восстанавливаем стартовые углы из направления взгляда, чтобы камера
    // сразу смотрела на цель, а не вдоль +Z по умолчанию.
    yaw_ = std::atan2f(front_.z, front_.x);
    pitch_ = std::asinf(front_.y);
    recomputeView();
}

inline void Camera::update(float dt, float aspect) {
    // === Обзор мышью: зажатая ЛКМ вращает камеру по смещению курсора ===
    //
    // Дельту курсора ОБЯЗАТЕЛЬНО снимаем каждый кадр, независимо от состояния
    // кнопки. Input::mouseDeltaX/Y() не просто читает смещение — они ещё и
    // обновляют базовую позицию lastMouseX_/lastMouseY_. Если снимать дельту
    // только при зажатой ЛКМ, то пока кнопка отпущена, база устаревает, и
    // при следующем нажатии накопившееся за это время смещение (сотни пикселей)
    // прилетает ОДНИМ рывком: камера прыгает/прыгает на исходный обзор.
    const double dX = Input::mouseDeltaX();
    const double dY = Input::mouseDeltaY();

    if (Input::isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
        yaw_ += static_cast<float>(dX) * sensitivity_;
        pitch_ -= static_cast<float>(dY) * sensitivity_;  // Y вверх — pitch вниз

        // Ограничиваем pitch, чтобы камера не перевернулась.
        const float maxPitch = glm::radians(89.0f);
        pitch_ = std::clamp(pitch_, -maxPitch, maxPitch);
    }

    // === Колесо прокрутки: меняет скорость движения (накопленное в Input) ===
    double scrollX = 0.0;
    double scrollY = 0.0;
    if (Input::consumeScroll(scrollX, scrollY)) {
        // Вверх — быстрее, вниз — медленнее; скорость держим в разумных пределах.
        const float factor = (scrollY > 0.0) ? 1.2f : 0.8f;
        speed_ = std::clamp(speed_ * factor, 0.5f, 50.0f);
    }

    // === Движение: WASD (горизонталь), Q/E (вверх/вниз) ===
    const float velocity = speed_ * dt;
    glm::vec3 move(0.0f);
    if (Input::isKeyPressed(GLFW_KEY_W)) move += front_;
    if (Input::isKeyPressed(GLFW_KEY_S)) move -= front_;
    if (Input::isKeyPressed(GLFW_KEY_D)) move += right_;
    if (Input::isKeyPressed(GLFW_KEY_A)) move -= right_;
    if (Input::isKeyPressed(GLFW_KEY_Q)) move -= up_;
    if (Input::isKeyPressed(GLFW_KEY_E)) move += up_;

    if (move != glm::vec3(0.0f)) {
        move = glm::normalize(move);
    }
    position_ += move * velocity;

    // === Матрицы ===
    recomputeView();

    // Проекция: перспектива FOV с near/far. Для Vulkan ось NDC Y направлена
    // вниз, а glm::perspective даёт Y вверх (конвенция OpenGL), поэтому
    // зеркалим Y в clip space (стандартный приём Vulkan) — так же, как
    // это делается для камеры в официальных туториалах.
    const glm::mat4 flipY = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
    projection_ = flipY * glm::perspectiveRH_ZO(fov_, aspect, nearPlane_, farPlane_);
}

inline void Camera::setClipPlanes(float nearPlane, float farPlane) {
    if (!std::isfinite(nearPlane) || !std::isfinite(farPlane)) {
        throw std::invalid_argument("Camera: плоскости отсечения должны быть конечными");
    }
    if (nearPlane <= 0.0f || farPlane <= nearPlane) {
        throw std::invalid_argument(
            "Camera: требуется 0 < near < far, получено " + std::to_string(nearPlane) +
            " и " + std::to_string(farPlane));
    }

    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
}

inline void Camera::recomputeView() {
    // Направление взгляда из углов: yaw — вокруг вертикали (ось X мира),
    // pitch — вокруг горизонтали (ось Z). Классическая FPS-формула.
    front_ = glm::normalize(glm::vec3(
        std::cos(pitch_) * std::cos(yaw_),
        std::sin(pitch_),
        std::cos(pitch_) * std::sin(yaw_)));

    right_ = glm::normalize(glm::cross(front_, glm::vec3(0.0f, 1.0f, 0.0f)));
    up_ = glm::normalize(glm::cross(right_, front_));

    view_ = glm::lookAt(position_, position_ + front_, up_);
}

}  // namespace core
