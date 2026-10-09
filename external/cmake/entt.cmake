# EnTT -- header-only ECS. Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.2
)
FetchContent_GetProperties(entt)
if(NOT entt_POPULATED)
    FetchContent_Populate(entt)
endif()
if(NOT TARGET EnTT::EnTT)
    add_library(EnTT INTERFACE)
    add_library(EnTT::EnTT ALIAS EnTT)
    target_include_directories(EnTT INTERFACE ${entt_SOURCE_DIR}/src)
    target_compile_features(EnTT INTERFACE cxx_std_17)
endif()
message(STATUS "EnTT included at: ${entt_SOURCE_DIR}")
