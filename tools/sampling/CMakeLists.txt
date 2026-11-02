# The path tracer's sampler on the CPU (docs/plans/2026-10-30-frontier-f1.md, T2):
# shaders/sampling/sobol.slang compiled to C++ by slangc through sobol_kernel.slang, for
# tests/renderer/sobol_test. SLANGC comes from shaders/CMakeLists.txt.
set(OHAO_SOBOL_KERNEL_CPP ${CMAKE_BINARY_DIR}/sampling/sobol_kernel.cpp)
file(GLOB OHAO_SAMPLING_SLANG CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/sampling/*.slang)
add_custom_command(
    OUTPUT ${OHAO_SOBOL_KERNEL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sampling
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/sobol_kernel.slang -I ${CMAKE_SOURCE_DIR}/shaders
            -target cpp -entry run -stage compute -o ${OHAO_SOBOL_KERNEL_CPP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sobol_kernel.slang ${OHAO_SAMPLING_SLANG}
    COMMENT "slangc: the path tracer's sampler to C++"
    VERBATIM)

add_library(ohao_sobol_cpu STATIC sobol_cpu.cpp ${OHAO_SOBOL_KERNEL_CPP})
set_source_files_properties(${OHAO_SOBOL_KERNEL_CPP} PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ohao_sobol_cpu PUBLIC cxx_std_20)
target_include_directories(ohao_sobol_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/sampling)

# The path guiding's lobes on the CPU (sub-project 23): shaders/sampling/vmf.slang through
# vmf_kernel.slang, for tests/renderer/vmf_test.
set(OHAO_VMF_KERNEL_CPP ${CMAKE_BINARY_DIR}/sampling/vmf_kernel.cpp)
add_custom_command(
    OUTPUT ${OHAO_VMF_KERNEL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sampling
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/vmf_kernel.slang -I ${CMAKE_SOURCE_DIR}/shaders
            -target cpp -entry run -stage compute -o ${OHAO_VMF_KERNEL_CPP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vmf_kernel.slang ${OHAO_SAMPLING_SLANG}
    COMMENT "slangc: the path guiding's lobes to C++"
    VERBATIM)

add_library(ohao_vmf_cpu STATIC vmf_cpu.cpp ${OHAO_VMF_KERNEL_CPP})
set_source_files_properties(${OHAO_VMF_KERNEL_CPP} PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ohao_vmf_cpu PUBLIC cxx_std_20)
target_include_directories(ohao_vmf_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/sampling)
