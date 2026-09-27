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

set(SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/shaders/triangle.frag
)

set(SHADER_OUTPUTS)
foreach(shader IN LISTS SHADER_SOURCES)
    get_filename_component(shader_name ${shader} NAME)
    set(shader_output ${SHADER_OUTPUT_DIR}/${shader_name}.spv)
    add_custom_command(
        OUTPUT ${shader_output}
        COMMAND ${GLSLC} -O ${shader} -o ${shader_output}
        DEPENDS ${shader}
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
    model_loader.cpp
    mesh.cpp
    pipeline.cpp
    command_buffers.cpp
    lod_manager.cpp
    frustum_culling.cpp
    instanced_renderer.cpp
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