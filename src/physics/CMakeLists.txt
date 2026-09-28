add_library(physics STATIC
    physics_world.cpp
    physics_system.cpp
    player_controller.cpp
)

target_include_directories(physics PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

target_link_libraries(physics PUBLIC
    ecs
    Jolt::Jolt
    glm::glm
)
