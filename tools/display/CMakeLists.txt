# The display transform on the CPU (docs/plans/2026-10-30-frontier-f2.md, T1):
# shaders/display/display.slang compiled to C++ by slangc through display_kernel.slang, for
# tests/renderer/display_test. SLANGC_CPP (the twins' slangc) comes from shaders/CMakeLists.txt.
set(OHAO_DISPLAY_KERNEL_CPP ${CMAKE_BINARY_DIR}/display/display_kernel.cpp)
file(GLOB OHAO_DISPLAY_SLANG CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/display/*.slang)
add_custom_command(
    OUTPUT ${OHAO_DISPLAY_KERNEL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/display
    COMMAND ${SLANGC_CPP} ${CMAKE_CURRENT_SOURCE_DIR}/display_kernel.slang -I ${CMAKE_SOURCE_DIR}/shaders
            -target cpp -entry run -stage compute -o ${OHAO_DISPLAY_KERNEL_CPP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/display_kernel.slang ${OHAO_DISPLAY_SLANG}
    COMMENT "slangc: the display transform to C++"
    VERBATIM)

add_library(ohao_display_cpu STATIC display_cpu.cpp ${OHAO_DISPLAY_KERNEL_CPP})
set_source_files_properties(${OHAO_DISPLAY_KERNEL_CPP} PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ohao_display_cpu PUBLIC cxx_std_20)
target_include_directories(ohao_display_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/display)
