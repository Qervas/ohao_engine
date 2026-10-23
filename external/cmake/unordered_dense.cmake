# ankerl::unordered_dense -- header-only open-addressing hash map (EntityId -> entity
# index in ohao_ecs). Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.4.0
)
FetchContent_GetProperties(unordered_dense)
if(NOT unordered_dense_POPULATED)
    FetchContent_Populate(unordered_dense)
endif()
if(NOT TARGET ohao_unordered_dense)
    add_library(ohao_unordered_dense INTERFACE)
    target_include_directories(ohao_unordered_dense INTERFACE ${unordered_dense_SOURCE_DIR}/include)
endif()
message(STATUS "unordered_dense included at: ${unordered_dense_SOURCE_DIR}")
