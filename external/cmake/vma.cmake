# VulkanMemoryAllocator configuration - header only library
include(FetchContent)

# Declare VMA repository
FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.0.1
)

# Make VMA available
FetchContent_GetProperties(vma)
if(NOT vma_POPULATED)
    FetchContent_Populate(vma)
endif()

# Create VMA interface library
if(NOT TARGET VulkanMemoryAllocator)
    add_library(VulkanMemoryAllocator INTERFACE)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${vma_SOURCE_DIR}/include)

    # VMA requires Vulkan headers
    find_package(Vulkan REQUIRED)
    target_link_libraries(VulkanMemoryAllocator INTERFACE Vulkan::Vulkan)
endif()

message(STATUS "VulkanMemoryAllocator included at: ${vma_SOURCE_DIR}")
