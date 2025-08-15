# external/cmake/imgui.cmake
macro(add_imgui_library)
    add_library(imgui STATIC
        ${IMGUI_PATH}/imgui.cpp
        ${IMGUI_PATH}/imgui_demo.cpp
        ${IMGUI_PATH}/imgui_draw.cpp
        ${IMGUI_PATH}/imgui_tables.cpp
        ${IMGUI_PATH}/imgui_widgets.cpp
        ${IMGUI_PATH}/backends/imgui_impl_glfw.cpp
        ${IMGUI_PATH}/backends/imgui_impl_vulkan.cpp
    )

    target_include_directories(imgui PUBLIC
        ${IMGUI_PATH}
        ${IMGUI_PATH}/backends
    )

    find_package(Vulkan REQUIRED)
    # GLFW is already available from our external/cmake/glfw.cmake

    target_link_libraries(imgui PUBLIC
        Vulkan::Vulkan
        glfw # This now refers to our downloaded and built GLFW
    )

    target_compile_definitions(imgui PUBLIC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    )

    target_compile_features(imgui PUBLIC cxx_std_17)

    # Optional: Add ImGui stdlib features
    add_library(imgui_stdlib STATIC
        ${IMGUI_PATH}/misc/cpp/imgui_stdlib.cpp
    )

    target_include_directories(imgui_stdlib PUBLIC
        ${IMGUI_PATH}
        ${IMGUI_PATH}/misc/cpp
    )

    target_link_libraries(imgui_stdlib PUBLIC imgui)
endmacro()
