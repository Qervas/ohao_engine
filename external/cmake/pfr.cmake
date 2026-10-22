# Boost.PFR (standalone, header-only) -- TEST-ONLY: counts aggregate fields so the
# reflection completeness test catches members that were neither registered nor skipped.
# Never link it into an engine library. Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG 2.2.0
)
FetchContent_GetProperties(pfr)
if(NOT pfr_POPULATED)
    FetchContent_Populate(pfr)
endif()
if(NOT TARGET pfr_test_only)
    add_library(pfr_test_only INTERFACE)
    target_include_directories(pfr_test_only INTERFACE ${pfr_SOURCE_DIR}/include)
endif()
message(STATUS "Boost.PFR (test-only) at: ${pfr_SOURCE_DIR}")
