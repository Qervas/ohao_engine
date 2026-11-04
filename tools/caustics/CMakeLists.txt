# Caustics' manifold connections on the CPU (docs/plans/2026-11-04-caustics-step2a.md, T2): shaders/rt/manifold.slang
# compiled to C++ by slangc through manifold_kernel.slang, for tests/renderer/manifold_tests. SLANGC comes from
# shaders/CMakeLists.txt.
set(OHAO_MANIFOLD_KERNEL_CPP ${CMAKE_BINARY_DIR}/caustics/manifold_kernel.cpp)
add_custom_command(
    OUTPUT ${OHAO_MANIFOLD_KERNEL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/caustics
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/manifold_kernel.slang -I ${CMAKE_SOURCE_DIR}/shaders
            -target cpp -entry run -stage compute -o ${OHAO_MANIFOLD_KERNEL_CPP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifold_kernel.slang ${CMAKE_SOURCE_DIR}/shaders/rt/manifold.slang
    COMMENT "slangc: the manifold connections to C++"
    VERBATIM)

add_library(ohao_manifold_cpu STATIC manifold_cpu.cpp ${OHAO_MANIFOLD_KERNEL_CPP})
set_source_files_properties(${OHAO_MANIFOLD_KERNEL_CPP} PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ohao_manifold_cpu PUBLIC cxx_std_20)
target_include_directories(ohao_manifold_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/caustics)
