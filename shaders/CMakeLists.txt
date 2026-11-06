# The output directory, made at configure time: a custom command whose output is the directory gave Ninja an edge
# named `shaders`, the target's own name, so `--target shaders` built the directory and nothing else.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shaders)

set(SHADER_BINARY_FILES "")

# ---- Slang (sub-project 2b spec, "Slang") -------------------------------------------------
# The version the SPIR-V and the CPU twins are made with is pinned in slangc.version: another
# one may emit other SPIR-V, so a mismatch is a warning here and in `ohao doctor` (tools.slangc).
# slangc from Slang's own release unpacked at ~/.local/opt/slang-<pin> (Linux), the Vulkan SDK's
# Bin ($VULKAN_SDK, then C:/VulkanSDK/<newest>), then PATH; -DSLANGC=<path> overrides them all.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/slangc.version OHAO_SLANGC_PINNED LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/slangc.version)
file(GLOB OHAO_VULKAN_SDK_BINS "C:/VulkanSDK/*/Bin")
list(SORT OHAO_VULKAN_SDK_BINS)
list(REVERSE OHAO_VULKAN_SDK_BINS)
find_program(SLANGC slangc HINTS "$ENV{HOME}/.local/opt/slang-${OHAO_SLANGC_PINNED}/bin" "$ENV{VULKAN_SDK}/Bin"
             ${OHAO_VULKAN_SDK_BINS})
if(NOT SLANGC)
    message(FATAL_ERROR "slangc not found: unpack Slang's release ${OHAO_SLANGC_PINNED} into "
                        "~/.local/opt/slang-${OHAO_SLANGC_PINNED}, or pass -DSLANGC=<path>")
endif()
execute_process(COMMAND ${SLANGC} -v OUTPUT_VARIABLE OHAO_SLANGC_OUT ERROR_VARIABLE OHAO_SLANGC_ERR)
string(STRIP "${OHAO_SLANGC_OUT}${OHAO_SLANGC_ERR}" OHAO_SLANGC_VERSION)
string(FIND "${OHAO_SLANGC_VERSION}" "${OHAO_SLANGC_PINNED}" OHAO_SLANGC_AT)
if(NOT OHAO_SLANGC_AT EQUAL 0)
    message(WARNING "slangc ${OHAO_SLANGC_VERSION} (${SLANGC}) is not the pinned ${OHAO_SLANGC_PINNED} "
                    "(shaders/slangc.version): the SPIR-V may differ from the goldens' build")
endif()
message(STATUS "slangc: ${SLANGC} (${OHAO_SLANGC_VERSION})")

# The CPU twins (tools/caustics, display, material, sampling: the same modules through slangc's C++ target) are made
# by the version pinned in slangc_cpp.version, SLANGC_CPP, looked for where SLANGC is. 2026.20's C++ emitter
# re-reads a member after a store to it (`Sampler.next()` returned the next coordinate; its SPIR-V is right), so a
# twin from it is not the module's code.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/slangc_cpp.version OHAO_SLANGC_CPP_PINNED LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/slangc_cpp.version)
find_program(SLANGC_CPP slangc HINTS "$ENV{HOME}/.local/opt/slang-${OHAO_SLANGC_CPP_PINNED}/bin" "$ENV{VULKAN_SDK}/Bin"
             ${OHAO_VULKAN_SDK_BINS})
if(NOT SLANGC_CPP)
    message(FATAL_ERROR "slangc for the CPU twins not found: unpack Slang's release ${OHAO_SLANGC_CPP_PINNED} into "
                        "~/.local/opt/slang-${OHAO_SLANGC_CPP_PINNED}, or pass -DSLANGC_CPP=<path>")
endif()
execute_process(COMMAND ${SLANGC_CPP} -v OUTPUT_VARIABLE OHAO_SLANGC_CPP_OUT ERROR_VARIABLE OHAO_SLANGC_CPP_ERR)
string(STRIP "${OHAO_SLANGC_CPP_OUT}${OHAO_SLANGC_CPP_ERR}" OHAO_SLANGC_CPP_VERSION)
string(FIND "${OHAO_SLANGC_CPP_VERSION}" "${OHAO_SLANGC_CPP_PINNED}" OHAO_SLANGC_CPP_AT)
if(NOT OHAO_SLANGC_CPP_AT EQUAL 0)
    message(WARNING "slangc ${OHAO_SLANGC_CPP_VERSION} (${SLANGC_CPP}) is not the CPU twins' pinned "
                    "${OHAO_SLANGC_CPP_PINNED} (shaders/slangc_cpp.version)")
endif()
message(STATUS "slangc for the CPU twins: ${SLANGC_CPP} (${OHAO_SLANGC_CPP_VERSION})")

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
    # Every [shader(...)] attribute must head an entry point this parse finds: one that does not
    # (an attribute left above a struct) gets no rule, and the last good SPIR-V would stay in use
    # unnoticed (the transparent pass ran OpenPBR step 1's shader through step 2 that way).
    string(REGEX MATCHALL "\\[shader\\(\"[a-z]+\"\\)\\]" SLANG_ATTRIBUTES "${SLANG_TEXT}")
    list(LENGTH SLANG_ATTRIBUTES SLANG_ATTRIBUTE_COUNT)
    if(NOT SLANG_ATTRIBUTE_COUNT EQUAL SLANG_ENTRY_COUNT)
        message(FATAL_ERROR "${SLANG}: ${SLANG_ATTRIBUTE_COUNT} [shader(...)] attributes but ${SLANG_ENTRY_COUNT} entry "
                            "points parsed: put each attribute directly above its entry function")
    endif()
    # A shader with inline ray queries declares the capability (else slangc warns and adds it).
    set(SLANG_CAPS "")
    if(SLANG_TEXT MATCHES "RayQuery<")
        set(SLANG_CAPS -capability spvRayQueryKHR)
    endif()
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
            COMMAND ${SLANGC} ${SLANG} -target spirv -profile spirv_1_6 ${SLANG_CAPS} -entry ${ENTRY} -fvk-use-entrypoint-name
                    -matrix-layout-column-major -I ${CMAKE_CURRENT_SOURCE_DIR} -o ${SPIRV}
            DEPENDS ${SLANG_SOURCES}
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
