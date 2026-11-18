#pragma once
// The level module, "level": a Level's glTF file expanded into entities by the `level` sync system
// (level/level_system.hpp), its nodes' extras read as hints (level/level_hints.hpp), its components
// (level/components.hpp), level.info and level.reload. A file's structure is the cooker's reader
// (cook/gltf_structure.hpp).
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& levelModule();
// Its registrations (level/types.cpp) and methods (level/methods.cpp).
void registerLevelTypes(reflect::TypeRegistry& types);
[[nodiscard]] std::span<const Method> levelMethods();

} // namespace ohao::ecs
