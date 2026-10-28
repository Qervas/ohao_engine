"""Builds assets/levels/blockout_room.glb, SP5's checked-in blockout level, in Blender.

    blender -b --factory-startup --python tools/level/blockout_room.py -- assets/levels/blockout_room.glb

A room the way a level designer blocks one out: a floor slab, four walls with a doorway, a
step, a ramp, a pillar, two crates, three spawn points and a camera. Every value is in
metres, Blender's Z up (the glTF exporter writes Y up). Each object carries its level
hints as custom properties, which the exporter writes as the node's glTF `extras`
(docs/specs/2026-10-26-level-pipeline-design.md, "Extras"): `col`, `body`, `mass`, `spawn`,
`surf`.
Geometry and materials are generated here, so the file is ours (MIT, as the repo) and
rebuilds from this script alone.
"""
import sys

import bmesh
import bpy


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, rgb, roughness):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Base Color'].default_value = (*rgb, 1.0)
    bsdf.inputs['Roughness'].default_value = roughness
    bsdf.inputs['Metallic'].default_value = 0.0
    return m


def mesh_object(name, verts, faces, mat, location=(0.0, 0.0, 0.0), **hints):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    vs = [bm.verts.new(v) for v in verts]
    for f in faces:
        bm.faces.new([vs[i] for i in f])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    ob.location = location
    for k, v in hints.items():
        ob[k] = v
    bpy.context.collection.objects.link(ob)
    return ob


BOX_FACES = [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]


def box(name, lo, hi, mat, **hints):
    """An axis-aligned box from `lo` to `hi` (world metres), its origin at its centre."""
    c = [(a + b) / 2 for a, b in zip(lo, hi)]
    h = [(b - a) / 2 for a, b in zip(lo, hi)]
    verts = [(-h[0], -h[1], -h[2]), (h[0], -h[1], -h[2]), (h[0], h[1], -h[2]), (-h[0], h[1], -h[2]),
             (-h[0], -h[1], h[2]), (h[0], -h[1], h[2]), (h[0], h[1], h[2]), (-h[0], h[1], h[2])]
    return mesh_object(name, verts, BOX_FACES, mat, location=c, **hints)


def ramp(name, lo, hi, mat, **hints):
    """A wedge rising along +Y from height lo.z at y = lo.y to hi.z at y = hi.y."""
    c = [(a + b) / 2 for a, b in zip(lo, hi)]
    h = [(b - a) / 2 for a, b in zip(lo, hi)]
    verts = [(-h[0], -h[1], -h[2]), (h[0], -h[1], -h[2]), (h[0], h[1], -h[2]), (-h[0], h[1], -h[2]),
             (-h[0], h[1], h[2]), (h[0], h[1], h[2])]
    faces = [(0, 1, 2, 3), (0, 3, 4), (1, 5, 2), (0, 4, 5, 1), (2, 5, 4, 3)]
    return mesh_object(name, verts, faces, mat, location=c, **hints)


def empty(name, location, yaw_deg=0.0, **hints):
    ob = bpy.data.objects.new(name, None)
    ob.empty_display_type = 'ARROWS'
    ob.location = location
    ob.rotation_euler = (0.0, 0.0, yaw_deg * 3.141592653589793 / 180.0)
    for k, v in hints.items():
        ob[k] = v
    bpy.context.collection.objects.link(ob)
    return ob


def build():
    clear()
    concrete = material('Concrete', (0.42, 0.42, 0.40), 0.9)
    plaster = material('Plaster', (0.70, 0.66, 0.58), 0.85)
    trim = material('Trim', (0.25, 0.30, 0.38), 0.6)
    wood = material('CrateWood', (0.55, 0.38, 0.22), 0.7)

    # The room: 8 x 6 m inside, walls 3 m high and 0.2 m thick, open to the sky.
    box('Floor', (-4.2, -3.2, -0.2), (4.2, 3.2, 0.0), concrete, col='box', surf='concrete')
    # Outdoor apron: the room floor is only 8 x 6 m; past the doorway there was no
    # collider, so walking out fell forever (velocity blew up). Same top height as Floor.
    box('Ground', (-40.0, -40.0, -0.2), (40.0, 40.0, 0.0), concrete, col='box', surf='concrete')
    box('WallNorth', (-4.2, 3.0, 0.0), (4.2, 3.2, 3.0), plaster, col='box')
    box('WallEast', (4.0, -3.0, 0.0), (4.2, 3.0, 3.0), plaster, col='box')
    box('WallWest', (-4.2, -3.0, 0.0), (-4.0, 3.0, 3.0), plaster, col='box')
    # The south wall has a 1.2 m doorway centred at x = 1.5.
    box('WallSouthA', (-4.2, -3.2, 0.0), (0.9, -3.0, 3.0), plaster, col='box')
    box('WallSouthB', (2.1, -3.2, 0.0), (4.2, -3.0, 3.0), plaster, col='box')
    box('WallSouthLintel', (0.9, -3.2, 2.2), (2.1, -3.0, 3.0), plaster, col='box')

    # A 0.3 m step and a ramp up to a 0.6 m platform: collision on steps and slopes.
    # The trim is metal underfoot (`surf`: the footsteps' surface, SP6 step 4).
    box('Step', (-3.0, 1.0, 0.0), (-1.5, 3.0, 0.3), trim, col='box', surf='metal')
    box('Platform', (-4.0, 1.0, 0.0), (-3.0, 3.0, 0.6), trim, col='box', surf='metal')
    ramp('Ramp', (2.0, -0.5, 0.0), (3.4, 2.5, 0.6), trim, col='mesh', surf='metal')
    box('Pillar', (-0.3, 0.2, 0.0), (0.3, 0.8, 3.0), trim, col='box', surf='metal')

    # Two crates: dynamic bodies for SP6 to simulate; SP5 builds their colliders.
    box('CrateA', (1.0, 1.4, 0.0), (1.6, 2.0, 0.6), wood, col='box', body='dynamic', mass=20.0, surf='wood')
    box('CrateB', (1.1, 1.5, 0.6), (1.5, 1.9, 1.0), wood, col='box', body='dynamic', mass=8.0, surf='wood')

    # Spawn points: an arrow empty each, facing along its local +Y (yaw about Z).
    empty('SpawnPlayer', (1.5, -2.2, 0.0), yaw_deg=0.0, spawn='player')
    empty('SpawnEnemyA', (-2.5, 2.0, 0.3), yaw_deg=180.0, spawn='enemy')
    empty('SpawnEnemyB', (3.0, 2.0, 0.5), yaw_deg=180.0, spawn='enemy')   # on the ramp: 0.6 * 2.5 / 3 m up

    # A camera at the doorway looking in, for captures (glTF camera node).
    cam = bpy.data.cameras.new('LevelCamera')
    cam.lens_unit = 'FOV'
    cam.angle = 70.0 * 3.141592653589793 / 180.0
    ob = bpy.data.objects.new('LevelCamera', cam)
    ob.location = (1.5, -5.5, 1.7)
    ob.rotation_euler = (1.4, 0.0, 0.0)   # about 80 degrees from straight down: level, a little down
    bpy.context.collection.objects.link(ob)


def export(path):
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', export_extras=True, export_yup=True,
                              export_apply=True, export_cameras=True, export_lights=False,
                              export_import_convert_lighting_mode='SPEC')


if __name__ == '__main__':
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    build()
    export(argv[0] if argv else 'blockout_room.glb')
