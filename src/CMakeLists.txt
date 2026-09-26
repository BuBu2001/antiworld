add_subdirectory(core)
add_subdirectory(renderer)
add_subdirectory(ecs)
add_subdirectory(physics)
add_subdirectory(world)
add_subdirectory(ai)

add_executable(antiworld main.cpp)

target_compile_definitions(antiworld PRIVATE
    ANTIWORLD_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)

target_link_libraries(antiworld PRIVATE
    core
    renderer
    ecs
    physics
    world
    ai
    Vulkan::Vulkan
    glfw
    glm::glm
    EnTT::EnTT
    Jolt::Jolt
)