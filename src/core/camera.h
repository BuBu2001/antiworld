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
    // Позиция принимается в ГЛОБАЛЬНЫХ координатах (double): на мире
    // 22 585 км float32 имеет шаг ~2 м, и камера «заикала» бы на каждом шаге.
    void init(const glm::dvec3& globalPosition, const glm::dvec3& globalTarget);

    // Уже в локальных координатах (double) — удобно для небольших миров.
    void init(const glm::vec3& position, const glm::vec3& target);

    // Обновление каждый кадр: движение (WASD+Q/E), обзор (мышь), скорость
    // (колесо). dt — время кадра (сек), aspect — ширина/высота окна.
    void update(float dt, float aspect);

    // Матрицы для UBO (Vulkan-ready: перевёрнутая Y-проекция, Z в [0,1]).
    const glm::mat4& view() const { return view_; }
    const glm::mat4& projection() const { return projection_; }
    // Единичная модель — объект (треугольник) статичен в центре сцены.
    static glm::mat4 model() { return glm::mat4(1.0f); }

    // Позиция глаза в ГЛОБАЛЬНЫХ координатах (double) — источник истины.
    // Именно её получает ChunkManager для расчёта нужных чанков.
    const glm::dvec3& globalPosition() const { return globalPosition_; }

    // Позиция глаза в ЛОКАЛЬНЫХ координатах (float, относительно origin) —
    // та, что реально уходит в view-матрицу и в шейдеры. Всегда маленькая,
    // поэтому float32 достаточен.
    const glm::vec3& position() const { return position_; }

    // Смена плавающего начала координат. Вызывается каждый кадр (или при
    // телепорте игрока): пересчитывает локальную позицию камеры из глобальной.
    // ВАЖНО: вызывать ДО update() либо сразу после update() в том же кадре,
    // когда origin сдвинулся, — иначе один кадр рисуется со сдвинутой сценой
    // и несдвинутой камерой (видимый «прыжок» на величину сдвига).
    void setOrigin(const glm::dvec3& origin);

    // Настройки (меняются между кадрами).
    void setSpeed(float metersPerSecond) { speed_ = metersPerSecond; }
    void setSensitivity(float radiansPerPixel) { sensitivity_ = radiansPerPixel; }
    void setFovDegrees(float fovDegrees) { fov_ = glm::radians(fovDegrees); }

    // Текущие углы и направление взгляда. Нужны для телеметрии и отладки:
    // без них нельзя отличить «камеру снесло вводом» от «камера едет сама».
    float yaw() const noexcept { return yaw_; }
    float pitch() const noexcept { return pitch_; }
    const glm::vec3& front() const noexcept { return front_; }
    const glm::vec3& right() const noexcept { return right_; }
    float speed() const noexcept { return speed_; }

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
    //
    // Разделение намеренное:
    //   globalPosition_ (double) — ЛОГИЧЕСКАЯ позиция в координатах мира.
    //       Только она переживает дистанции в тысячи километров: на стороне
    //       мира 22 585 км float32 имеет ULP ~2 м, и камера накапливала бы
    //       ошибку до метра на каждом шаге движения.
    //   position_ (float) — ЛОКАЛЬНАЯ позиция относительно origin_, из неё
    //       строится view-матрица. Всегда рядом с нулём, поэтому точная.
    //   origin_ (double) — текущее плавающее начало координат.
    glm::dvec3 globalPosition_{0.0, 0.0, 3.0};
    glm::dvec3 origin_{0.0, 0.0, 0.0};
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

inline void Camera::init(const glm::dvec3& globalPosition, const glm::dvec3& globalTarget) {
    globalPosition_ = globalPosition;
    position_ = glm::vec3(globalPosition - origin_);

    const glm::dvec3 dir = globalTarget - globalPosition;
    const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    front_ = (len > 0.0) ? glm::normalize(glm::vec3(static_cast<float>(dir.x / len),
                                                     static_cast<float>(dir.y / len),
                                                     static_cast<float>(dir.z / len)))
                         : glm::vec3(0.0f, 0.0f, -1.0f);

    // Восстанавливаем стартовые углы из направления взгляда, чтобы камера
    // сразу смотрела на цель, а не вдоль +Z по умолчанию.
    yaw_ = std::atan2f(front_.z, front_.x);
    pitch_ = std::asinf(std::clamp(front_.y, -1.0f, 1.0f));
    recomputeView();
}

inline void Camera::init(const glm::vec3& position, const glm::vec3& target) {
    init(glm::dvec3(position), glm::dvec3(target));
}

inline void Camera::setOrigin(const glm::dvec3& origin) {
    origin_ = origin;
    // Локальная позиция = глобальная минус origin. Именно она уходит в
    // view-матрицу; разность считается в double и только потом сужается до
    // float, поэтому не теряет точность даже на краю мира.
    const glm::dvec3 local = globalPosition_ - origin_;
    position_ = glm::vec3(static_cast<float>(local.x), static_cast<float>(local.y),
                          static_cast<float>(local.z));
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
    // Накопление позиции — в double. При скорости 40 м/с и dt ~1/60 с шаг
    // равен 0.67 м, и в double он не теряется после 22 585 км пути.
    const double vx = static_cast<double>(move.x) * velocity;
    const double vy = static_cast<double>(move.y) * velocity;
    const double vz = static_cast<double>(move.z) * velocity;
    globalPosition_ += glm::dvec3(vx, vy, vz);
    position_ = glm::vec3(static_cast<float>(globalPosition_.x - origin_.x),
                          static_cast<float>(globalPosition_.y - origin_.y),
                          static_cast<float>(globalPosition_.z - origin_.z));

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
