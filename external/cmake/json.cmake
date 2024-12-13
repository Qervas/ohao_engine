include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# Check if json has already been downloaded
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    message(STATUS "Downloading nlohmann/json...")
    # Configure options for json
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    set(JSON_MultipleHeaders OFF CACHE INTERNAL "")

    FetchContent_MakeAvailable(json)
    message(STATUS "Download completed: nlohmann/json")
endif()

FetchContent_MakeAvailable(json)
