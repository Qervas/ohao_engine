# OHAO Render Library — rendering algorithms, passes, RT
# GPU abstraction lives in gpu/vulkan/

find_package(OpenImageDenoise REQUIRED)

file(GLOB_RECURSE RENDERER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

# Exclude disabled passes
list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/_disabled/")

add_library(ohao_renderer ${RENDERER_SOURCES})

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
)

target_link_libraries(ohao_renderer PUBLIC
    ohao_gpu_vulkan
    ohao_scene
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
    OpenImageDenoise
)

# Conditional NRD support (Sub-plan 4.A). nrd_denoise.cpp is always in
# RENDERER_SOURCES via the glob; its body is guarded by OHAO_NRD_ENABLED
# so it compiles to an empty TU when OHAO_NRD=OFF.
if(OHAO_NRD)
    target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)
    target_link_libraries(ohao_renderer PRIVATE NRD)
    # Sub-plan 4.C T3a: our own NRDIntegration static lib is named
    # OhaoNRDIntegration to avoid clash with NRD's upstream INTERFACE target
    # (which points at nrd-src/Integration/ and carries no symbols).
    if(TARGET OhaoNRDIntegration)
        target_link_libraries(ohao_renderer PRIVATE OhaoNRDIntegration)
        target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_INTEGRATION_AVAILABLE)
    endif()
endif()
