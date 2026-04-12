file(GLOB_RECURSE PHYSICS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE PHYSICS_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_physics STATIC ${PHYSICS_SOURCES} ${PHYSICS_HEADERS})

target_include_directories(ohao_physics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(ohao_physics PUBLIC GLM_ENABLE_EXPERIMENTAL)

target_link_libraries(ohao_physics PUBLIC
    glm
    nlohmann_json::nlohmann_json
)

# Link Jolt Physics backend (modular - builds even if Jolt unavailable)
if(TARGET Jolt)
    target_link_libraries(ohao_physics PUBLIC Jolt)
    target_compile_definitions(ohao_physics PUBLIC OHAO_HAS_JOLT=1)
    message(STATUS "ohao_physics: Jolt Physics backend enabled")
else()
    target_compile_definitions(ohao_physics PUBLIC OHAO_HAS_JOLT=0)
    message(STATUS "ohao_physics: Jolt Physics NOT available, using null backend")
endif()
