"""Writes tests/level/fixtures/level_audio.gltf, the reverb-zone fixture of audio_schedule_tests (sub-project 11,
step 2, t27_verb_hint): a box mesh in an embedded (data URI) buffer and one node per `verb` case.

    python tests/level/make_audio_fixture.py [out]      (from the repo root)

The output is checked in; rerunning this script reproduces it byte for byte (check_level_assets.py
writes it elsewhere and compares)."""
import base64
import json
import struct
import sys


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def box_mesh(lo, hi):
    """8 corners, 12 triangles, each wound counter-clockwise seen from outside."""
    xs, ys, zs = (lo[0], hi[0]), (lo[1], hi[1]), (lo[2], hi[2])
    v = [(x, y, z) for x in xs for y in ys for z in zs]   # index = 4*ix + 2*iy + iz
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    out = []
    for q in quads:
        for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
            a, b, d = (v[i] for i in t)
            centre = [(a[i] + b[i] + d[i]) / 3 for i in range(3)]
            if dot(cross(sub(b, a), sub(d, a)), sub(centre, c)) < 0:
                t = (t[0], t[2], t[1])
            out.extend(t)
    return v, out


# A 4 x 2 x 4 m volume, centred on its node.
verts, idx = box_mesh((-2, -1, -2), (2, 1, 2))
blob = bytearray()
for p in verts:
    blob += struct.pack('<3f', *p)
pos_len = len(blob)
for i in idx:
    blob += struct.pack('<H', i)
idx_len = len(blob) - pos_len
while len(blob) % 4:
    blob += b'\0'

nodes = [
    # vis 0: a volume nobody sees; the hint makes it a trigger box and a ReverbZone{room}.
    {'name': 'RoomZone', 'mesh': 0, 'translation': [0, 1, 0], 'extras': {'verb': 'room', 'vis': 0}},
    # A preset the bank does not have: W_AUDIO_PRESET, and the zone counts as the default.
    {'name': 'CaveZone', 'mesh': 0, 'translation': [20, 1, 0], 'extras': {'verb': 'cave', 'vis': 0}},
    # The node says otherwise: a solid mesh collider stays one.
    {'name': 'SolidZone', 'mesh': 0, 'translation': [40, 1, 0], 'extras': {'verb': 'corridor', 'col': 'mesh'}},
]
doc = {
    'asset': {'version': '2.0', 'generator': 'OHAO fixture (tests/level/make_audio_fixture.py)'},
    'scene': 0,
    'scenes': [{'name': 'Audio', 'nodes': list(range(len(nodes)))}],
    'nodes': nodes,
    'meshes': [{'name': 'Volume', 'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1}]}],
    'accessors': [
        {'bufferView': 0, 'componentType': 5126, 'count': len(verts), 'type': 'VEC3',
         'min': [-2, -1, -2], 'max': [2, 1, 2]},
        {'bufferView': 1, 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'},
    ],
    'bufferViews': [
        {'buffer': 0, 'byteOffset': 0, 'byteLength': pos_len, 'target': 34962},
        {'buffer': 0, 'byteOffset': pos_len, 'byteLength': idx_len, 'target': 34963},
    ],
    'buffers': [{'byteLength': len(blob),
                 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(blob)).decode()}],
}
out = sys.argv[1] if len(sys.argv) > 1 else 'tests/level/fixtures/level_audio.gltf'
with open(out, 'w', newline='\n') as f:
    f.write(json.dumps(doc, indent=1) + '\n')
print(out, len(blob), 'buffer bytes')
