file(GLOB_RECURSE CORE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(CORE_SOURCES)
    add_library(ohao_core STATIC ${CORE_HEADERS} ${CORE_SOURCES})
else()
    add_library(ohao_core INTERFACE)
endif()

target_include_directories(ohao_core
    PUBLIC ${CMAKE_SOURCE_DIR}/ohao
)

if(TARGET ohao_core)
    # Propagate C++20 to all dependents (scene, gpu, examples, tests).
    target_compile_features(ohao_core PUBLIC cxx_std_20)
endif()
