#!/usr/bin/env bash
#
# Установка зависимостей и сборка AntiWorld.
#
# Что делает:
#   1. системные пакеты через pacman (base-devel, cmake, vulkan-headers, glfw, glm);
#   2. EnTT и Jolt Physics собирает из исходников в постоянный префикс
#      (по умолчанию ~/.local) — в отличие от /tmp, он переживает перезагрузку;
#   3. конфигурирует и собирает проект, подставляя этот префикс в CMAKE_PREFIX_PATH;
#   4. проверяет, что бинарник собрался.
#
# Скрипт идемпотентен: уже установленные зависимости пропускаются, повторный
# запуск ничего не ломает. Ключи смотрите в ./install.sh --help

set -euo pipefail

# Версии зафиксированы, чтобы сборка была воспроизводимой.
readonly ENTT_TAG="v3.15.0"
readonly JOLT_TAG="v5.6.0"
readonly ENTT_URL="https://github.com/skypjack/entt.git"
readonly JOLT_URL="https://github.com/jrouwe/JoltPhysics.git"

PREFIX="$HOME/.local"
BUILD_DIR="build"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_DEPS=1
DO_BUILD=1
FORCE=0

# Каталог с исходниками и сборочными каталогами зависимостей. В mktemp, а не в
# /tmp/opencode: после установки отсюда ничего не нужно, а мусор в /tmp не должен
# переживать установку.
WORK_DIR=""
# Каталог с исходниками и сборочными каталогами зависимостей. В mktemp, а не в
# /tmp/opencode: после установки отсюда ничего не нужно, а мусор в /tmp не должен
# переживать установку.
cleanup() {
    local rc=$?
    if [ -z "$WORK_DIR" ] || [ ! -d "$WORK_DIR" ]; then
        return
    fi
    # При неудаче логи удалять нельзя: die() как раз печатает путь к логу.
    if [ "$rc" -ne 0 ] && compgen -G "$WORK_DIR/*.log" >/dev/null 2>&1; then
        printf 'логи сохранены в %s\n' "$WORK_DIR" >&2
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_RED=$'\033[31m'; C_OFF=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_OFF=""
fi

say()  { printf '%s==>%s %s\n' "$C_BOLD$C_GREEN" "$C_OFF" "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '%s[внимание]%s %s\n' "$C_YELLOW" "$C_OFF" "$*" >&2; }
die()  { printf '%s[ошибка]%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Установка зависимостей и сборка AntiWorld.

Использование:
    ./install.sh [ключи]

Ключи:
    --prefix DIR   куда ставить EnTT и Jolt (по умолчанию ~/.local)
    --build-dir D  каталог сборки проекта (по умолчанию build)
    --jobs N       параллельных компиляций (по умолчанию nproc)
    --debug        собрать проект с -DCMAKE_BUILD_TYPE=Debug
    --release      собрать проект с -DCMAKE_BUILD_TYPE=Release (по умолчанию)
    --no-deps      не ставить зависимости, только собрать проект
    --no-build     поставить зависимости, но не собирать проект
    --force        переустановить зависимости, даже если они уже есть
    -h, --help     эта справка

Примеры:
    ./install.sh                      # всё по умолчанию
    ./install.sh --prefix /opt/local  # зависимости в общий префикс
    ./install.sh --no-deps           # пересобрать проект, зависимости уже есть
    ./install.sh --debug --jobs 8    # отладочная сборка
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)     [ $# -ge 2 ] || die "--prefix требует аргумент";     PREFIX="$2";     shift 2 ;;
        --build-dir)  [ $# -ge 2 ] || die "--build-dir требует аргумент";  BUILD_DIR="$2";  shift 2 ;;
        --jobs|-j)    [ $# -ge 2 ] || die "--jobs требует аргумент";      JOBS="$2";      shift 2 ;;
        --debug)      BUILD_TYPE="Debug"; shift ;;
        --release)    BUILD_TYPE="Release"; shift ;;
        --no-deps)    DO_DEPS=0; shift ;;
        --no-build)   DO_BUILD=0; shift ;;
        --force)      FORCE=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "неизвестный ключ: $1 (см. --help)" ;;
    esac
done

readonly PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ENTT_CONFIG="$PREFIX/lib/EnTT/cmake/EnTTConfig.cmake"
readonly JOLT_CONFIG="$PREFIX/lib/cmake/Jolt/JoltConfig.cmake"

# Каталог сборки: относительный путь считаем от каталога проекта, абсолютный
# берём как есть. Без этой проверки `--build-dir /tmp/foo` склеился бы с
# PROJECT_DIR в нечто вроде /home/user/proj//tmp/foo — cmake послушно создал бы
# такую вложенную директорию внутри проекта.
case "$BUILD_DIR" in
    /*) readonly BUILD_PATH="$BUILD_DIR" ;;
    *)  readonly BUILD_PATH="$PROJECT_DIR/$BUILD_DIR" ;;
esac

# --- Проверки окружения -------------------------------------------------------

command -v git >/dev/null 2>&1 || die "не найден git"
command -v cmake >/dev/null 2>&1 || die "не найден cmake"

say "AntiWorld: проверка окружения"
info "каталог проекта:  $PROJECT_DIR"
info "префикс:         $PREFIX"
info "тип сборки:      $BUILD_TYPE"
info "параллелизм:     -j$JOBS"
info "cmake:           $(cmake --version | head -1)"
info "компилятор:      $(${CXX:-c++} --version | head -1)"

# --- Системные пакеты --------------------------------------------------------

install_system_packages() {
    if ! command -v pacman >/dev/null 2>&1; then
        die "скрипт рассчитан на Arch Linux (нужен pacman).
       Поставьте вручную: base-devel cmake vulkan-headers glfw glm,
       затем запустите ./install.sh --no-deps"
    fi

    say "Системные пакеты"
    # -noconfirm, чтобы не зависнуть на вопросах; --needed, чтобы не трогать
    # уже установленное. Пакеты base-devel и git нужны для компиляции.
    local pkgs=(base-devel cmake git vulkan-headers glfw glm)
    local missing=()
    local pkg
    for pkg in "${pkgs[@]}"; do
        pacman -Q "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done

    if [ ${#missing[@]} -eq 0 ]; then
        info "все системные пакеты уже установлены"
        return
    fi

    info "устанавливаю: ${missing[*]}"
    if [ "$(id -u)" -eq 0 ]; then
        pacman -S --needed --noconfirm "${missing[@]}"
    else
        sudo pacman -S --needed --noconfirm "${missing[@]}"
    fi
}

# --- EnTT --------------------------------------------------------------------

install_entt() {
    local config="$PREFIX/lib/EnTT/cmake/EnTTConfig.cmake"
    if [ -f "$config" ] && [ "$FORCE" -eq 0 ]; then
        say "EnTT: уже установлен ($ENTT_TAG)"
        return
    fi

    say "EnTT $ENTT_TAG"
    local src="$WORK_DIR/entt"
    info "клонирую $ENTT_URL"
    # -c advice.detachedHead=false убирает простыню советов git: клонируем по
    # тегу, поэтому HEAD всегда в detached-состоянии, и это ожидаемо.
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$ENTT_TAG" "$ENTT_URL" "$src"

    # ENT_INSTALL=ON обязателен: без него CMake-конфиг не ставится, и
    # find_package(EnTT) в проекте не найдёт пакет.
    info "собираю"
    cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENTT_INSTALL=ON \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        >"$WORK_DIR/entt-configure.log" 2>&1 \
        || { warn "лог: $WORK_DIR/entt-configure.log"; die "не удалось сконфигурировать EnTT"; }
    cmake --build "$src/build" --parallel "$JOBS" \
        >"$WORK_DIR/entt-build.log" 2>&1 \
        || { warn "лог: $WORK_DIR/entt-build.log"; die "не удалось собрать EnTT"; }
    cmake --install "$src/build" >/dev/null

    [ -f "$config" ] || die "EnTT установлен, но не найден $config"
    info "готов"
}

# --- Jolt --------------------------------------------------------------------

install_jolt() {
    local config="$PREFIX/lib/cmake/Jolt/JoltConfig.cmake"
    if [ -f "$config" ] && [ "$FORCE" -eq 0 ]; then
        say "Jolt $JOLT_TAG: уже установлен"
        return
    fi

    say "Jolt Physics $JOLT_TAG"
    local src="$WORK_DIR/jolt"
    info "клонирую $JOLT_URL"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$JOLT_TAG" "$JOLT_URL" "$src"

    # JPH_USE_VK=OFF — отключает реализацию на compute-шейдерах внутри Jolt:
    # рендером всё равно занимается src/renderer, а ещё это снимает требование DXC.
    # Остальные TARGET_*=OFF убирают примеры, которые мы не собираем.
    # Jolt собираем только под Release, он большой и в Debug собирается долго.
    info "собираю (это долго, Jolt — большая библиотека)"
    cmake -S "$src/Build" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DJPH_USE_VK=OFF \
        -DTARGET_SAMPLES=OFF \
        -DTARGET_VIEWER=OFF \
        -DTARGET_PERFORMANCE_TEST=OFF \
        -DTARGET_UNIT_TESTS=OFF \
        -DTARGET_HELLO_WORLD=OFF \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        >"$WORK_DIR/jolt-configure.log" 2>&1 \
        || { warn "лог: $WORK_DIR/jolt-configure.log"; die "не удалось сконфигурировать Jolt"; }
    cmake --build "$src/build" --parallel "$JOBS" \
        >"$WORK_DIR/jolt-build.log" 2>&1 \
        || { warn "лог: $WORK_DIR/jolt-build.log"; die "не удалось собрать Jolt"; }
    cmake --install "$src/build" >/dev/null

    [ -f "$config" ] || die "Jolt установлен, но не найден $config"
    info "готов"
}

# --- Сборка проекта ----------------------------------------------------------

configure_project() {
    say "Конфигурация проекта"

    # Если в кэше остался префикс от прошлой установки (например, от /tmp,
    # который уже очистили), CMake продолжит искать зависимости там и упадёт
    # с вводящим в заблуждение «Could not find a package». Пересоздаём кэш.
    local cache="$BUILD_PATH/CMakeCache.txt"
    if [ -f "$cache" ]; then
        local cached
        cached="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "$cache" | head -1)"
        if [ -n "$cached" ] && [ "$cached" != "$PREFIX" ]; then
            warn "в кэше сборки прописан другой префикс: $cached"
            warn "сбрасываю кэш, зависимости возьмутся из $PREFIX"
            rm -f "$cache"
        fi
    fi

    cmake -S "$PROJECT_DIR" -B "$BUILD_PATH" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        >"$WORK_DIR/project-configure.log" 2>&1 \
        || { warn "лог: $WORK_DIR/project-configure.log"; die "не удалось сконфигурировать проект"; }
    info "готово"
}

build_project() {
    say "Сборка проекта ($BUILD_TYPE)"
    cmake --build "$BUILD_PATH" --parallel "$JOBS" \
        >"$WORK_DIR/project-build.log" 2>&1 \
        || { warn "лог: $WORK_DIR/project-build.log"; die "не удалось собрать проект"; }

    local binary="$BUILD_PATH/src/antiworld"
    [ -x "$binary" ] || die "сборка завершилась, но нет исполняемого файла $binary"
    info "бинарник: $binary"
}

# --- Основной сценарий -------------------------------------------------------

WORK_DIR="$(mktemp -d)"

if [ "$DO_DEPS" -eq 1 ]; then
    install_system_packages
    install_entt
    install_jolt
else
    say "Зависимости: пропущены (--no-deps)"
    [ -f "$ENTT_CONFIG" ] || warn "EnTT не найден в $PREFIX, find_package(EnTT) может упасть"
    [ -f "$JOLT_CONFIG" ] || warn "Jolt не найден в $PREFIX, find_package(Jolt) может упасть"
fi

if [ "$DO_BUILD" -eq 1 ]; then
    configure_project
    build_project
else
    say "Сборка проекта: пропущена (--no-build)"
fi

cat <<EOF

${C_BOLD}${C_GREEN}Готово.${C_OFF}

Запуск:
    $BUILD_PATH/src/antiworld

Чтобы собрать заново (зависимости уже стоят):
    $PROJECT_DIR/install.sh --no-deps

Удалить собранное:
    rm -rf $BUILD_PATH
EOF
