# OpenPBR on the CPU (docs/plans/2026-10-29-openpbr-step1.md, T1): shaders/material/openpbr.slang
# compiled to C++ by slangc through openpbr_kernel.slang, for the energy-table generator and the
# module's tests (tests/material). SLANGC comes from shaders/CMakeLists.txt.
set(OHAO_OPENPBR_KERNEL_CPP ${CMAKE_BINARY_DIR}/material/openpbr_kernel.cpp)
file(GLOB OHAO_MATERIAL_SLANG CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/material/*.slang)
add_custom_command(
    OUTPUT ${OHAO_OPENPBR_KERNEL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/material
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/openpbr_kernel.slang -I ${CMAKE_SOURCE_DIR}/shaders
            -target cpp -entry run -stage compute -o ${OHAO_OPENPBR_KERNEL_CPP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/openpbr_kernel.slang ${OHAO_MATERIAL_SLANG}
    COMMENT "slangc: OpenPBR's kernel to C++"
    VERBATIM)

add_library(ohao_openpbr_cpu STATIC openpbr_cpu.cpp ${OHAO_OPENPBR_KERNEL_CPP})
set_source_files_properties(${OHAO_OPENPBR_KERNEL_CPP} PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ohao_openpbr_cpu PUBLIC cxx_std_20)
target_include_directories(ohao_openpbr_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/material)

# The energy tables: build/Release/openpbr_tables ohao/render/material/openpbr_tables.hpp.
add_executable(openpbr_tables openpbr_tables.cpp)
target_link_libraries(openpbr_tables PRIVATE ohao_openpbr_cpu)
