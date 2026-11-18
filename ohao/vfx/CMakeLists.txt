# The vfx module (vfx/module.hpp): the `vfx` system that writes the step's spawn records, its components, and below
# it, in effects/, the effects library (ohao_vfx_effects) the renderer draws with too.
add_subdirectory(effects)
ohao_add_module(vfx DEPENDS ecs visual physics LINKS ohao_vfx_effects
    SOURCES components.hpp module.cpp module.hpp storage.cpp types.cpp vfx.cpp vfx.hpp)
