# GLFW — windowing + input for the interactive viewer.
#
# Built from source rather than linked against a prebuilt binary. The project
# forces the static CRT (CMAKE_MSVC_RUNTIME_LIBRARY in the root CMakeLists),
# and a glfw3.lib compiled against the dynamic CRT fails to link:
#
#   glfw3.lib(context.c.obj) : error LNK2019: unresolved external symbol
#       __imp___stdio_common_vsscanf
#   glfw3.lib(input.c.obj)   : error LNK2019: unresolved external symbol
#       __imp_strspn
#
# Building it here means it inherits CMAKE_MSVC_RUNTIME_LIBRARY and matches
# whatever the rest of the project uses. It also removes the previous
# hardcoded scoop path, so the viewer builds on machines without scoop.
include(FetchContent)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(GLFW_BUILD_DOCS OFF CACHE INTERNAL "")
set(GLFW_BUILD_TESTS OFF CACHE INTERNAL "")
set(GLFW_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(GLFW_INSTALL OFF CACHE INTERNAL "")
# Belt and braces alongside CMAKE_MSVC_RUNTIME_LIBRARY: GLFW's own switch for
# selecting the DLL runtime. OFF keeps it on the static CRT.
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(glfw)
