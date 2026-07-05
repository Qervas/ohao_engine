# VulkanMemoryAllocator configuration - header only library
include(FetchContent)

# Declare VMA repository
# Pinned to the same commit NRI v178 references (post-3.1.0 master) so
# NRI_VK can compile against flags like VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT.
# VMA's 3.x series keeps source-compatible API for OHAO's existing callers
# in ohao/gpu/vulkan/gpu_allocator.cpp. Must stay in sync with the URL pin
# inside build/_deps/nri-src/CMakeLists.txt (Sub-plan 4.C T3a).
FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG e722e57c891a8fbe3cc73ca56c19dd76be242759
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
