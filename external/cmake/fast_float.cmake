# fast_float -- header-only, correctly rounded decimal -> float parsing (the algorithm
# behind GCC's std::from_chars). Used privately by ohao_reflect's decoder: ~4x faster
# than MSVC's from_chars<float>, same results. Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    fast_float
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG v6.1.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(fast_float)
if(NOT fast_float_POPULATED)
    FetchContent_Populate(fast_float)
endif()
if(NOT TARGET ohao_fast_float)
    add_library(ohao_fast_float INTERFACE)
    target_include_directories(ohao_fast_float INTERFACE ${fast_float_SOURCE_DIR}/include)
endif()
message(STATUS "fast_float included at: ${fast_float_SOURCE_DIR}")
