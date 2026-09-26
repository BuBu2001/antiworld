# AntiWorld

3D-симулятор развития племён на C++20 без игрового движка.

## Структура проекта

```
CMakeLists.txt          # корневой: C++20 + find_package(...)
src/
  main.cpp              # точка входа
  core/                 # база: логгер и общая инфраструктура
  renderer/             # рендеринг (Vulkan + GLFW + шейдеры)
  ecs/                  # ECS (EnTT)
  physics/              # физика (Jolt Physics)
  world/                # мир, террейн, генерация
  ai/                   # ИИ племён
shaders/                # GLSL/Vulkan-шейдеры
assets/                 # текстуры, модели, данные
include/                # публичные/сторонние заголовки (если понадобятся)
```

Модули `renderer`, `ecs`, `physics`, `world`, `ai` собираются как отдельные
статические библиотеки со своими `CMakeLists.txt` и подключаются к
исполняемому файлу `antiworld`.

## Быстрая установка

```bash
./install.sh
```

Скрипт поставит всё нужное и соберёт проект. Он идемпотентен — повторный запуск
ничего не переустанавливает, а за секунду просто пересобирает проект.

Что он делает:

1. системные пакеты через `pacman`: `base-devel cmake git vulkan-headers glfw glm`;
2. `EnTT` v3.15.0 и `Jolt` v5.6.0 собирает из исходников в `~/.local`
   (версии зафиксированы в скрипте, чтобы сборка была воспроизводимой);
3. конфигурирует проект с `-DCMAKE_PREFIX_PATH=~/.local`;
4. собирает его и проверяет, что бинарник на месте.

Зависимости кладутся именно в `~/.local`, а не в `/tmp`: `/tmp` очищается, и
после перезагрузки проект перестал бы собираться.

Ключи: `--prefix DIR`, `--build-dir DIR`, `--jobs N`, `--debug`, `--no-deps`,
`--no-build`, `--force`, `--help`. Полный список — в `./install.sh --help`.

```bash
./install.sh --no-deps      # только пересобрать проект, зависимости уже стоят
./install.sh --debug        # отладочная сборка
./install.sh --prefix /opt/local   # зависимости в общий префикс
```

Если сборка упала, логи остаются во временном каталоге, путь к нему скрипт
печатает в stderr.

## Установка вручную

<details>
<summary>Если скрипт не подходит (ручная установка)</summary>

Системные пакеты (Arch Linux):

```bash
sudo pacman -S --needed base-devel cmake vulkan-headers glfw glm
```

`glm` и `glfw` поставляют CMake-конфиги (`glm::glm`, target `glfw`),
`vulkan-headers` — `find_package(Vulkan)`.

### EnTT (header-only)

Через AUR:

```bash
yay -S entt
```

или из исходников (нужен `-DENTT_INSTALL=ON`, иначе CMake-конфиг не установится):

```bash
git clone https://github.com/skypjack/entt.git
cmake -S entt -B entt/build -DCMAKE_BUILD_TYPE=Release -DENTT_INSTALL=ON
cmake --build entt/build -j"$(nproc)"
sudo cmake --install entt/build
```

### Jolt Physics (из исходников, в репозиториях/AUR отсутствует)

```bash
git clone https://github.com/jrouwe/JoltPhysics.git
cmake -S JoltPhysics/Build -B Jolt/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DJPH_USE_VK=OFF \
    -DTARGET_SAMPLES=OFF -DTARGET_VIEWER=OFF \
    -DTARGET_PERFORMANCE_TEST=OFF -DTARGET_UNIT_TESTS=OFF -DTARGET_HELLO_WORLD=OFF
cmake --build Jolt/build -j"$(nproc)"
sudo cmake --install Jolt/build
```

Пояснения:

- `-DJPH_USE_VK=OFF` — отключает Vulkan-реализацию вычислительных шейдеров
  внутри Jolt (нам она не нужна, рендером занимается `src/renderer`) и заодно
  убирает требование компилятора DXC.
- Jolt устанавливает CMake-конфиг как `JoltConfig.cmake`, поэтому в CMake
  используется `find_package(Jolt CONFIG REQUIRED)` (в старых релизах имя
  было `JoltPhysics`). Целевая библиотека — `Jolt::Jolt`.
- Если ставите в нестандартный префикс (`~/local`, `/tmp/...`), укажите его:
  `-DCMAKE_PREFIX_PATH="$HOME/local"` при конфигурации проекта.

</details>

## Сборка вручную

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Запуск

```bash
./build/src/antiworld
```

При старте печатает в лог параметры ландшафта: диапазон высот, сезон, среднюю
температуру, высотные пороги гор и долю каждого биома. Пути к `assets/` и
шейдерам зашиты на этапе сборки, поэтому запускать можно из любого каталога.

## Мир: биомы и климат

### `src/world/biome.h` — биомы

Четыре биома: `Desert`, `Forest`, `Tundra`, `Mountains`.

- `getBiome(height, temperature, humidity, params)` — доминирующий биом
  точки, если нужен ответ «один из четырёх».
- `sampleBiome(...)` — то же самое, но с плавными весами всех биомов
  (`BiomeBlend`), смешанным цветом и `snowBias`. Именно его использует рендер,
  поэтому границы биомов не видно как ступеньку.
- `fitBiomeHeights(params, minHeight, maxHeight)` — переносит пороги высот
  (16..24 ед. по умолчанию) в реальный диапазон сгенерированной карты по
  доле высоты. Без этого высота шумов редко достигает абсолютных значений и
  горы не появляются. `Terrain` вызывает это после генерации.
- `biomeColor()` — базовый цвет: песок, зелень, тундровая охра, камень.

Все переходы идут через `smoothstep`, поэтому границы плавные и на любом
сиде карта содержит все четыре биома.

### `src/world/climate.h` — климат

`Climate` — время суток и сезон, температура, влажность, солнце.

- `update(dt)` двигает время; игровой год = 600 секунд (2.5 минуты на
  сезон, 4 «дня» в году). Непереданный/отрицательный/не-конечный `dt`
  игнорируется.
- Сезоны: весна, лето, осень, зима; фаза года `[0,1)` делится на четыре
  части, максимум температуры — в середине лета (фаза `0.375`), максимум
  `frost` — в середине зимы.
- `temperatureAt(x, z)` — текущая температура в точке, `temperatureAt(x, z,
  season)` — для конкретного сезона. `annualMeanTemperatureAt(x, z)` —
  среднегодовая, по ней биомы классифицируются, иначе границы «дышали» бы
  вместе с сезоном.
- Температура падает с высотой (`lapseRate`), растёт к востоку и падает к
  западу; влажность, наоборот, выше на западе и падает с высотой.
- `Climate::Config` — все числа настраиваются в одном месте: длительность
  года, амплитуды сезона и суток, широтный перепад, базовая температура и
  влажность.
- `setHeightSampler()` / `setWorldSize()` связывают климат с картой высот и
  её габаритами. Владелец обязан обнулить сэмплер перед своим удалением
  (`Terrain` делает это в деструкторе).

### Рендер: цвет биома и снег

Цвет и «снежность» передаются на вершины, а не рисуются текстурой:

- `renderer::Vertex` получил `color[3]` и `snowBias` (вершина выросла с
  32 до 48 байт, offsets 32 и 44).
- `TerrainGenerator::createMesh(heightmap, painter)` красит вершины во время
  построения меша; `Terrain` передаёт свой колбэк, который берёт
  `sampleBiome()` и пишет смешанный цвет и `snowBias`.
- `shaders/triangle.frag` считает снег как
  `smoothstep(0, 0.25, frost + snowBias - 1)`, где `frost` приходит из
  `Climate`. На вертикальных скалах покрытие уменьшается через `normal.y`,
  чтобы снег не «облекал» утёсы.
- Эффект: летом снега нет нигде, зимой тундра и вершины гор белые, в лесу
  снег примерно наполовину, пустыня не снежит никогда.
- `FrameEnvironment` (`sunDirection`, `sunIntensity`, `ambient`, `frost`)
  идёт в uniform buffer и заполняется в `VulkanBase::drawFrame()`;
  `ecs::World` хранит его и передаёт в каждый кадр.
