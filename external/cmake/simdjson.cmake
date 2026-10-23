# simdjson -- SIMD JSON parser (On-Demand API), used by ohao_ecs to read scene files.
# Built from its single-header amalgamation. Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.10.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(simdjson)
if(NOT simdjson_POPULATED)
    FetchContent_Populate(simdjson)
endif()
if(NOT TARGET ohao_simdjson)
    add_library(ohao_simdjson STATIC ${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp)
    target_include_directories(ohao_simdjson PUBLIC ${simdjson_SOURCE_DIR}/singleheader)
    target_compile_features(ohao_simdjson PUBLIC cxx_std_17)
endif()
message(STATUS "simdjson included at: ${simdjson_SOURCE_DIR}")
