---
module: scene
id: loaders
title: Asset loaders
---

## What

Asset loaders parse GLTF (tinygltf), FBX (Assimp), OBJ, and procedural primitives into Mesh/Material components — never Vulkan objects.

## How

Detect format → parse meshes/materials/textures → up-axis fix → fill components.

model_loader.hpp is the facade; model_gltf.cpp / model_fbx.cpp / model.cpp specialize.

primitive_mesh_generator builds cornell-style boxes and spheres without files.


- Detect format
- Parse meshes/materials/textures
- Up-axis fix
- Fill Mesh + Material components
- No Vulkan calls

## Why

Keeping loaders Vulkan-free means offline tools and tests can load scenes without a GPU.

## Contracts

- No VkDevice in scene/asset
- Textures referenced by path/index for later bindless upload

## Notes

model_loader.hpp: defines `class ModelLoader`.

FBX loader via ufbx — handles all FBX rotation orders, pivots, and pre-rotations correctly.

Falls back to Assimp for non-FBX formats (Collada, etc.)

primitive_mesh_generator.hpp: defines `class PrimitiveMeshGenerator`.

## Notes

Source map:
- `ohao/scene/asset/model_loader.hpp`
- `ohao/scene/asset/model_gltf.cpp`
- `ohao/scene/asset/model_fbx.cpp`
- `ohao/scene/asset/model.cpp`
- `ohao/scene/asset/primitive_mesh_generator.hpp`
