# external/CMakeLists.txt
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/json.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/glm.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/vma.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinygltf.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/miniaudio.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/jolt.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/assimp.cmake)

# Sub-plan 4.A — NRD (NVIDIA RayTracingDenoiser). Gated by OHAO_NRD option.
if(OHAO_NRD)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/nrd.cmake)
endif()
