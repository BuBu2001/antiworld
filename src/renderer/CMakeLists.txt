# Модуль рендеринга: базовый слой Vulkan + GLFW-окно + шейдеры.
#
# Шейдеры (GLSL 450) компилируются в SPIR-V через glslc на этапе сборки.
# Полученные .spv лежат в ${CMAKE_BINARY_DIR}/shaders и встраиваются в код
# через макрос SHADERS_ROOT_DIR (абсолютный путь, не зависящий от CWD).
find_program(GLSLC glslc REQUIRED)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc не найден — установите пакет shaderc (Arch: sudo pacman -S shaderc)")
endif()

set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Общие куски GLSL (sky_common.glsl) подключаются через #include из
# triangle.frag и sky.frag. -I нужен glslc'у, иначе он не найдёт файл по
# имени; каждый шейдер также пересобирается при изменении общего куска —
# без этого в DEPENDS правка неба не пересобирала бы terrain.frag, и два
# шейдера разошлись бы по цвету дымки.
set(SHADER_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_COMMON ${SHADER_INCLUDE_DIR}/sky_common.glsl)

set(SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/shaders/sky.vert
    ${CMAKE_SOURCE_DIR}/shaders/sky.frag
    ${CMAKE_SOURCE_DIR}/shaders/map.vert
    ${CMAKE_SOURCE_DIR}/shaders/map.frag
    ${CMAKE_SOURCE_DIR}/shaders/hud.vert
    ${CMAKE_SOURCE_DIR}/shaders/hud.frag
)

set(SHADER_OUTPUTS)
foreach(shader IN LISTS SHADER_SOURCES)
    get_filename_component(shader_name ${shader} NAME)
    set(shader_output ${SHADER_OUTPUT_DIR}/${shader_name}.spv)
    add_custom_command(
        OUTPUT ${shader_output}
        COMMAND ${GLSLC} -I${SHADER_INCLUDE_DIR} -O ${shader} -o ${shader_output}
        DEPENDS ${shader} ${SHADER_COMMON}
        COMMENT "Компиляция шейдера ${shader_name}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${shader_output})
endforeach()

add_custom_target(antiworld_shaders DEPENDS ${SHADER_OUTPUTS})

add_library(renderer STATIC
    vulkan_base.cpp
    shader.cpp
    uniform_buffer.cpp
    render_pass.cpp
    vertex_buffer.cpp
    texture.cpp
    world_map_pass.cpp
    hud_pass.cpp
    hud_font.cpp
    model_loader.cpp
    mesh.cpp
    pipeline.cpp
    command_buffers.cpp
    lod_manager.cpp
    frustum_culling.cpp
    instanced_renderer.cpp
    vegetation_geometry.cpp
)

# Шейдеры собираются до/вместе с библиотекой.
add_dependencies(renderer antiworld_shaders)

target_include_directories(renderer PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

# Абсолютный путь к скомпилированным шейдерам для Shader::shaderPath().
target_compile_definitions(renderer PRIVATE SHADERS_ROOT_DIR="${SHADER_OUTPUT_DIR}")

# VulkanBase и остальные классы используют core::Window и core::Logger.
target_link_libraries(renderer PUBLIC
    core
    glfw
    Vulkan::Vulkan
)