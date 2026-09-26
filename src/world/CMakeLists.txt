# Модуль мира: процедурный ландшафт (шум -> карта высот -> mesh + коллайдер),
# биомы (пустыня/лес/тундра/горы) и климат (времена года, температура).
# Зависит от renderer (mesh, материалы), physics (тела Jolt) и core (логгер).
add_library(world STATIC
    noise.cpp
    biome.cpp
    climate.cpp
    terrain_generator.cpp
    terrain.cpp
    spatial_hash.cpp
)

target_include_directories(world PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

target_link_libraries(world PUBLIC
    core
    physics
    renderer
    glm::glm
    EnTT::EnTT
    Jolt::Jolt
)
