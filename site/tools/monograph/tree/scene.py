"""Tree units: 02 · Scene."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'scene',
    "title": '02 · Scene',
    "hub": 'scene.html',
    "children": [
        page(
            'world',
            'ECS world & hierarchy',
            'EnTT registry, ordered systems, Parent/Children with cycle rejection, subtree destroy.',
            files=['ohao/ecs/world.hpp', 'ohao/ecs/world.cpp', 'ohao/ecs/components.hpp'],
            design=['World::create stamps Name, Transform, Children, TransformDirty and a never-recycled CreationOrder.', 'Systems run in insertion order: transform, then render_extract, then anything appended.', 'setParent rejects cycles and keeps the local transform; destroy takes the whole subtree.'],
            workflow=['create(name, local)', 'setParent / setTransform (patch)', 'addSystem / addSystemAfter', 'update(dt) runs the systems in order', 'destroy(subtree)'],
        ),
        page(
            'transform-system',
            'Transform propagation',
            'Parent-first world matrices, prevWorld motion history, the patch rule and verifyTransforms.',
            files=['ohao/ecs/transform_system.hpp', 'ohao/ecs/transform_system.cpp'],
            design=['Roots are dirty entities with no dirty ancestor; each subtree is walked parent-first from a stack.', 'A move tags MovedLastFrame; the next update settles prevWorld = world so motion vectors return to zero.', 'verifyTransforms counts entities whose world disagrees with local x parent world while untagged.'],
            workflow=['Settle last frame movers', 'Collect dirty roots', 'Recompute subtrees parent-first', 'Tag RenderDirty, clear TransformDirty'],
        ),
        page(
            'render-extract',
            'Render extract',
            'Registry to RenderScene: slots in creation order, dirty uploads, lights, stale mesh handles.',
            files=['ohao/ecs/render_extract.hpp', 'ohao/ecs/render_extract.cpp', 'ohao/ecs/spawn.hpp', 'ohao/ecs/spawn.cpp', 'ohao/ecs/framing.cpp'],
            design=['Work arrives as RenderDirty plus EnTT construct/update/destroy signals; nothing is polled.', 'The extract owns RenderSlot and only touches slots it handed out.', 'Lights are rebuilt whole and ordered by creation sequence.'],
            workflow=['Release queued slots', 'Update owned slots / collect new renderables', 'Create instances sorted by CreationOrder', 'Clear RenderDirty', 'Rebuild lights if touched'],
        ),
        page(
            'render-scene',
            'RenderScene',
            'Mesh arenas with one BLAS per mesh, material/instance/light tables, per-slot staging, retire queue, TLAS build vs refit.',
            files=['ohao/render/scene/render_scene.hpp', 'ohao/render/scene/render_scene.cpp', 'ohao/render/scene/mesh_registry.cpp', 'ohao/render/scene/material_table.cpp', 'ohao/render/scene/instance_table.cpp', 'ohao/render/scene/light_table.cpp', 'ohao/render/scene/model_upload.cpp', 'ohao/render/scene/render_scene_types.hpp'],
            design=['No device-local table is written from the host: every write is a recorded copy from the frame slot staging buffer.', 'Replaced buffers wait MAX_FRAMES_IN_FLIGHT frames in a retire queue.', 'Structural instance changes rebuild the TLAS; transform-only changes refit it.'],
            workflow=['registerMesh / registerMaterial at load time', 'create/update/destroyInstance, setLights', 'recordUpdates: staged copies, then TLAS build or refit', 'onFrameRetired after the fence wait'],
        ),
        page(
            'loaders',
            'Asset loaders',
            'Assimp (GLB/GLTF, node transforms applied), ufbx + Assimp (FBX), native OBJ, procedural primitives.',
            files=['ohao/scene/asset/model_loader.hpp', 'ohao/scene/asset/model_loader.cpp', 'ohao/scene/asset/model_fbx.cpp', 'ohao/scene/asset/model.cpp', 'ohao/scene/asset/primitive_mesh_generator.hpp'],
            workflow=['Detect format by extension', 'Parse meshes, materials, textures into a Model, walking the node tree', 'Repair normals', 'Fill the per-material arrays + materialPerTriangle', 'render::uploadModel turns the Model into mesh, materials, textures'],
        ),
    ],
}
