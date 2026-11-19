# The nav_show module (nav_show/module.hpp): the navmesh drawn into frames, nav.show (one feature over navigation and
# world_render, neither of which knows it).
ohao_add_module(nav_show DEPENDS ecs navigation world_render LINKS glm)
