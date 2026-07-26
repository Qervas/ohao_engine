"""Tree units: 02 · Scene."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'scene',
    "title": '02 · Scene',
    "hub": 'scene.html',
    "children": [
        page(
            'actors',
            'Actors & hierarchy',
            'Actor as SceneObject with parent/children and lifecycle.',
            files=['ohao/scene/actor/actor.hpp', 'ohao/scene/actor/actor.cpp', 'ohao/scene/scene_object.hpp'],
            workflow=['construct → addComponent → initialize → start → update → destroy', 'Transform always present'],
        ),
        page(
            'components',
            'Components',
            'Mesh, Material, Light, Transform, factory packs.',
            files=['ohao/scene/component/component.hpp', 'ohao/scene/component/mesh_component.hpp', 'ohao/scene/component/material_component.hpp', 'ohao/scene/component/light_component.hpp', 'ohao/scene/component/transform_component.hpp', 'ohao/scene/component/component_factory.hpp', 'ohao/scene/component/component_pack.hpp'],
            design=['Composition over inheritance: renderer iterates MeshComponents, not class hierarchies.'],
        ),
        page(
            'loaders',
            'Asset loaders',
            'GLTF (tinygltf), FBX (Assimp), OBJ custom, primitives.',
            files=['ohao/scene/asset/model_loader.hpp', 'ohao/scene/asset/model_gltf.cpp', 'ohao/scene/asset/model_fbx.cpp', 'ohao/scene/asset/model.cpp', 'ohao/scene/asset/primitive_mesh_generator.hpp'],
            workflow=['Detect format', 'Parse meshes/materials/textures', 'Up-axis fix', 'Fill Mesh + Material components', 'No Vulkan calls'],
        ),
        page(
            'scene-api',
            'Scene API',
            'createActor, lookup, tags, physics world ownership, serialize hooks.',
            files=['ohao/scene/scene.hpp', 'ohao/scene/scene.cpp', 'ohao/scene/default_scene_factory.hpp', 'ohao/scene/default_scene_factory.cpp', 'ohao/scene/scene_module.hpp'],
            design=['Scene owns Actor maps (by id and name), mesh/physics registries, and optional PhysicsWorld.', 'string_view at query boundaries; owned strings in maps. Tags via findActorsByTag.', 'scene_module.hpp is the module umbrella include for consumers.'],
            workflow=['createActor / createActorWithComponents(primitive)', 'addActor / removeActor', 'update(dt) walks actors → components', 'Physics world step if present'],
        ),
        page(
            'transform',
            'Transform math',
            'TRS composition and world matrix used by upload/TLAS.',
            files=['ohao/scene/transform.hpp', 'ohao/scene/transform.cpp'],
            design=['Local TRS → local matrix; parent chain → world matrix.', 'Upload and TLAS instance transforms read world matrices only — dirty flags matter.'],
        ),
    ],
}
