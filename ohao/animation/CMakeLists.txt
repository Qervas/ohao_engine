# The animation module (animation/module.hpp): skeletons, clips, graphs and their evaluation, the glTF reader for skins
# and animations, the systems that step and pose them, their components, events and animation.info.
ohao_add_module(animation DEPENDS ecs visual LINKS ohao_core glm)
target_link_libraries(ohao_animation PRIVATE ohao_simdjson)
