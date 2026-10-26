"""Writes tests/level/fixtures/level_physics.gltf, the collision fixture of level_physics_tests (sub-project 5,
step 4, test 6): meshes with real geometry in an embedded (data URI) buffer, one node per fitting case.

    python tests/level/make_physics_fixture.py      (from the repo root)

The output is checked in; rerunning this script reproduces it byte for byte."""
import base64
import json
import struct


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def outward(v, tris, c):
    """Each triangle wound counter-clockwise seen from outside (from the centre c)."""
    out = []
    for t in tris:
        a, b, d = (v[i] for i in t)
        centre = [(a[i] + b[i] + d[i]) / 3 for i in range(3)]
        if dot(cross(sub(b, a), sub(d, a)), sub(centre, c)) < 0:
            t = (t[0], t[2], t[1])
        out.extend(t)
    return out


def box_mesh(lo, hi):
    """8 corners, 12 triangles."""
    xs, ys, zs = (lo[0], hi[0]), (lo[1], hi[1]), (lo[2], hi[2])
    v = [(x, y, z) for x in xs for y in ys for z in zs]   # index = 4*ix + 2*iy + iz
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = [t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))]
    return v, outward(v, tris, [(lo[i] + hi[i]) / 2 for i in range(3)])


def tetra():
    v = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    return v, outward(v, [(0, 2, 1), (0, 1, 3), (0, 3, 2), (1, 2, 3)], (0.25, 0.25, 0.25))


def ground():
    return [(-5, 0, -5), (-5, 0, 5), (5, 0, 5), (5, 0, -5)], [0, 1, 2, 0, 2, 3]   # facing +y


meshes = [('Brick', box_mesh((0, 0, 0), (2, 1, 0.5))), ('Tetra', tetra()), ('Ground', ground()),
          ('Cube', box_mesh((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5)))]

blob = bytearray()
views, accessors, gltf_meshes = [], [], []
for name, (verts, idx) in meshes:
    pos_off = len(blob)
    for p in verts:
        blob += struct.pack('<3f', *p)
    views.append({'buffer': 0, 'byteOffset': pos_off, 'byteLength': len(blob) - pos_off, 'target': 34962})
    lo = [min(p[i] for p in verts) for i in range(3)]
    hi = [max(p[i] for p in verts) for i in range(3)]
    accessors.append({'bufferView': len(views) - 1, 'componentType': 5126, 'count': len(verts), 'type': 'VEC3',
                      'min': lo, 'max': hi})
    while len(blob) % 4:
        blob += b'\0'
    idx_off = len(blob)
    for i in idx:
        blob += struct.pack('<H', i)
    views.append({'buffer': 0, 'byteOffset': idx_off, 'byteLength': len(blob) - idx_off, 'target': 34963})
    accessors.append({'bufferView': len(views) - 1, 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'})
    while len(blob) % 4:
        blob += b'\0'
    gltf_meshes.append({'name': name, 'primitives': [{'attributes': {'POSITION': len(accessors) - 2},
                                                      'indices': len(accessors) - 1}]})

nodes = [
    {'name': 'FitBox', 'mesh': 0, 'extras': {'col': 'box'}},
    {'name': 'FitSphere', 'mesh': 0, 'translation': [10, 0, 0], 'extras': {'col': 'sphere'}},
    {'name': 'FitCapsule', 'mesh': 0, 'translation': [20, 0, 0], 'extras': {'col': 'capsule'}},
    {'name': 'Hull', 'mesh': 1, 'translation': [30, 0, 0], 'extras': {'col': 'convex'}},
    {'name': 'Ground', 'mesh': 2, 'translation': [0, -5, 0]},
    {'name': 'Prop', 'translation': [40, 0, 0], 'extras': {'body': 'dynamic', 'mass': 3}},
    {'name': 'UCX_Prop_00', 'mesh': 3, 'translation': [40, 0, 0]},
    {'name': 'UCX_Prop_01', 'mesh': 3, 'translation': [40, 2, 0]},
    {'name': 'DynMesh', 'mesh': 0, 'translation': [50, 0, 0], 'extras': {'col': 'mesh', 'body': 'dynamic'}},
    {'name': 'Stretched', 'mesh': 3, 'translation': [60, 0, 0], 'scale': [2, 1, 3], 'extras': {'col': 'box'}},
    {'name': 'StretchedMesh', 'mesh': 3, 'translation': [70, 0, 0], 'scale': [2, 1, 3], 'extras': {'col': 'mesh'}},
    {'name': 'Mirrored', 'mesh': 3, 'translation': [80, 0, 0], 'scale': [-1, 1, 1], 'extras': {'col': 'mesh'}},
    {'name': 'Marker', 'translation': [90, 0, 0]},
]
doc = {
    'asset': {'version': '2.0', 'generator': 'OHAO fixture (tests/level/make_physics_fixture.py)'},
    'scene': 0,
    'scenes': [{'name': 'Physics', 'nodes': list(range(len(nodes)))}],
    'nodes': nodes,
    'meshes': gltf_meshes,
    'accessors': accessors,
    'bufferViews': views,
    'buffers': [{'byteLength': len(blob),
                 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(blob)).decode()}],
}
out = 'tests/level/fixtures/level_physics.gltf'
with open(out, 'w', newline='\n') as f:
    f.write(json.dumps(doc, indent=1) + '\n')
print(out, len(blob), 'buffer bytes')
