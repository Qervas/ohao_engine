# external/CMakeLists.txt
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinyfiledialogs.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/json.cmake)

# Set the path to the ImGui submodule
set(IMGUI_PATH ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

# Check if ImGui exists
if(NOT EXISTS ${IMGUI_PATH}/imgui.h)
    message(FATAL_ERROR "ImGui submodule not found. Please run: git submodule update --init --recursive")
endif()

# Check and switch to docking branch if needed
execute_process(
    COMMAND git -C ${IMGUI_PATH} rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE IMGUI_CURRENT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT ${IMGUI_CURRENT_BRANCH} STREQUAL "docking")
    message(STATUS "Switching ImGui to docking branch...")
    execute_process(
        COMMAND git -C ${IMGUI_PATH} fetch origin docking
        COMMAND git -C ${IMGUI_PATH} checkout docking
        RESULT_VARIABLE IMGUI_CHECKOUT_RESULT
    )
    if(NOT ${IMGUI_CHECKOUT_RESULT} EQUAL 0)
        message(FATAL_ERROR "Failed to switch ImGui to docking branch. Please check the submodule status.")
    endif()
endif()

# Add ImGui library
add_imgui_library()
