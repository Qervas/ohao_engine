file(GLOB_RECURSE ACTOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ACTOR_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")

add_library(ohao_actor STATIC ${ACTOR_SOURCES} ${ACTOR_HEADERS})

target_include_directories(ohao_actor PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_actor PUBLIC
    glm
    ${Vulkan_LIBRARIES}
) 