# Find glslc compiler
find_program(GLSLC glslc)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc shader compiler not found!")
endif()

# Find all shader files (including subdirectories)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.geom"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tesc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tese"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rgen"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rmiss"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rchit"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rahit"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rint"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rcall"
)

# Create output directory
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders/
)

# Find include files for dependency tracking (all subdirectories)
file(GLOB_RECURSE SHADER_INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/**/*.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/water/*.glsl"
)

# Generate compilation commands for each shader
set(SHADER_BINARY_FILES "")
foreach(SHADER ${SHADER_SOURCES})
    # Get relative path from shaders directory to create unique output names
    file(RELATIVE_PATH SHADER_REL ${CMAKE_CURRENT_SOURCE_DIR} ${SHADER})
    # Replace directory separators with underscores for flat output structure
    string(REPLACE "/" "_" SHADER_FLAT_NAME ${SHADER_REL})
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${SHADER_FLAT_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/includes ${SHADER} -o ${SPIRV}
        DEPENDS ${SHADER} ${SHADER_INCLUDES} ${CMAKE_BINARY_DIR}/shaders
        COMMENT "Compiling shader ${SHADER_FLAT_NAME}"
    )
    list(APPEND SHADER_BINARY_FILES ${SPIRV})
endforeach()

# ---- Slang (sub-project 2b spec, "Slang") -------------------------------------------------
# slangc from the Vulkan SDK's Bin ($VULKAN_SDK, then C:/VulkanSDK/<newest>), then PATH. The
# version the SPIR-V was made with is pinned in slangc.version: another one may emit other
# SPIR-V, so a mismatch is a warning here and in `ohao doctor` (tools.slangc).
file(GLOB OHAO_VULKAN_SDK_BINS "C:/VulkanSDK/*/Bin")
list(SORT OHAO_VULKAN_SDK_BINS)
list(REVERSE OHAO_VULKAN_SDK_BINS)
find_program(SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" ${OHAO_VULKAN_SDK_BINS})
if(NOT SLANGC)
    message(FATAL_ERROR "slangc not found: install the Vulkan SDK (it ships slangc), or put slangc on PATH")
endif()
execute_process(COMMAND ${SLANGC} -v OUTPUT_VARIABLE OHAO_SLANGC_OUT ERROR_VARIABLE OHAO_SLANGC_ERR)
string(STRIP "${OHAO_SLANGC_OUT}${OHAO_SLANGC_ERR}" OHAO_SLANGC_VERSION)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/slangc.version OHAO_SLANGC_PINNED LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/slangc.version)
string(FIND "${OHAO_SLANGC_VERSION}" "${OHAO_SLANGC_PINNED}" OHAO_SLANGC_AT)
if(NOT OHAO_SLANGC_AT EQUAL 0)
    message(WARNING "slangc ${OHAO_SLANGC_VERSION} (${SLANGC}) is not the pinned ${OHAO_SLANGC_PINNED} "
                    "(shaders/slangc.version): the SPIR-V may differ from the goldens' build")
endif()
message(STATUS "slangc: ${SLANGC} (${OHAO_SLANGC_VERSION})")

# Every *.slang. A file with [shader("stage")] entry points is compiled, one SPIR-V per entry:
# a single entry gives <flattened path>.spv, several give <flattened path>.<entry>.spv. A file
# without one is a module, only imported (shared/layout.slang). The entry is the identifier
# before the first '(' after the [shader(...)] attribute and any further [...] attribute lines;
# its SPIR-V entry point keeps that name (-fvk-use-entrypoint-name). Matrices are column-major,
# as in GLSL and glm. Each target depends on every .slang (imports), and the configure step
# reruns when one changes, so a new entry point gets its rule.
file(GLOB_RECURSE SLANG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.slang")
foreach(SLANG ${SLANG_SOURCES})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SLANG})
    file(READ ${SLANG} SLANG_TEXT)
    string(REGEX MATCHALL
        "\\[shader\\(\"[a-z]+\"\\)\\]([ \t\r\n]*\\[[^\r\n]*\\])*[ \t\r\n]*[A-Za-z_][A-Za-z0-9_<>]*[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*\\("
        SLANG_DECLS "${SLANG_TEXT}")
    set(SLANG_ENTRIES "")
    foreach(DECL ${SLANG_DECLS})
        string(REGEX MATCH "([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\\($" _ "${DECL}")
        list(APPEND SLANG_ENTRIES ${CMAKE_MATCH_1})
    endforeach()
    list(LENGTH SLANG_ENTRIES SLANG_ENTRY_COUNT)
    file(RELATIVE_PATH SLANG_REL ${CMAKE_CURRENT_SOURCE_DIR} ${SLANG})
    string(REPLACE "/" "_" SLANG_FLAT ${SLANG_REL})
    foreach(ENTRY ${SLANG_ENTRIES})
        if(SLANG_ENTRY_COUNT EQUAL 1)
            set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${SLANG_FLAT}.spv")
        else()
            set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${SLANG_FLAT}.${ENTRY}.spv")
        endif()
        get_filename_component(SPIRV_NAME ${SPIRV} NAME)
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${SLANGC} ${SLANG} -target spirv -profile spirv_1_6 -entry ${ENTRY} -fvk-use-entrypoint-name
                    -matrix-layout-column-major -I ${CMAKE_CURRENT_SOURCE_DIR} -o ${SPIRV}
            DEPENDS ${SLANG_SOURCES} ${CMAKE_BINARY_DIR}/shaders
            COMMENT "Compiling Slang shader ${SPIRV_NAME}"
        )
        list(APPEND SHADER_BINARY_FILES ${SPIRV})
    endforeach()
endforeach()

# Create shader compilation target
add_custom_target(shaders
    DEPENDS ${SHADER_BINARY_FILES}
    COMMENT "Compiling all shaders"
)

# Copy compiled SPVs into the runtime output directory ONCE, here, instead of
# each consuming test target attaching its own POST_BUILD copy_directory into
# the same destination.
# Two independent copies into the same directory race under a parallel
# (-j8) build and can fail with "Error copying directory ...
# build/Release/bin/shaders". Test targets should add_dependencies(<tgt>
# shaders) and rely on this copy instead of performing their own.
#
# Mirror CMake's own default per-config RUNTIME_OUTPUT_DIRECTORY behavior
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY is set to CMAKE_BINARY_DIR with no
# per-config override in the top-level CMakeLists.txt) so this resolves to
# the same directory executables actually land in, on both multi-config
# generators (e.g. Visual Studio, where CMake appends /$<CONFIG>) and
# single-config generators (e.g. Ninja/Makefiles, where it does not).
get_property(OHAO_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(OHAO_IS_MULTI_CONFIG)
    set(OHAO_SHADER_RUNTIME_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(OHAO_SHADER_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
endif()

add_custom_command(TARGET shaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OHAO_SHADER_RUNTIME_DIR}/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "${OHAO_SHADER_RUNTIME_DIR}/bin/shaders"
    COMMENT "Copying shader SPVs to runtime output directory"
)

# Optional: Print found shader files
message(STATUS "Found shader files:")
foreach(SHADER ${SHADER_SOURCES})
    message(STATUS "  ${SHADER}")
endforeach()
