# GLFW configuration using FetchContent
include(FetchContent)

# Find Vulkan first to ensure GLFW can link against it properly
find_package(Vulkan REQUIRED)

# Declare GLFW repository
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4  # Updated for better macOS/MoltenVK support
)

# GLFW build options
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)

# Make GLFW available
FetchContent_MakeAvailable(glfw)

message(STATUS "GLFW included and built from source")
message(STATUS "Vulkan found: ${Vulkan_FOUND}")
message(STATUS "Vulkan include dir: ${Vulkan_INCLUDE_DIRS}")
