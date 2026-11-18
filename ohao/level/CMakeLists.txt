# The level module (level/module.hpp): a Level's glTF file expanded into entities (level_system: the `level` sync
# system), its nodes' extras read as hints (level_hints), its components, level.info and level.reload. A file's
# structure is the cooker's reader (cook/gltf_structure.hpp).
ohao_add_module(level DEPENDS ecs visual physics audio vfx LINKS ohao_cook ohao_reflect ohao_unordered_dense)
