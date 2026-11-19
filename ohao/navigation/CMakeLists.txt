# The navigation module (navigation/module.hpp): the navmesh of the World's static collision, the agents that walk it
# and the obstacles that block it, on Recast/Detour (external/cmake/recast.cmake), which is private to it (private/).
ohao_add_module(navigation DEPENDS ecs visual physics LINKS ohao_core glm)
target_link_libraries(ohao_navigation PRIVATE ohao_recast)
