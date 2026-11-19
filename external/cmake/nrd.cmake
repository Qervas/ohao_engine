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
# NRD's SPIR-V from ShaderMake's pinned DXC (v1.8.2505, downloaded), never a Vulkan SDK's: the SDK 1.4.363's DXC
# emits SPV_KHR_compute_shader_derivatives for NRD's compute derivatives, a device extension the engine does not
# enable (VUID-VkShaderModuleCreateInfo-pCode-08740/08742). The same DXC on every platform.
set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "" FORCE)
unset(SHADERMAKE_DXC_VK_PATH CACHE)

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
# NRD's CMakeLists writes Shaders/NRDConfig.hlsli at every configure, and every NRD shader includes it: each
# configure would recompile NRD's shaders and library and relink every program (a shader edit reconfigures). The
# header gets its old time back when its text is unchanged (file(COPY) keeps a file's time).
set(OHAO_NRD_CONFIG "${FETCHCONTENT_BASE_DIR}/nrd-src/Shaders/NRDConfig.hlsli")
set(OHAO_NRD_CONFIG_KEPT "${CMAKE_BINARY_DIR}/nrd_config_kept")
if(EXISTS "${OHAO_NRD_CONFIG}")
    file(COPY "${OHAO_NRD_CONFIG}" DESTINATION "${OHAO_NRD_CONFIG_KEPT}")
endif()
# NRD compiles its shaders in a custom target, which a build runs every time (ShaderMake then finds them current:
# 50 ms of an unchanged tree's build). add_custom_target is wrapped while NRD is configured: NRDShaders' commands
# become a custom command with a stamp, run when ShaderMake, a shader or Shaders.cfg changes. Every other call goes
# to CMake's own as it came (each argument a bracket argument, so empty ones and semicolons survive).
function(add_custom_target name)
    set(passed "")
    set(command "")
    set(sources "")
    set(mode "")
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 1 ${last})
        if(last EQUAL 0)
            break()
        endif()
        set(a "${ARGV${i}}")
        string(APPEND passed " [==[${a}]==]")
        if(a STREQUAL "SOURCES")
            set(mode sources)
        elseif(a MATCHES "^(ALL|COMMAND|DEPENDS|WORKING_DIRECTORY|VERBATIM|COMMENT)$")
            set(mode "")
        endif()
        if(mode STREQUAL "sources")
            if(NOT a STREQUAL "SOURCES")
                list(APPEND sources "${a}")
            endif()
        elseif(NOT a STREQUAL "ALL")
            string(APPEND command " [==[${a}]==]")
        endif()
    endforeach()
    if(NOT (OHAO_NRD_WRAPPING AND name STREQUAL "NRDShaders"))
        cmake_language(EVAL CODE "_add_custom_target([==[${name}]==]${passed})")
        return()
    endif()
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/NRDShaders.stamp")
    set(inputs "")
    foreach(s ${sources})
        string(APPEND inputs " [==[${s}]==]")
    endforeach()
    cmake_language(EVAL CODE "add_custom_command(OUTPUT [==[${stamp}]==]${command}
        COMMAND [==[${CMAKE_COMMAND}]==] -E touch [==[${stamp}]==]
        DEPENDS [==[${SHADERMAKE_PATH}]==] [==[${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Shaders.cfg]==]${inputs})")
    _add_custom_target(NRDShaders ALL DEPENDS "${stamp}" SOURCES ${sources})
endfunction()
set(OHAO_NRD_WRAPPING ON)
FetchContent_MakeAvailable(NRD)
set(OHAO_NRD_WRAPPING OFF)
if(EXISTS "${OHAO_NRD_CONFIG_KEPT}/NRDConfig.hlsli")
    file(READ "${OHAO_NRD_CONFIG}" OHAO_NRD_CONFIG_NOW)
    file(READ "${OHAO_NRD_CONFIG_KEPT}/NRDConfig.hlsli" OHAO_NRD_CONFIG_BEFORE)
    if(OHAO_NRD_CONFIG_NOW STREQUAL OHAO_NRD_CONFIG_BEFORE)
        file(COPY "${OHAO_NRD_CONFIG_KEPT}/NRDConfig.hlsli" DESTINATION "${FETCHCONTENT_BASE_DIR}/nrd-src/Shaders")
    endif()
endif()

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
# leaves OHAO's own Werror policy (if any) untouched. MSVC's /WX the same, and
# NRD's own: MSVC 14.52 warns (C4834) on MathLib's discarded [[nodiscard]] results.
foreach(nri_tgt NRD NRI NRI_Shared NRI_VK NRI_Validation)
    if(TARGET ${nri_tgt})
        get_target_property(_opts ${nri_tgt} COMPILE_OPTIONS)
        if(_opts)
            # -Werror lives inside a generator-expression list; filter it out
            # (works because CMake stores options as a ;-list internally).
            list(REMOVE_ITEM _opts "-Werror" "/WX")
            set_target_properties(${nri_tgt} PROPERTIES COMPILE_OPTIONS "${_opts}")
        endif()
        get_target_property(_iopts ${nri_tgt} INTERFACE_COMPILE_OPTIONS)
        if(_iopts)
            list(REMOVE_ITEM _iopts "-Werror" "/WX")
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
