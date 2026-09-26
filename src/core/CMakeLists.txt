add_library(core STATIC
    logger.cpp
    window.cpp
    floating_origin.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

# GLFW и Vulkan нужны окну (GLFW_INCLUDE_VULKAN) и потребителям core.
target_link_libraries(core PUBLIC
    glfw
    Vulkan::Vulkan
    glm::glm
)