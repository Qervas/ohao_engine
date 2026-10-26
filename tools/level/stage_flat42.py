"""Stages Olivier's Flat42 props (Somnabulate) as glTF for OHAO, in Blender.

    blender -b --factory-startup --python tools/level/stage_flat42.py -- SRC_DIR OUT_DIR [--visual-only] Prop [Prop ...]

SRC_DIR is Somnabulate's `.../Assets/Olivier/Props/Flat42` (read only: nothing there is
written), OUT_DIR is where `<Prop>.glb` goes (`assets/local/flat42`, git-ignored: see
docs/specs/2026-10-26-level-pipeline-design.md, "Assets and staging"). Per prop:

- `SM_<Prop>.fbx` (LOD0; `_LOD1` is not staged) is imported. The FBX files are Z up, metres.
- Its material is rebuilt from `T_<Prop>_BC/N/ORM.png`: base colour; ORM's G and B as
  roughness and metallic and its R as occlusion (glTF's packing); the normal map with its
  green channel flipped, since Unreal's are DirectX (-Y) and glTF's are OpenGL (+Y).
- `UCX_SM_<Prop>_NN` meshes (Unreal's convex collision hulls) are kept, hidden from
  rendering, with extras `{"col": "convex", "vis": 0}`; the visual mesh gets `{"col": "none"}`
  when hulls exist, so the hulls are its collision.
- `SOCKET_<name>` empties (attach points) are kept with extras `{"socket": "<name>"}`.

`--visual-only` writes `<Prop>_visual.glb` instead, without hulls or sockets: what today's
whole-file `ModelAsset` path can draw, since it merges every mesh in a file and reads no
`extras` (level pipeline steps 2-3 make the full file load correctly).
"""
import os
import sys

import bpy
import numpy as np


def flipped_normal(src, out_dir, prop):
    """The DirectX normal map with green flipped, written next to the output (OpenGL convention)."""
    img = bpy.data.images.load(src)
    px = np.array(img.pixels[:], dtype=np.float32).reshape(-1, 4)
    px[:, 1] = 1.0 - px[:, 1]
    out = bpy.data.images.new(f'T_{prop}_N_gl', img.size[0], img.size[1], alpha=False)
    out.colorspace_settings.name = 'Non-Color'
    out.pixels = px.ravel().tolist()
    path = os.path.join(out_dir, f'_T_{prop}_N_gl.png')
    out.filepath_raw = path
    out.file_format = 'PNG'
    out.save()
    return out


def material(src_dir, out_dir, prop):
    m = bpy.data.materials.new(f'M_{prop}')
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes['Principled BSDF']

    def tex(name, non_color):
        n = nt.nodes.new('ShaderNodeTexImage')
        n.image = bpy.data.images.load(os.path.join(src_dir, name))
        if non_color:
            n.image.colorspace_settings.name = 'Non-Color'
        return n

    bc = tex(f'T_{prop}_BC.png', False)
    nt.links.new(bc.outputs['Color'], bsdf.inputs['Base Color'])
    orm_path = os.path.join(src_dir, f'T_{prop}_ORM.png')
    if os.path.exists(orm_path):
        orm = tex(f'T_{prop}_ORM.png', True)
        sep = nt.nodes.new('ShaderNodeSeparateColor')
        nt.links.new(orm.outputs['Color'], sep.inputs['Color'])
        nt.links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
        nt.links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
        # Occlusion: the exporter reads it from a group named "glTF Material Output".
        grp = bpy.data.node_groups.get('glTF Material Output')
        if grp is None:
            grp = bpy.data.node_groups.new('glTF Material Output', 'ShaderNodeTree')
            grp.interface.new_socket('Occlusion', in_out='INPUT', socket_type='NodeSocketFloat')
        g = nt.nodes.new('ShaderNodeGroup')
        g.node_tree = grp
        nt.links.new(sep.outputs['Red'], g.inputs['Occlusion'])
    n_path = os.path.join(src_dir, f'T_{prop}_N.png')
    if os.path.exists(n_path):
        nimg = nt.nodes.new('ShaderNodeTexImage')
        nimg.image = flipped_normal(n_path, out_dir, prop)
        nmap = nt.nodes.new('ShaderNodeNormalMap')
        nt.links.new(nimg.outputs['Color'], nmap.inputs['Color'])
        nt.links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])
    return m


def stage(src_dir, out_dir, prop, visual_only=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=os.path.join(src_dir, f'SM_{prop}.fbx'))
    if visual_only:
        for ob in [o for o in bpy.data.objects if o.name.startswith(('UCX_', 'SOCKET_'))]:
            bpy.data.objects.remove(ob, do_unlink=True)
    mat = material(src_dir, out_dir, prop)
    hulls = [o for o in bpy.data.objects if o.name.startswith('UCX_')]
    for ob in bpy.data.objects:
        if ob.name.startswith('UCX_'):
            ob['col'] = 'convex'
            ob['vis'] = 0
        elif ob.name.startswith('SOCKET_'):
            ob['socket'] = ob.name[len('SOCKET_'):]
        elif ob.type == 'MESH':
            ob.data.materials.clear()
            ob.data.materials.append(mat)
            if hulls:
                ob['col'] = 'none'
    out = os.path.join(out_dir, f'{prop}_visual.glb' if visual_only else f'{prop}.glb')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_extras=True, export_yup=True,
                              export_apply=True, export_cameras=False, export_lights=False)
    print(f'STAGED {prop}: {out} ({os.path.getsize(out) // 1024} KB), {len(hulls)} hulls')


if __name__ == '__main__':
    args = sys.argv[sys.argv.index('--') + 1:]
    visual_only = '--visual-only' in args
    args = [a for a in args if a != '--visual-only']
    src, dst, props = args[0], args[1], args[2:]
    os.makedirs(dst, exist_ok=True)
    for p in props:
        stage(src, dst, p, visual_only)
