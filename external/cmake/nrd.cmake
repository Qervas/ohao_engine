# NRD — NVIDIA RayTracingDenoiser (MIT License)
# Spatio-temporal denoiser library used for realtime RT diffuse/specular/shadow
# signals. Integrated as a hard CMake dependency via FetchContent (Sub-plan 4.A).
# https://github.com/NVIDIAGameWorks/RayTracingDenoiser

include(FetchContent)

# Trim NRD's build surface. NRD's CMake exposes several toggles that default
# ON and pull in tools we don't need (sample, integration helpers, CI shader
# validation). Keep only the core library + SPIR-V shaders for Vulkan.
# These are set as CACHE BOOL before MakeAvailable so NRD picks them up.
set(NRD_DISABLE_SHADER_COMPILATION OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS        OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS        OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS       ON  CACHE BOOL "" FORCE)
set(NRD_STATIC_LIBRARY             ON  CACHE BOOL "" FORCE)

# Pin NRD normal+roughness encoding to R10G10B10A2_UNORM (rotated oct + sign-in-roughness
# + 2-bit materialID). This is NRD's upstream default, but making it explicit protects
# us from future NRD bumps silently flipping the encoding.
# Roughness encoding: LINEAR (1) = raw linear roughness stored as-is (NRD default).
set(NRD_NORMAL_ENCODING    "2" CACHE STRING "" FORCE)
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)

FetchContent_Declare(
    NRD
    GIT_REPOSITORY https://github.com/NVIDIAGameWorks/RayTracingDenoiser.git
    GIT_TAG        v4.17.2
)
FetchContent_MakeAvailable(NRD)

if(TARGET NRD)
    message(STATUS "NRD: target 'NRD' available (pinned tag v4.17.2)")
else()
    message(WARNING "NRD: target 'NRD' NOT found after FetchContent_MakeAvailable")
endif()

# Sub-plan 4.C T3a: NRI (pulled in by NRD_NRI=ON) hard-codes -Werror on its
# NRI_Shared PUBLIC compile options, and NRI_VK compiles a bundled copy of
# vk_mem_alloc.h as a translation unit. On our GCC 14.x, VMA emits
# -Wimplicit-fallthrough warnings that trip NRI's -Werror. NRI offers no
# knob to disable this, and simply appending -Wno-error is shadowed by the
# later -Werror in NRI_Shared's PUBLIC INTERFACE. Surgically strip -Werror
# out of NRI_Shared's compile options here — affects only NRI's own compiles,
# leaves OHAO's own Werror policy (if any) untouched.
foreach(nri_tgt NRI NRI_Shared NRI_VK NRI_Validation)
    if(TARGET ${nri_tgt})
        get_target_property(_opts ${nri_tgt} COMPILE_OPTIONS)
        if(_opts)
            # -Werror lives inside a generator-expression list; filter it out
            # (works because CMake stores options as a ;-list internally).
            list(REMOVE_ITEM _opts "-Werror")
            set_target_properties(${nri_tgt} PROPERTIES COMPILE_OPTIONS "${_opts}")
        endif()
        get_target_property(_iopts ${nri_tgt} INTERFACE_COMPILE_OPTIONS)
        if(_iopts)
            list(REMOVE_ITEM _iopts "-Werror")
            set_target_properties(${nri_tgt} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_iopts}")
        endif()
    endif()
endforeach()

# Sub-plan 4.C: NRDIntegration — NVIDIA's reference Vulkan integration helper,
# vendored into external/nrd_integration/. Wraps NRD's "recipe" API
# (GetComputeDispatches, GetInstanceDesc) with concrete Vulkan pipeline,
# descriptor pool, texture pool, and barrier management.
set(OHAO_NRD_INTEGRATION_DIR ${CMAKE_SOURCE_DIR}/external/nrd_integration)

# CONFIGURE_DEPENDS so that any .cpp added to or removed from
# external/nrd_integration/ triggers a re-configure and the static-lib
# branch below flips on/off automatically — no manual reconfigure needed.
# (T3a added NRDIntegration.cpp here; the glob picks it up.)
file(GLOB OHAO_NRD_INTEGRATION_SOURCES CONFIGURE_DEPENDS
    "${OHAO_NRD_INTEGRATION_DIR}/*.cpp"
    "${OHAO_NRD_INTEGRATION_DIR}/*.cxx"
    "${OHAO_NRD_INTEGRATION_DIR}/*.cc"
    "${OHAO_NRD_INTEGRATION_DIR}/*.h"
    "${OHAO_NRD_INTEGRATION_DIR}/*.hpp"
)

if(OHAO_NRD_INTEGRATION_SOURCES)
    set(OHAO_NRD_HAS_CPP OFF)
    foreach(src ${OHAO_NRD_INTEGRATION_SOURCES})
        if(src MATCHES "\\.(cpp|cxx|cc)$")
            set(OHAO_NRD_HAS_CPP ON)
        endif()
    endforeach()
    if(OHAO_NRD_HAS_CPP)
        # Sub-plan 4.C T3a: NRDIntegration depends on NRI for its abstraction
        # over Vulkan pipelines/barriers/resources. Verify NRI target is
        # available (pulled in by NRD_NRI=ON via external/cmake/nri.cmake).
        if(NOT TARGET NRI)
            message(FATAL_ERROR
                "NRD: OhaoNRDIntegration.cpp is present but NRI target is missing. "
                "Ensure external/cmake/nri.cmake is included before nrd.cmake "
                "(sets NRD_NRI=ON so NRD's own FetchContent pulls NRI v178).")
        endif()
        # Note: NRD's own CMakeLists defines an INTERFACE target literally
        # named `NRDIntegration` (points at nrd-src/Integration/). We build
        # our concrete static lib under a namespaced name to avoid the clash,
        # while still consuming the vendored .hpp from external/nrd_integration/.
        add_library(OhaoNRDIntegration STATIC ${OHAO_NRD_INTEGRATION_SOURCES})
        target_include_directories(OhaoNRDIntegration PUBLIC
            ${OHAO_NRD_INTEGRATION_DIR}
            ${Vulkan_INCLUDE_DIRS}
        )
        target_link_libraries(OhaoNRDIntegration PUBLIC NRD NRI ${Vulkan_LIBRARIES})
        target_compile_features(OhaoNRDIntegration PUBLIC cxx_std_17)
        message(STATUS "NRD: OhaoNRDIntegration static lib enabled (NRI-backed, Vulkan-only)")
    else()
        message(STATUS "NRD: NRDIntegration vendored as headers-only (no .cpp TU); skipping static lib")
    endif()
endif()
