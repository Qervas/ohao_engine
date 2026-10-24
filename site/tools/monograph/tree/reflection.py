"""Tree units: 12 · Reflection & agent API (sub-project 3)."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'reflection',
    "title": '12 · Reflection & agent API',
    "hub": None,
    "children": [
        page(
            'registry',
            'Type registry & schema',
            'Registered types with docs, units and limits; the lint; the catalogue, the scene JSON Schema and schemaHash.',
            files=['ohao/reflect/registry.hpp', 'ohao/reflect/registry.cpp', 'ohao/reflect/type_info.hpp',
                   'ohao/reflect/schema.cpp', 'ohao/ecs/scene_types.cpp', 'ohao/ecs/scene_types.hpp'],
        ),
        page(
            'scene-files',
            'Canonical scene files',
            'Stable entity ids, defaults omitted, the one pretty layout, per-type writers and shortest floats.',
            files=['ohao/ecs/scene_io.hpp', 'ohao/ecs/scene_io.cpp', 'ohao/ecs/scene_writers.cpp',
                   'ohao/reflect/json_layout.cpp', 'ohao/reflect/json_writer.cpp', 'ohao/ecs/world.cpp'],
        ),
        page(
            'load-validate',
            'Loading & validation',
            'Two-phase load, the streaming fast path and the tree decoder, structured diagnostics, versions and upgrades.',
            files=['ohao/ecs/scene_io.cpp', 'ohao/reflect/decode.hpp', 'ohao/reflect/decode.cpp',
                   'ohao/reflect/json_cursor.hpp', 'ohao/reflect/codec.hpp'],
        ),
        page(
            'snapshots',
            'Snapshots & restore',
            'Columnar captures of every saved component, and a restore that writes only what differs.',
            files=['ohao/ecs/snapshot.hpp', 'ohao/ecs/snapshot.cpp', 'ohao/ecs/snapshot_column.hpp',
                   'ohao/ecs/scene_types.hpp'],
        ),
        page(
            'api',
            'Session, commands & queries',
            'The method table, atomic command batches with inverses and rollback, $k references, queries and summaries.',
            files=['ohao/api/session.hpp', 'ohao/api/session.cpp', 'ohao/api/commands.cpp', 'ohao/api/internal.hpp'],
        ),
        page(
            'instantiate',
            'Instantiate & the scene view',
            'MeshSource to MeshRenderer through a backend seam, shared meshes, the asset resolver, Camera and Environment.',
            files=['ohao/ecs/instantiate.hpp', 'ohao/ecs/instantiate.cpp', 'ohao/ecs/asset_resolver.hpp',
                   'ohao/ecs/scene_view.hpp', 'ohao/ecs/scene_view.cpp', 'ohao/runtime/scene_view_apply.hpp'],
        ),
        page(
            'cli',
            'The ohao CLI & render host',
            'One JSON document on stdout, exit classes, scripts as RPC requests, render.capture through a host.',
            files=['tools/ohao/main.cpp', 'tools/ohao/render_host.hpp', 'tools/ohao/render_host.cpp'],
        ),
    ],
}
