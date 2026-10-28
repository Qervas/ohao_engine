"""Builds the player's first-person arms and rifle (sub-project 10 step 3): writes
assets/characters/fps_arms/fps_arms.glb. Ours (MIT), made in Blender 5.2.2 from nothing but the
numbers below, run in background mode:

    blender -b --factory-startup --python tools/anim/make_fps_arms.py -- <out.glb>

Deterministic: an empty factory scene, every object, bone, key and material made here in a fixed
order, linear keys, no timestamps; two runs write the same bytes (tests/level/check_level_assets.py
rebuilds it and compares, skipping without Blender 5.2.2).

The rig is authored in the view's space (the head's): Blender +Y forward, +Z up, +X right; the glTF
export turns it into the engine's -Z forward, +Y up. Its root sits at the eye.

Bones: root; gun (the rifle and the right arm, rigid); mag (the magazine, child of gun); upper_L
(the left upper arm, pivot at the shoulder, child of gun) and fore_L (the left forearm and hand,
pivot at the elbow). Every vertex is weighted wholly to one bone.

Clips (60 fps, linear keys): rest (the bind pose, additive zero), idle (2 s, a slow breath), fire
(0.15 s, the rifle punched back and up), reload (2.0 s: the magazine out at 0.6 s, a new one in at
1.5 s; the engine scales it so mag_in lands on the weapon's reload time), ads_in (0.2 s, from the
hip to the sights), ads_hold (the sights at the eye) and sprint (0.6 s loop, the rifle turned down
and across).
"""
import math
import sys

import bpy
import bmesh
from mathutils import Matrix, Quaternion, Vector

FPS = 60
GRIP = Vector((0.15, 0.32, -0.20))          # the right hand's grip on the rifle, at the hip
MUZZLE = GRIP + Vector((0.0, 0.52, 0.018))  # the barrel's tip
MAG = GRIP + Vector((0.0, 0.085, -0.03))    # the magazine's pivot (its top, in the receiver)
SHOULDER_R = Vector((0.30, -0.25, -0.45))
ELBOW_R = Vector((0.24, -0.02, -0.40))
WRIST_R = GRIP + Vector((0.025, -0.07, -0.07))
SHOULDER_L = Vector((-0.24, -0.20, -0.45))
ELBOW_L = Vector((-0.12, 0.12, -0.36))
WRIST_L = GRIP + Vector((-0.035, 0.17, -0.075))


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    scene.render.fps_base = 1.0
    scene.frame_start = 0


def material(name, rgb, roughness, metallic=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (rgb[0], rgb[1], rgb[2], 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return m


def add_box(bm, lo, hi, frame=Matrix.Identity(4)):
    """An axis-aligned box in `frame`'s space; returns its new vertices."""
    xs, ys, zs = (lo[0], hi[0]), (lo[1], hi[1]), (lo[2], hi[2])
    vs = [bm.verts.new(frame @ Vector((xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]))) for i in range(8)]
    for f in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        bm.faces.new([vs[k] for k in f])
    return vs


def add_tube(bm, a, b, ra, rb, sides=8):
    """A capped tube from a (radius ra) to b (radius rb)."""
    axis = (b - a).normalized()
    helper = Vector((0.0, 0.0, 1.0)) if abs(axis.z) < 0.9 else Vector((1.0, 0.0, 0.0))
    u = axis.cross(helper).normalized()
    v = axis.cross(u).normalized()
    ring_a, ring_b = [], []
    for i in range(sides):
        ang = 2.0 * math.pi * i / sides
        d = u * math.cos(ang) + v * math.sin(ang)
        ring_a.append(bm.verts.new(a + d * ra))
        ring_b.append(bm.verts.new(b + d * rb))
    for i in range(sides):
        j = (i + 1) % sides
        bm.faces.new([ring_a[i], ring_a[j], ring_b[j], ring_b[i]])
    bm.faces.new(list(reversed(ring_a)))
    bm.faces.new(ring_b)
    return ring_a + ring_b


def part(name, mat_index, bone, build, parts):
    parts.append((name, mat_index, bone, build))


def build_mesh(arm_obj):
    mats = [
        material("Sleeve", (0.13, 0.15, 0.10), 0.9),
        material("Glove", (0.035, 0.035, 0.035), 0.7),
        material("Gunmetal", (0.09, 0.09, 0.10), 0.45, 0.6),
        material("Polymer", (0.11, 0.10, 0.09), 0.6),
    ]
    g = GRIP
    parts = []
    # The rifle (gun) and its magazine (mag).
    part("receiver", 2, "gun", lambda bm: add_box(bm, g + Vector((-0.022, -0.05, -0.03)), g + Vector((0.022, 0.22, 0.035))), parts)
    part("handguard", 3, "gun", lambda bm: add_box(bm, g + Vector((-0.026, 0.12, -0.012)), g + Vector((0.026, 0.33, 0.03))), parts)
    part("barrel", 2, "gun", lambda bm: add_tube(bm, g + Vector((0.0, 0.30, 0.018)), MUZZLE, 0.011, 0.010), parts)
    part("stock", 3, "gun", lambda bm: add_box(bm, g + Vector((-0.018, -0.24, -0.055)), g + Vector((0.018, -0.05, 0.022))), parts)
    part("grip", 3, "gun", lambda bm: add_box(bm, g + Vector((-0.014, -0.025, -0.10)), g + Vector((0.014, 0.02, -0.03))), parts)
    part("sight", 2, "gun", lambda bm: add_box(bm, g + Vector((-0.007, 0.0, 0.035)), g + Vector((0.007, 0.05, 0.06))), parts)
    part("magazine", 2, "mag", lambda bm: add_box(bm, MAG + Vector((-0.015, -0.025, -0.11)), MAG + Vector((0.015, 0.025, 0.0))), parts)
    # The right arm, rigid with the rifle.
    part("upper_R", 0, "gun", lambda bm: add_tube(bm, SHOULDER_R, ELBOW_R, 0.055, 0.045), parts)
    part("fore_R", 0, "gun", lambda bm: add_tube(bm, ELBOW_R, WRIST_R, 0.045, 0.034), parts)
    part("hand_R", 1, "gun", lambda bm: add_tube(bm, WRIST_R, g + Vector((0.0, 0.0, -0.045)), 0.036, 0.03, 6), parts)
    # The left arm: upper (pivot at the shoulder) and forearm with hand (pivot at the elbow).
    part("upper_L", 0, "upper_L", lambda bm: add_tube(bm, SHOULDER_L, ELBOW_L, 0.055, 0.045), parts)
    part("fore_L", 0, "fore_L", lambda bm: add_tube(bm, ELBOW_L, WRIST_L, 0.045, 0.034), parts)
    part("hand_L", 1, "fore_L", lambda bm: add_tube(bm, WRIST_L, g + Vector((-0.004, 0.22, -0.02)), 0.036, 0.03, 6), parts)

    mesh = bpy.data.meshes.new("Arms")
    for m in mats:
        mesh.materials.append(m)
    bm = bmesh.new()
    groups = []   # per vertex index: bone
    for name, mat_index, bone, build in parts:
        before = len(bm.verts)
        faces_before = len(bm.faces)
        build(bm)
        bm.verts.ensure_lookup_table()
        bm.faces.ensure_lookup_table()
        for f in bm.faces[faces_before:]:
            f.material_index = mat_index
        groups.extend([bone] * (len(bm.verts) - before))
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = False
    obj = bpy.data.objects.new("Arms", mesh)
    bpy.context.scene.collection.objects.link(obj)
    for bone in ("gun", "mag", "upper_L", "fore_L"):
        obj.vertex_groups.new(name=bone)
    for i, bone in enumerate(groups):
        obj.vertex_groups[bone].add([i], 1.0, 'REPLACE')
    obj.parent = arm_obj
    mod = obj.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm_obj
    return obj


def build_armature():
    data = bpy.data.armatures.new("ArmsRig")
    obj = bpy.data.objects.new("ArmsRig", data)
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='EDIT')
    eb = data.edit_bones

    def bone(name, head, tail, parent=None):
        b = eb.new(name)
        b.head, b.tail, b.roll = head, tail, 0.0
        b.use_deform = True
        b.use_connect = False
        if parent:
            b.parent = eb[parent]
        return b

    bone("root", Vector((0, 0, 0)), Vector((0, 0.05, 0)))
    bone("gun", GRIP, GRIP + Vector((0, 0.1, 0)), "root")
    bone("mag", MAG, MAG + Vector((0, 0, -0.08)), "gun")
    bone("upper_L", SHOULDER_L, ELBOW_L, "gun")
    bone("fore_L", ELBOW_L, WRIST_L, "upper_L")
    bpy.ops.object.mode_set(mode='OBJECT')
    return obj


def world_rot(arm_obj, bone_name, axis, deg):
    """A rotation about a view-space axis, as the bone's own (rest-relative) rotation."""
    rest = arm_obj.data.bones[bone_name].matrix_local.to_quaternion()
    q = Quaternion(Vector(axis), math.radians(deg))
    return rest.inverted() @ q @ rest


def world_loc(arm_obj, bone_name, offset):
    """A view-space offset, in the bone's rest frame."""
    rest = arm_obj.data.bones[bone_name].matrix_local.to_3x3()
    return rest.inverted() @ Vector(offset)


X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)


def clip(arm_obj, name, keys):
    """keys: {bone: [(frame, loc (view space), [(axis, deg), ...]), ...]}; linear interpolation."""
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm_obj.animation_data_create()
    arm_obj.animation_data.action = act
    for bone_name in sorted(keys):
        pb = arm_obj.pose.bones[bone_name]
        pb.rotation_mode = 'QUATERNION'
        for frame, loc, rots in keys[bone_name]:
            q = Quaternion((1, 0, 0, 0))
            for axis, deg in rots:
                q = world_rot(arm_obj, bone_name, axis, deg) @ q
            pb.location = world_loc(arm_obj, bone_name, loc)
            pb.rotation_quaternion = q
            pb.keyframe_insert("location", frame=frame)
            pb.keyframe_insert("rotation_quaternion", frame=frame)
    for slot_bag in action_fcurves(act):
        for kp in slot_bag.keyframe_points:
            kp.interpolation = 'LINEAR'
    arm_obj.animation_data.action = None
    for pb in arm_obj.pose.bones:
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)


def action_fcurves(act):
    if hasattr(act, "fcurves") and len(act.fcurves):
        return list(act.fcurves)
    out = []
    for layer in act.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                out.extend(bag.fcurves)
    return out


O = (0, 0, 0)


def build_clips(a):
    still = lambda frames: {"gun": [(f, O, []) for f in frames]}
    clip(a, "rest", still((0, 60)))
    clip(a, "idle", {"gun": [(0, O, []), (60, (0, 0, -0.004), [(X, 0.6)]), (120, O, [])]})
    clip(a, "fire", {"gun": [(0, O, []), (2, (0, -0.035, 0.004), [(X, 3.0)]), (9, O, [])]})
    hip_to_sights = (-0.15, -0.06, 0.145)
    clip(a, "ads_in", {"gun": [(0, O, []), (12, hip_to_sights, [])]})
    clip(a, "ads_hold", {"gun": [(0, hip_to_sights, []), (60, hip_to_sights, [])]})
    clip(a, "sprint", {"gun": [(0, (0.02, -0.02, -0.08), [(Z, 35), (Y, 20), (X, -12)]),
                                (18, (0.02, -0.02, -0.09), [(Z, 35), (Y, 20), (X, -14)]),
                                (36, (0.02, -0.02, -0.08), [(Z, 35), (Y, 20), (X, -12)])]})
    # Reload, 2 s (120 frames): the rifle rolls in and dips; the left hand leaves the handguard,
    # pulls the magazine (mag_out 0.6 s), drops out of view, brings a new one (mag_in 1.5 s), and
    # goes back. The graph scales it so 1.5 s lands on the weapon's reload time.
    tilt = [(Y, -25), (X, -8)]
    clip(a, "reload", {
        "gun": [(0, O, []), (18, (0.0, 0.0, -0.04), tilt), (100, (0.0, 0.0, -0.04), tilt), (120, O, [])],
        "upper_L": [(0, O, []), (24, O, [(X, -12)]), (36, O, [(X, -20)]), (54, O, [(X, -40)]), (72, O, [(X, -40)]),
                    (84, O, [(X, -18)]), (90, O, [(X, -16)]), (106, O, [(X, -5)]), (120, O, [])],
        "fore_L": [(0, O, []), (24, O, [(X, -25)]), (36, O, [(X, -45)]), (54, O, [(X, -60)]), (72, O, [(X, -60)]),
                   (84, O, [(X, -45)]), (90, O, [(X, -42)]), (106, O, [(X, -10)]), (120, O, [])],
        "mag": [(0, O, []), (34, O, []), (36, (0.0, 0.0, -0.03), []), (52, (0.02, -0.05, -0.45), [(X, -40)]),
                (60, (0.0, -0.05, -0.45), []), (78, (0.0, -0.03, -0.12), []), (88, (0.0, 0.0, -0.01), []),
                (90, O, []), (120, O, [])],
    })


def export(path):
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', export_yup=True, export_apply=False,
                              export_cameras=False, export_lights=False, export_extras=False,
                              export_skins=True, export_animations=True, export_animation_mode='ACTIONS',
                              export_force_sampling=False, export_optimize_animation_size=False,
                              export_def_bones=False, export_rest_position_armature=True,
                              export_import_convert_lighting_mode='SPEC')


if __name__ == '__main__':
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    reset()
    rig = build_armature()
    build_mesh(rig)
    build_clips(rig)
    export(argv[0] if argv else 'fps_arms.glb')
    print("muzzle (view space, Blender axes)", tuple(round(v, 4) for v in MUZZLE))
