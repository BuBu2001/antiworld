add_library(ecs STATIC
    systems.cpp
    world.cpp
)

target_include_directories(ecs PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

target_link_libraries(ecs PUBLIC
    EnTT::EnTT
    glm::glm
    Jolt::Jolt
    core
    renderer
)