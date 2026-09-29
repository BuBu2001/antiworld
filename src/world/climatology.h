#pragma once

// Климатология мира: статические пространственные поля, посчитанные ОДИН раз
// на сетке и больше не меняющиеся.
//
// Зачем это отдельный класс, а не ещё пара формул в Climate: климат по
// определению зависит не от двух чисел, а от точки на карте. Прежний Climate
// знал только «температура падает с высотой» и «температура падает к западу»
// — то есть широтный градиент был подменён линейной зависимостью от X.
// Из-за этого не существовало ни материальности климата (Сибирь), ни осадков,
// ни снеговой линии, ни льда, ни уровня моря, кроме одной константы.
//
// Мир — квадрат extent × extent, который натягивается на сферу, поэтому Z это
// широта: latitude = asin(z / (extent/2)), от -90° (южный полюс) до +90°
// (северный). X — долгота, и по X мир ЦИКЛИЧЕН (период extent), а по Z
// зажат границами (полюса). Все запросы это учитывают: по X индексы
// сворачиваются, по Z clamp-ятся на крайние ячейки.
//
// Модель намеренно статическая: никаких движущихся циклонов, дрейфа и
// накопления ошибок. Всё, что здесь считается, — равновесие: климат, который
// получился бы за тысячи лет усреднения. Поэтому класс неизменяем после
// build(), его можно свободно читать из потоков чанков без синхронизации.
//
// Порядок вычисления полей важен и задаёт зависимости:
//   высоты -> суша/вода -> расстояние до океана -> температура -> осадки ->
//   влажность -> лёд -> уровень моря
// Материковость (расстояние до океана) — это то, что превращает «холодно на
// севере» в «холодно и сухо в глубине материка», то есть в Сибирь: зима
// суровая, лето жаркое, осадков мало.

#include <cstdint>
#include <memory>
#include <vector>

#include "world/terrain_generator.h"

namespace world {

class Climatology {
public:
    struct Config {
        // Разрешение сетки. 512×512 на 22 585 км даёт ячейку 44 км: этого
        // хватает и для материковости (Сибирь шириной в несколько тысяч
        // километров), и для снеговой линии, и для уровня моря. Ниже 128
        // материальность уже не читается, выше 1024 построение дороже, чем
        // сама генерация мира.
        std::uint32_t width{512};
        std::uint32_t height{512};
        // Сторона мира в метрах. Должна совпадать с extent у рельефа: по этому
        // числу считается период и по нему же замыкается мир по X.
        double extent{world::kWorldExtent};
        // Сколько РЕАЛЬНЫХ метров в одной вертикальной единице рельефа.
        //
        // У TerrainGenerator из ChunkManager amplitude = 900, oceanDepth = 2500,
        // maxLandHeight = 3500 при мире 22 585 км поперёк — то есть единицы
        // рельефа это МЕТРЫ, и вертикального преувеличения нет. Ставить 1.0
        // обязательно: иначе температура падает с высотой в 60 раз медленнее
        // (или в 60 раз быстрее), и либо вершины тропиков оказываются вечными
        // ледниками, либо снеговая линия в метрах (6000) оказывается выше любой
        // вершины (3500) и снег не лежит нигде.
        //
        // Число остаётся настраиваемым, а не «зашитым единицей», потому что
        // вертикальный масштаб рельефа — это ровно тот параметр, который при
        // смене amplitude придётся согласовать с климатом заново.
        double verticalMetersPerUnit{1.0};

        // --- Температура ---
        // Среднегодовая температура на уровне моря у экватора, ℃.
        double equatorTemperature{27.0};
        // ... у полюса, ℃.
        double polarTemperature{-25.0};
        // Выпуклость широтного градиента. 1 — линейно от экватора к полюсу,
        // больше 1 — средние широты холоднее линейных (так и в реальности:
        // Сибирь зимой около -30, а -20 пришлось бы на 45°).
        double latitudeExponent{1.7};
        // Падение температуры с высотой, ℃ на метр. 6.5 ℃/км — земной градиент.
        double lapseRate{0.0065};
        // Насколько тёплые широтные течения прогревают океан, ℃. Течение
        // гонит тепло от экватора к полюсам, поэтому поправка положительна у
        // экватора и уходит в ноль к 70°, дальше океан холоднее среднего.
        double currentWarmth{5.0};
        // Расстояние затухания морского влияния вглубь суши, м. На берегу
        // влияние полное, на 700 км inland — треть, на 2000 км — почти ноль.
        double currentDecay{700000.0};

        // --- Материальность ---
        // Расстояние, на котором материк считается «глубоким», м. Внутри
        // континента океан не слышен: зима суровая, осадков мало.
        double continentalRange{1200000.0};
        // Насколько континентальность снижает СРЕДНЕГОДОВУЮ температуру, ℃.
        // Немного: внутренность континента зимой намного холоднее, но летом
        // теплее, и год в среднем лишь чуть холоднее.
        double continentalCooling{4.0};
        // Насколько континентальность РАСТЯГИВАЕТ сезон, ℃ полуразмаха.
        // Вот это и даёт Сибирь: летом +20, зимой -35 при одной среднегодовой.
        double continentalSwing{12.0};

        // --- Осадки ---
        // Профиль ячеек Хедли и фронта полярного: максимум у экватора (восходящий
        // воздух), минимум в субтропиках (нисходящий), второй максимум в
        // умеренных (фронт полярного фронта), спад к полюсу. Нормировано на 1.
        double precipitationEquator{1.0};
        double precipitationSubtropical{0.15};
        double precipitationMidLat{0.8};
        double precipitationPolar{0.1};
        // Широты, на которых стоят перечисленные экстремумы, °.
        double subtropicalLatitude{25.0};
        double midLatitude{55.0};
        // Насколько осадки скудеют в глубине материка, 0..1. Влага не доходит:
        // именно поэтому внутри Сибири сухо, а на её наветренном берегу мокро.
        double continentalDryness{0.5};
        // Насколько орография усиливает осадки, на единицу подъёма в 1 км.
        double orographicGain{0.45};
        // На сколько метров назад по ветру смотрим, чтобы понять, в долине ли
        // точка. 350 км — примерно масштаб дождевого фронта.
        double rainShadowDistance{350000.0};
        // Осадки при precipitationNorm = 1, мм в год. Тропики дают до 3000,
        // поэтому это нормировка для показа, а не физика.
        double precipitationMillimeters{3000.0};

        // --- Влажность ---
        double humidityBase{0.12};
        double humidityPrecipitation{0.70};
        double humidityOcean{0.12};
        double humidityContinental{0.30};

        // --- Лёд ---
        // Среднегодовая температура, ниже которой начинается вечный лёд, ℃.
        // Выше — не начинается (температура в среднем за год не может
        // удерживать ледник).
        double iceTemperature{-10.0};
        // Ширина перехода по температуре, ℃: от полностью талого до полностью
        // ледяного.
        double iceTemperatureRange{10.0};
        // Осадки, ниже которых ледник не растёт даже в холоде: лёд нужен
        // снегопад, сухой холод даёт камень, а не глетчер.
        double icePrecipitationFloor{0.06};
        // Температура, ниже которой замерзает море, ℃, и ширина перехода.
        double seaIceTemperature{-1.5};
        double seaIceRange{5.0};
        // Снеговая линия: высота, выше которой снег лежит круглый год.
        double snowLineEquator{6000.0};
        double snowLinePrecipitation{1200.0};

        // --- Уровень моря ---
        // Тепловое расширение: на сколько метров поднимается океан на каждый
        // градус теплее опорной температуры, м/℃.
        double seaLevelPerDegree{0.45};
        double seaLevelReferenceTemperature{12.0};
        // Сколько метров уровня даёт ледниковый объём. Считается по факту:
        // чем больше льда на планете, тем ниже вода в океане.
        double seaLevelFromIce{14.0};
        // Ограничение амплитуды, м. Больше ±40 м на планете в 22 585 км —
        // уже не океан, а лужа; к тому же такой размах уже заметно гуляет
        // береговой линией, а не «чуть-чуть».
        double seaLevelLimit{40.0};
    };

    // Единственный способ создать: без рельефа полей не существует, все поля
    // выводятся из высот и маски материка.
    //
    // Два объявления вместо одного с аргументом по умолчанию: значение вида
    // Config{} в аргументе по умолчанию сослаться на ещё не завершённый
    // вложенный тип нельзя, компилятор требует, чтобы все NSDMI Config были
    // готовы до конца объемлющего класса (та же причина, по которой у WorldMap
    // два конструктора).
    static std::unique_ptr<Climatology> build(const TerrainGenerator::Config& terrain,
                                              std::uint32_t seed);
    static std::unique_ptr<Climatology> build(const TerrainGenerator::Config& terrain,
                                              std::uint32_t seed, Config config);

    ~Climatology() = default;
    // Сетки занимают единицы мегабайт, а экземпляр на мир ровно один.
    Climatology(const Climatology&) = delete;
    Climatology& operator=(const Climatology&) = delete;

    const Config& config() const noexcept { return config_; }

    // --- Запросы. Потокобезопасны: поля не меняются после build(). ---

    // Широта точки по Z, градусы: -90 на южном полюсе, +90 на северном.
    double latitudeAt(double wz) const noexcept;
    // Расстояние до ближайшего океана в метрах, 0 на береговой линии.
    double distanceToOceanAt(double wx, double wz) const;
    // Среднегодовая температура, ℃. По ней выбирается биом.
    double temperatureAt(double wx, double wz) const;
    // Полуразмах сезонных колебаний, ℃: лето = T + swing, зима = T - swing.
    // Континентальность увеличивает, море уменьшает.
    double seasonalSwingAt(double wx, double wz) const;
    // Осадки, нормированные 0..1 (1 — как в тропиках).
    double precipitationAt(double wx, double wz) const;
    // Осадки в мм в год, для подписей и HUD.
    double precipitationMillimetersAt(double wx, double wz) const;
    // Относительная влажность, 0..1. По ней выбирается биом.
    double humidityAt(double wx, double wz) const;
    // Уровень моря в точке, м относительно geography.seaLevel. Поле ненулевое:
    // тёплый океан стоит выше холодного, а ледники понижают уровень глобально.
    double seaLevelAt(double wx, double wz) const;
    // То же в МИРОВЫХ ЕДИНИЦАХ рельефа — в таком виде поле сравнивается с
    // высотой узла при отрисовке воды и при раскраске берега. Единая точка
    // пересчёта: иначе появятся два независимых перевода метров в единицы и
    // рано или поздно один из них забудут.
    double seaLevelWorldUnitsAt(double wx, double wz) const;
    // Доля вечного льда/снега на суше, 0..1.
    double iceAt(double wx, double wz) const;
    // Доля морского льда, 0..1.
    double seaIceAt(double wx, double wz) const;
    // Снеговая линия в точке, м над уровнем моря. Выше неё снег круглый год.
    double snowLineAt(double wx, double wz) const;
    // Снеговая линия в мировых единицах рельефа — для сравнения с высотой
    // узла ландшафта (см. seaLevelWorldUnitsAt про единый пересчёт).
    double snowLineWorldUnitsAt(double wx, double wz) const;
    // Материальность 0..1: 0 у берега, 1 в глубине континента.
    double continentalityAt(double wx, double wz) const;
    // Сезон сдвинут на полгода в южном полушарии: там зима в декабре.
    // Возвращает 1, если фаза года в этой точке перевёрнута.
    bool southernHemisphere(double wz) const noexcept;
    // Высота рельефа из сетки, м. Без неё запросы температуры и осадков всё
    // равно работают (рельеф просто не учитывается), но вызывать стоит
    // именно сетку: она в разы дешевле повторного sampleHeightAt.
    float heightAt(double wx, double wz) const;

    // --- Сводка по миру (для логов) ---
    double landFraction() const noexcept { return landFraction_; }
    double meanTemperature() const noexcept { return meanTemperature_; }
    double iceAreaFraction() const noexcept { return iceAreaFraction_; }
    double seaLevelMean() const noexcept { return seaLevelMean_; }
    double seaLevelGlobalOffset() const noexcept { return seaLevelGlobalOffset_; }
    // Разброс уровня моря, м: на столько поднимается самый тёплый океан
    // относительно самого холодного.
    double seaLevelRange() const noexcept { return seaLevelMax_ - seaLevelMin_; }

private:
    Climatology() = default;

    // Этапы построения, в порядке зависимостей: высоты -> расстояние до
    // океана -> температура -> осадки -> влажность -> лёд -> уровень моря.
    void computeHeights();
    void computeDistanceToOcean();
    void computeTemperature();
    void computePrecipitation();
    void computeHumidity();
    void computeIce();
    void computeSeaLevel();
    // Широта ячейки по её номеру — нужна внутри полей, где точки уже
    // раскладываются по индексам, а не по координатам.
    double latitudeAtCell(std::size_t index) const;

    // Билинейная выборка из любой сетки: по X индексы сворачиваются (мир
    // цикличен), по Z зажимаются на полюсах.
    double sample(const std::vector<float>& grid, double wx, double wz) const;
    // Высота по сетке без интерполяции — для расчётов при построении.
    float heightRaw(std::size_t index) const { return height_[index]; }
    std::size_t index(std::uint32_t i, std::uint32_t j) const {
        return static_cast<std::size_t>(j) * config_.width + i;
    }
    // Материальность из расстояния до океана.
    double continentalityFromDistance(double distance) const;
    // Тёплое ли море (тёплые течения) по широте, ℃ поправки.
    double currentAt(double latitudeDegrees) const;
    // Наветренная высота: смотрим в сторону, ОТКУДА дует ветер.
    double upwindHeight(double wx, double wz, double latitudeDegrees) const;
    // Ведущий знак ветра: в тропиках пассаты дуют с востока (+1 — наветренный
    // восток), в умеренных западные ветры дуют с запада (-1).
    static double windFromSign(double latitudeDegrees, double subtropicalLatitude);

    Config config_;
    std::uint32_t seed_{0};
    TerrainGenerator::Config terrain_;

    // Сетки. Индекс = j * width + i, i по X, j по Z. Ячейка (i, j) стоит в
    // точке wx = -half + extent*(i+0.5)/width — то есть сетка покрывает весь
    // мир, а не его «углы», как было бы при координатах в узлах сетки.
    std::vector<float> height_;         // м
    std::vector<float> distance_;       // м до океана
    std::vector<float> temperature_;    // ℃ среднегодовая
    std::vector<float> swing_;          // ℃ полуразмах сезона
    std::vector<float> precipitation_;  // 0..1
    std::vector<float> humidity_;       // 0..1
    std::vector<float> seaLevel_;       // м
    std::vector<float> ice_;            // 0..1
    std::vector<float> seaIce_;         // 0..1
    std::vector<float> snowLine_;       // м

    double landFraction_{0.0};
    double meanTemperature_{0.0};
    double iceAreaFraction_{0.0};
    double seaLevelMean_{0.0};
    double seaLevelMin_{0.0};
    double seaLevelMax_{0.0};
    double seaLevelGlobalOffset_{0.0};
};

}  // namespace world
