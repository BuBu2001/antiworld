#include "world/climate.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace world {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Фаза года, соответствующая середине лета — максимум сезонной температуры
// и минимум влажности. Год разбит на четыре сезона по 0.25, поэтому середина
// лета = (1 + 0.5) / 4. Все сезонные функции (температура, морозность,
// влажность) строятся как косинус от этой точки, иначе пики разъезжаются
// относительно границ сезонов.
constexpr float kMidSummer = 0.375f;

// Приведение фазы в [0, 1): нужно и setTime (прыжок), и update (переход через
// границу года/суток). Работает и для отрицательных значений.
float wrapPhase(float value) {
    value -= std::floor(value);
    return value;
}

}  // namespace

const char* seasonName(Season season) {
    switch (season) {
        case Season::Spring:
            return "весна";
        case Season::Summer:
            return "лето";
        case Season::Autumn:
            return "осень";
        case Season::Winter:
            return "зима";
    }
    return "неизвестно";
}

Climate::Climate() : Climate(Config{}) {}

Climate::Climate(Config config) : config_(config) {
    if (!std::isfinite(config_.yearLength) || config_.yearLength <= 0.0f) {
        throw std::invalid_argument("Climate: длительность года должна быть положительной");
    }
    if (!std::isfinite(config_.daysPerYear) || config_.daysPerYear <= 0.0f) {
        throw std::invalid_argument("Climate: число суток в году должно быть положительным");
    }
    yearPhase_ = wrapPhase(config_.startYearPhase);
    timeOfDay_ = wrapPhase(config_.startTimeOfDay);
}

// --- Календарь ---

void Climate::update(float deltaTime) {
    // Нечисловой или отрицательный шаг игнорируем: иначе один битый кадр
    // увёл бы время года назад (pause/resize/потеря фокуса).
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }

    // Сколько игровых лет прошло за шаг: для счётчика в HUD.
    elapsedYears_ += deltaTime / config_.yearLength;

    // Перенос через границу года: год — замкнутый цикл, поэтому фаза
    // перескакивает с 1 на 0, а счётчик полных лет это фиксирует.
    yearPhase_ = wrapPhase(yearPhase_ + deltaTime / config_.yearLength);
    timeOfDay_ = wrapPhase(timeOfDay_ + deltaTime / (config_.yearLength / config_.daysPerYear));
}

void Climate::setTime(float yearPhase, float timeOfDay) {
    if (!std::isfinite(yearPhase) || !std::isfinite(timeOfDay)) {
        throw std::invalid_argument("Climate: фаза года и время суток должны быть конечными");
    }
    yearPhase_ = wrapPhase(yearPhase);
    timeOfDay_ = wrapPhase(timeOfDay);
}

Season Climate::season() const noexcept {
    // Год разбит на четыре равные фазы, поэтому индекс фазы и есть сезон.
    const auto index = static_cast<std::size_t>(yearPhase_ * static_cast<float>(kSeasonCount));
    return static_cast<Season>(std::min(index, kSeasonCount - 1));
}

float Climate::seasonProgress() const noexcept {
    const float scaled = yearPhase_ * static_cast<float>(kSeasonCount);
    return scaled - std::floor(scaled);
}

// --- Температура и влажность ---

float Climate::seasonalOffset(float yearPhase) const noexcept {
    // Косинус с периодом в год от середины лета: +amplitude в середине лета,
    // 0 в середине весны и осени, -amplitude в середине зимы. Непрерывен вместе
    // с первой производной, поэтому смена сезонов не даёт скачка температуры.
    return config_.seasonalAmplitude *
           std::cos(kTwoPi * (wrapPhase(yearPhase) - kMidSummer));
}

float Climate::latitudeFactor(float x) const noexcept {
    if (worldSizeX_ <= 0.0f) {
        return 0.0f;
    }
    // Мир центрирован в нуле, поэтому нормируем на половину ширины карты.
    return std::clamp(x / (worldSizeX_ * 0.5f), -1.0f, 1.0f);
}

float Climate::latitudeOffset(float x, float /*z*/) const noexcept {
    // -latitudeSpread на западе (x = -sizeX/2) и +latitudeSpread на востоке:
    // карта устроена как континент с холодным западом и тёплым востоком.
    // Z не используется: карта шире, чем глубока, и градиент по X читается как
    // «запад -> восток».
    return config_.latitudeSpread * latitudeFactor(x);
}

float Climate::heightAt(float x, float z) const {
    return heightSampler_ ? heightSampler_(x, z) : 0.0f;
}

float Climate::diurnalOffset() const noexcept {
    // Максимум в полдень (0.5), минимум в полночь (0).
    return config_.diurnalAmplitude * std::cos(kTwoPi * (timeOfDay_ - 0.5f));
}

float Climate::temperature() const noexcept {
    return config_.baseTemperature + seasonalOffset(yearPhase_) + diurnalOffset();
}

float Climate::annualMeanTemperatureAt(float x, float z) const {
    // Среднегодовая = база + широта − высотное охлаждение. Сезонного и
    // суточного слагаемых нет намеренно: по этой величине выбирается биом,
    // и он не должен меняться вместе со временем года.
    return config_.baseTemperature + latitudeOffset(x, z) -
           config_.lapseRate * heightAt(x, z);
}

float Climate::temperatureAt(float x, float z) const {
    return annualMeanTemperatureAt(x, z) + seasonalOffset(yearPhase_) + diurnalOffset();
}

float Climate::temperatureAt(float x, float z, Season season) const {
    // Каноническая точка сезона — его середина, поэтому результат не зависит
    // от того, на каком именно моменте сезона мы спрашиваем.
    const float phase = (static_cast<float>(static_cast<std::size_t>(season)) + 0.5f) /
                        static_cast<float>(kSeasonCount);
    return annualMeanTemperatureAt(x, z) + seasonalOffset(phase) + diurnalOffset();
}

float Climate::humidity() const noexcept {
    // Влажность по тому же косинусу, что и температура, но без обратного знака:
    // суше всего в середине лета, влажнее всего в середине зимы.
    return std::clamp(config_.baseHumidity +
                          config_.humiditySeasonal * std::cos(kTwoPi * (yearPhase_ - kMidSummer)),
                      0.0f, 1.0f);
}

float Climate::annualMeanHumidityAt(float x, float z) const {
    // Влажность зависит от положения (суше на востоке — там жарко и сухо) и от
    // высоты (на вершинах суше). Сезонной составляющей нет намеренно: биом
    // выбирается по среднегодовым величинам, иначе лес летом превращался бы в
    // пустыню, а зимой наоборот.
    const float altitude = std::max(0.0f, heightAt(x, z));
    return std::clamp(config_.baseHumidity -
                          config_.humidityLatitudeSpread * latitudeFactor(x) -
                          config_.humidityAltitudeRate * altitude,
                      0.0f, 1.0f);
}

float Climate::humidityAt(float x, float z) const {
    // Та же пространственная картина плюс сезонная волна: летом суше, зимой
    // влажнее. Основа берётся у среднегодовой функции, чтобы пространственная
    // часть не дублировалась.
    const float annualMean = annualMeanHumidityAt(x, z);
    return std::clamp(annualMean +
                          (humidity() - std::clamp(config_.baseHumidity, 0.0f, 1.0f)),
                      0.0f, 1.0f);
}

float Climate::frost() const noexcept {
    // 0 в середине лета, 1 в середине зимы. Степень 1.5 разводит «тёплый» и
    // «холодный» сезоны: в середине весны и осени морозность заметно ниже, чем
    // в разгар зимы, и снег тает быстрее, чем падает.
    const float raw = 0.5f * (1.0f - std::cos(kTwoPi * (yearPhase_ - kMidSummer)));
    return std::pow(std::clamp(raw, 0.0f, 1.0f), 1.5f);
}

// --- Связь с ландшафтом ---

void Climate::setHeightSampler(std::function<float(float, float)> sampler) {
    heightSampler_ = std::move(sampler);
}

void Climate::setWorldSize(float sizeX, float sizeZ) {
    if (!std::isfinite(sizeX) || !std::isfinite(sizeZ)) {
        throw std::invalid_argument("Climate: размеры мира должны быть конечными");
    }
    worldSizeX_ = std::max(0.0f, sizeX);
    worldSizeZ_ = std::max(0.0f, sizeZ);
}

// --- Освещение ---

glm::vec3 Climate::sunDirection() const noexcept {
    // Солнце восходит на востоке (+X) в 0.25, в зените в 0.5, садится на
    // западе (-X) в 0.75 и ночью уходит под горизонт. Небольшой наклон по Z
    // задаёт направление на юг, чтобы тени не падали строго вдоль осей.
    const float angle = kTwoPi * (timeOfDay_ - 0.25f);
    return glm::normalize(glm::vec3{std::cos(angle), std::sin(angle), 0.35f});
}

float Climate::daylight() const noexcept {
    // Солнце над горизонтом — день, под горизонтом — ночь. Множитель 3.0
    // вместо 1.0 даёт не резкий выключатель, а плавные сумерки.
    return std::clamp(sunDirection().y * 3.0f, 0.0f, 1.0f);
}

float Climate::sunIntensity() const noexcept {
    // Яркость падает вместе с высотой солнца и зимой (солнце ниже и тусклее).
    const float seasonal = 0.8f + 0.35f * (1.0f - frost());
    return daylight() * seasonal;
}

float Climate::ambient() const noexcept {
    // Ambient никогда не ноль: ночью и в тени должно что-то читаться.
    return 0.10f + 0.22f * daylight();
}

}  // namespace world
