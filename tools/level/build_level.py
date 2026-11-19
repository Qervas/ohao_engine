"""Builds a level's .glb from its layout file, in Blender (docs/specs/2026-11-18-tidewall-design.md, section 6).

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup --python tools/level/build_level.py -- LAYOUT.json OUT.glb

The layout file is the source: its `pieces`, in order, each a kind of the kit (tools/level/kit) and its parameters, in
the engine's frame (metres, Y up). The kit sets every piece's level hints. The same layout gives the same bytes with
Blender 5.3.0, the version pinned here (another version is refused: its exporter may write other bytes).
"""
import json
import os
import sys
import time

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit  # noqa: E402

BLENDER = '5.3.0'


def build(layout):
    kit.reset()
    names = set()
    for i, piece in enumerate(layout['pieces']):
        kind, name = piece.get('kind'), piece.get('name')
        if kind not in kit.PIECES:
            raise SystemExit(f"layout: pieces[{i}]: unknown kind '{kind}' (the kit's: {', '.join(sorted(kit.PIECES))})")
        if not name or name in names:
            raise SystemExit(f"layout: pieces[{i}]: a name is required and must be unique ('{name}')")
        names.add(name)
        kit.PIECES[kind](piece)


def export(path):
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', export_extras=True, export_yup=True,
                              export_apply=True, export_cameras=True, export_lights=False,
                              export_import_convert_lighting_mode='SPEC')


if __name__ == '__main__':
    if bpy.app.version_string.split()[0] != BLENDER:
        raise SystemExit(f'build_level.py: Blender {BLENDER} is pinned; this is {bpy.app.version_string}')
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    if len(argv) != 2:
        raise SystemExit('usage: blender -b --factory-startup --python tools/level/build_level.py -- LAYOUT.json OUT.glb')
    t0 = time.perf_counter()
    with open(argv[0], encoding='utf-8') as f:
        layout = json.load(f)
    build(layout)
    export(argv[1])
    print(f"build_level: {argv[0]} -> {argv[1]}: {len(layout['pieces'])} pieces, {len(bpy.data.objects)} objects, "
          f"{time.perf_counter() - t0:.2f} s")
