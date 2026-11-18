# The stable headers every engine unit parses (the standard library's, EnTT's registry, glm, unordered_dense),
# precompiled once per target: a unit then parses only the engine's own headers and its code. Never an engine header:
# an edit to one would rebuild the precompiled header before every unit of the target, serially.
#
#   ohao_precompile(<target>)
#
# ohao_add_module calls it for every module; ohao_precompile_directories(<dir>...) for the C++ libraries and programs
# of the given directories (the API, the hosts, the tools and benches).
set(OHAO_PCH_HEADERS
    <algorithm> <array> <chrono> <cmath> <cstdint> <cstring> <deque> <functional> <map> <memory> <mutex> <optional>
    <span> <string> <string_view> <unordered_map> <utility> <vector>
    <ankerl/unordered_dense.h> <entt/entity/registry.hpp> <glm/glm.hpp> <glm/gtc/matrix_transform.hpp>
    <glm/gtc/quaternion.hpp>)

function(ohao_precompile target)
    target_precompile_headers(${target} PRIVATE ${OHAO_PCH_HEADERS})
endfunction()

function(ohao_precompile_directories)
    foreach(dir ${ARGN})
        get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t ${targets})
            get_target_property(type ${t} TYPE)
            # A target that does not see the World's headers (job_bench: the core's job pool alone) has none of them.
            if((type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "EXECUTABLE") AND NOT t STREQUAL "job_bench")
                ohao_precompile(${t})
            endif()
        endforeach()
    endforeach()
endfunction()
