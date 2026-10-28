"""The animation test rig (sub-project 10, docs/specs/2026-10-28-animation-design.md, "Assets and
content"): a jointed column of four boxes on a four-joint chain, with known weights, four clips
that cover every glTF sampler and channel kind, and its animation graph. Pure Python (no Blender),
so its bytes are the same on every machine; ours (MIT).

    python tools/anim/make_test_rig.py [OUT_DIR]     (default tests/anim/fixtures)

Writes rig.glb and rig.animgraph.json. Positions, times, weights and translations are multiples of 1/8
(exact in float32); quaternion components come from the table in quat_z, stored as float32, so the C++
tests assert the values read back to the bit against float(table value).

The rig, in metres, Y up:
  joints   root (0,0,0) -> lower (0,0.5,0) -> middle (0,1,0) -> upper (0,1.5,0), names as written
  boxes    box k spans y 0.5k .. 0.5k+0.5, x and z -0.125 .. 0.125, 24 vertices (4 a face, flat normals)
  weights  box k's bottom-face vertices: joint k, 1.0; every other vertex of box k: joint k 0.5 and
           joint k+1 0.5 (box 3: joint 3, 1.0)
  clips    wave   (loop, 2 s): lower, middle and upper rotate about Z, LINEAR, 0 / +22.5 / 0 / -22.5 / 0 deg
                   at 0 / 0.5 / 1 / 1.5 / 2 s
           raise  (1 s): lower rotates about Z 0 -> 90 deg (LINEAR, 2 keys): the column above y 0.5 swings
                   to -X, 1.5 m outside the bind box
           step   (1 s): upper rotation STEP 0 / 45 / 0 deg at 0 / 0.5 / 1 s about Z, upper scale STEP 1 /
                   1.5 / 1 at the same times
           spline (1 s): root translation CUBICSPLINE (0,0,0) -> (0,0.25,0), in and out tangents 0 then
                   (0,0.5,0)
"""
import json
import os
import struct
import sys

FLOAT, UBYTE, USHORT = 5126, 5121, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963

JOINTS = ["root", "lower", "middle", "upper"]
HALF = 0.125


def quat_z(deg):
    """A rotation about +Z as glTF [x, y, z, w]; the angles used are exact table values."""
    table = {0.0: (0.0, 1.0), 22.5: (0.19509032, 0.98078528), -22.5: (-0.19509032, 0.98078528),
             45.0: (0.38268343, 0.92387953), 90.0: (0.70710678, 0.70710678)}
    s, c = table[deg]
    return [0.0, 0.0, s, c]


class Bin:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, fmt, values, comp, ctype, count, target=None, minmax=None):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        self.data += struct.pack("<" + fmt * len(values), *values)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset}
        if target:
            view["target"] = target
        self.views.append(view)
        acc = {"bufferView": len(self.views) - 1, "componentType": comp, "count": count, "type": ctype}
        if minmax:
            acc["min"], acc["max"] = minmax
        self.accessors.append(acc)
        return len(self.accessors) - 1


def box_vertices(k):
    """Box k's 24 vertices: (position, normal, joints, weights), 4 a face, faces in a fixed order."""
    y0, y1 = 0.5 * k, 0.5 * k + 0.5
    faces = [  # normal, four corners counter-clockwise seen from outside
        ((0, -1, 0), [(-HALF, y0, -HALF), (HALF, y0, -HALF), (HALF, y0, HALF), (-HALF, y0, HALF)]),
        ((0, 1, 0), [(-HALF, y1, HALF), (HALF, y1, HALF), (HALF, y1, -HALF), (-HALF, y1, -HALF)]),
        ((0, 0, 1), [(-HALF, y0, HALF), (HALF, y0, HALF), (HALF, y1, HALF), (-HALF, y1, HALF)]),
        ((0, 0, -1), [(HALF, y0, -HALF), (-HALF, y0, -HALF), (-HALF, y1, -HALF), (HALF, y1, -HALF)]),
        ((1, 0, 0), [(HALF, y0, HALF), (HALF, y0, -HALF), (HALF, y1, -HALF), (HALF, y1, HALF)]),
        ((-1, 0, 0), [(-HALF, y0, -HALF), (-HALF, y0, HALF), (-HALF, y1, HALF), (-HALF, y1, -HALF)]),
    ]
    out = []
    for fi, (n, corners) in enumerate(faces):
        for p in corners:
            bottom_face = fi == 0
            if bottom_face or k == 3:
                j, w = (k, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)
            else:
                j, w = (k, k + 1, 0, 0), (0.5, 0.5, 0.0, 0.0)
            out.append((p, n, j, w))
    return out


def build():
    b = Bin()
    verts, indices = [], []
    for k in range(4):
        base = len(verts)
        verts += box_vertices(k)
        for f in range(6):
            q = base + 4 * f
            indices += [q, q + 1, q + 2, q, q + 2, q + 3]
    pos = [c for v in verts for c in v[0]]
    nrm = [c for v in verts for c in v[1]]
    jnt = [c for v in verts for c in v[2]]
    wgt = [c for v in verts for c in v[3]]
    a_pos = b.add("f", pos, FLOAT, "VEC3", len(verts), ARRAY_BUFFER,
                  ([-HALF, 0.0, -HALF], [HALF, 2.0, HALF]))
    a_nrm = b.add("f", nrm, FLOAT, "VEC3", len(verts), ARRAY_BUFFER)
    a_jnt = b.add("B", jnt, UBYTE, "VEC4", len(verts), ARRAY_BUFFER)
    a_wgt = b.add("f", wgt, FLOAT, "VEC4", len(verts), ARRAY_BUFFER)
    a_idx = b.add("H", indices, USHORT, "SCALAR", len(indices), ELEMENT_ARRAY_BUFFER)
    # Inverse bind matrices (column-major): joint k sits at (0, 0.5k, 0) in the rig's space.
    ibm = []
    for k in range(4):
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -0.5 * k, 0, 1]
    a_ibm = b.add("f", ibm, FLOAT, "MAT4", 4)

    # Nodes: 0 the mesh node "Rig" (skinned; its own transform is ignored by skinning), 1..4 the joints.
    nodes = [{"name": "Rig", "mesh": 0, "skin": 0}]
    for k, name in enumerate(JOINTS):
        node = {"name": name}
        if k > 0:
            node["translation"] = [0.0, 0.5, 0.0]
        if k < 3:
            node["children"] = [k + 2]
        nodes.append(node)
    joint_node = {name: k + 1 for k, name in enumerate(JOINTS)}

    animations = []

    def channel_set(name, channels):
        samplers, chans = [], []
        for node, path, interp, times, values, comps in channels:
            a_in = b.add("f", times, FLOAT, "SCALAR", len(times), None, ([min(times)], [max(times)]))
            flat = [c for v in values for c in v]
            count = len(values)
            a_out = b.add("f", flat, FLOAT, "VEC4" if comps == 4 else "VEC3", count)
            samplers.append({"input": a_in, "output": a_out, "interpolation": interp})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": joint_node[node], "path": path}})
        animations.append({"name": name, "channels": chans, "samplers": samplers})

    wave_t = [0.0, 0.5, 1.0, 1.5, 2.0]
    wave_q = [quat_z(a) for a in (0.0, 22.5, 0.0, -22.5, 0.0)]
    channel_set("wave", [(j, "rotation", "LINEAR", wave_t, wave_q, 4) for j in ("lower", "middle", "upper")])
    channel_set("raise", [("lower", "rotation", "LINEAR", [0.0, 1.0], [quat_z(0.0), quat_z(90.0)], 4)])
    step_t = [0.0, 0.5, 1.0]
    channel_set("step", [
        ("upper", "rotation", "STEP", step_t, [quat_z(0.0), quat_z(45.0), quat_z(0.0)], 4),
        ("upper", "scale", "STEP", step_t, [[1.0, 1.0, 1.0], [1.5, 1.5, 1.5], [1.0, 1.0, 1.0]], 3),
    ])
    # CUBICSPLINE output: per key, in-tangent, value, out-tangent.
    spline = [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0],
              [0.0, 0.5, 0.0], [0.0, 0.25, 0.0], [0.0, 0.5, 0.0]]
    channel_set("spline", [("root", "translation", "CUBICSPLINE", [0.0, 1.0], spline, 3)])

    while len(b.data) % 4:
        b.data.append(0)
    doc = {
        "asset": {"version": "2.0", "generator": "OHAO tools/anim/make_test_rig.py"},
        "scene": 0,
        "scenes": [{"name": "RigScene", "nodes": [0, 1]}],
        "nodes": nodes,
        "meshes": [{"name": "Column", "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
            "indices": a_idx, "material": 0}]}],
        "materials": [{"name": "RigGrey", "pbrMetallicRoughness": {
            "baseColorFactor": [0.75, 0.625, 0.5, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.75},
            "doubleSided": False}],
        "skins": [{"name": "RigSkin", "joints": [joint_node[j] for j in JOINTS], "inverseBindMatrices": a_ibm,
                   "skeleton": joint_node["root"]}],
        "animations": animations,
        "buffers": [{"byteLength": len(b.data)}],
        "bufferViews": b.views,
        "accessors": b.accessors,
    }
    text = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    while len(text) % 4:
        text += b" "
    glb = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + 8 + len(b.data))
    glb += struct.pack("<II", len(text), 0x4E4F534A) + text
    glb += struct.pack("<II", len(b.data), 0x004E4942) + bytes(b.data)
    return glb


GRAPH = {
    "skeleton": {"file": "tests/anim/fixtures/rig.glb", "skin": "RigSkin"},
    "clips": {
        "wave": {"file": "tests/anim/fixtures/rig.glb", "animation": "wave", "loop": True,
                 "events": [{"t": 0.25, "name": "tick"}]},
        "raise": {"file": "tests/anim/fixtures/rig.glb", "animation": "raise", "loop": False},
        "step": {"file": "tests/anim/fixtures/rig.glb", "animation": "step", "loop": True},
        "spline": {"file": "tests/anim/fixtures/rig.glb", "animation": "spline", "loop": True},
    },
    "params": {"speed": "float", "raised": "bool"},
    "layers": [
        {
            "name": "body", "blend": "replace", "weight": 1.0, "initial": "wave",
            "states": {
                "wave": {"clip": "wave"},
                "raise": {"clip": "raise"},
                "held": {"clip": "raise", "rate": 0.0, "startAt": 1.0},
                "moves": {"blend1d": {"param": "speed", "points": [{"clip": "wave", "at": 0.0},
                                                                  {"clip": "step", "at": 1.0}]}},
            },
            "transitions": [
                {"from": "wave", "to": "raise", "when": {"param": "raised", "op": "==", "value": 1}, "fade": 6},
                {"from": "raise", "to": "held", "when": {"clipEnd": True}, "fade": 0},
                {"from": "held", "to": "wave", "when": {"param": "raised", "op": "==", "value": 0}, "fade": 12},
                {"from": "wave", "to": "moves", "when": {"param": "speed", "op": ">", "value": 0.5}, "fade": 6},
                {"from": "moves", "to": "wave", "when": {"param": "speed", "op": "<=", "value": 0.5}, "fade": 6},
            ],
        },
        {
            "name": "top", "blend": "additive", "weight": 1.0, "mask": ["upper"], "initial": "still",
            "states": {"still": {"clip": "step", "rate": 0.0}, "nod": {"clip": "step"}},
            "transitions": [
                {"from": "still", "to": "nod", "when": {"event": "nod"}, "fade": 0},
                {"from": "nod", "to": "still", "when": {"clipEnd": True}, "fade": 0},
            ],
        },
    ],
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("tests", "anim", "fixtures")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "rig.glb"), "wb") as f:
        f.write(build())
    with open(os.path.join(out, "rig.animgraph.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(GRAPH, indent=2) + "\n")
    print("wrote", os.path.join(out, "rig.glb"), "and rig.animgraph.json")


if __name__ == "__main__":
    main()
