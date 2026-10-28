#!/usr/bin/env python3
"""Builds the enemy humanoid and its rifle from Quaternius' CC0 "Character Enemy" (Toon Shooter
Game Kit): `assets/characters/toon_enemy/enemy.glb` and `rifle.glb`.

Pure Python (no Blender: the sessions that run it may not start Blender), deterministic: two runs
write the same bytes. MIT, ours; the input is CC0 (see the output directory's README.upstream.md).

What it does to the upstream file:
  - one skin and one mesh: the body and the head (two meshes on two identical 43-joint skins
    upstream) merged into `Enemy`, on skin `EnemySkin`;
  - the 14 weapon meshes parented to the right hand dropped; the `AK` among them written apart as
    `rifle.glb`, its vertices in the `Index1.R` joint's frame at the bind pose (attach it there with
    an identity Transform);
  - the armature's x100 scale and the mesh's export transform baked: joints, vertices and clips are
    in metres, the skeleton's root unscaled; the character turned 180 degrees about +Y so it faces
    -Z, the engine's forward;
  - the duplicated `CharacterArmature|*` clips and the unused ones (Duck, Jump*, Punch, Wave, Yes,
    No) dropped; the rest renamed in lower case;
  - clips made here (the kit has none): `aim_idle` (Idle_Shoot's first frame, held), the additive
    `rest`, `aim_up` and `aim_down` (the spine pitched +-60 degrees), `fire` and `hit` (Idle_Shoot and
    HitReact relative to their first frame), and `reload` (2 s, the left arm to the magazine and
    back; the graph puts `mag_out`, `mag_in` and `bolt` on it).
Additive clips are authored as rest x delta: the engine adds rest^-1 x layer on top of a pose.

  python tools/anim/make_enemy.py --source <dir with Character_Enemy.glb> [--out DIR]
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys

SOURCE_SHA256 = "0af9db940907efa76662838a0718bc7b9fec6162332f31d465a856e9fb1730ab"
KEEP_CLIPS = [  # upstream name -> ours
    ("Idle", "idle"), ("Walk", "walk"), ("Run_Gun", "run"),
    ("Walk_Shoot", "walk_aim"), ("Run_Shoot", "run_aim"), ("Death", "death"),
]
HEIGHT = 1.8   # m, the top of the head: PlayerController's default capsule height
RIFLE_MESH = "AK"
GRIP_JOINT = "Index1.R"

# --- small linear algebra (row-major 4x4 lists, quaternions as [x, y, z, w]) ---

def mat_identity():
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_inv(m):
    a = [row[:] + [1.0 if r == c else 0.0 for c in range(4)] for r, row in enumerate(m)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda r: abs(a[r][col]))
        a[col], a[piv] = a[piv], a[col]
        p = a[col][col]
        a[col] = [v / p for v in a[col]]
        for r in range(4):
            if r != col and a[r][col] != 0.0:
                f = a[r][col]
                a[r] = [v - f * w for v, w in zip(a[r], a[col])]
    return [row[4:] for row in a]


def mat_apply(m, p, w=1.0):
    return [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] * w for r in range(3)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def quat_conj(q):
    return [-q[0], -q[1], -q[2], q[3]]


def quat_norm(q):
    n = math.sqrt(sum(v * v for v in q))
    q = [v / n for v in q]
    return q if q[3] >= 0.0 else [-v for v in q]


def quat_axis_angle(axis, deg):
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


def quat_to_mat3(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def quat_from_mat3(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s]
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = [0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s]
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = [(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s]
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = [(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s]
    return quat_norm(q)


def quat_rotate(q, v):
    m = quat_to_mat3(q)
    return [sum(m[r][k] * v[k] for k in range(3)) for r in range(3)]


def trs(t, r, s):
    m3 = quat_to_mat3(r)
    return [[m3[0][0] * s[0], m3[0][1] * s[1], m3[0][2] * s[2], t[0]],
            [m3[1][0] * s[0], m3[1][1] * s[1], m3[1][2] * s[2], t[1]],
            [m3[2][0] * s[0], m3[2][1] * s[1], m3[2][2] * s[2], t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def decompose(m):
    t = [m[0][3], m[1][3], m[2][3]]
    s = [math.sqrt(sum(m[r][c] ** 2 for r in range(3))) for c in range(3)]
    r3 = [[m[r][c] / s[c] for c in range(3)] for r in range(3)]
    return t, quat_from_mat3(r3), s


def normal_matrix(m):
    inv = mat_inv(m)
    return [[inv[c][r] for c in range(3)] for r in range(3)]   # inverse transpose, 3x3


def norm3(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]

# --- reading the GLB ---

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORM = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}


def read_glb(path):
    data = open(path, "rb").read()
    _, _, length = struct.unpack_from("<III", data, 0)
    off, chunks = 12, []
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunks.append(data[off + 8: off + 8 + clen])
        off += 8 + clen
    return data, json.loads(chunks[0]), chunks[1]


def accessor(gltf, binchunk, index):
    a = gltf["accessors"][index]
    view = gltf["bufferViews"][a["bufferView"]]
    fmt, size = COMP[a["componentType"]]
    n = NCOMP[a["type"]]
    stride = view.get("byteStride", size * n)
    base = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    out = []
    for i in range(a["count"]):
        vals = list(struct.unpack_from("<" + fmt * n, binchunk, base + i * stride))
        if a.get("normalized"):
            vals = [max(v / NORM[a["componentType"]], -1.0) for v in vals]
        out.append(vals)
    return out

# --- writing the GLB ---


class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors = [], []

    def add(self, rows, comp, ctype, target=None, minmax=False):
        fmt, _ = COMP[comp]
        start = len(self.bin)
        for row in rows:
            self.bin += struct.pack("<" + fmt * len(row), *row)
        view = {"buffer": 0, "byteOffset": start, "byteLength": len(self.bin) - start}
        if target:
            view["target"] = target
        self.views.append(view)
        while len(self.bin) % 4:
            self.bin += b"\0"
        acc = {"bufferView": len(self.views) - 1, "componentType": comp, "count": len(rows), "type": ctype}
        if minmax:
            acc["min"] = [min(r[i] for r in rows) for i in range(len(rows[0]))]
            acc["max"] = [max(r[i] for r in rows) for i in range(len(rows[0]))]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def write(self, path, doc):
        doc["buffers"] = [{"byteLength": len(self.bin)}]
        doc["bufferViews"] = self.views
        doc["accessors"] = self.accessors
        js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
        while len(js) % 4:
            js += b" "
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js
        out += struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin)
        with open(path, "wb") as f:
            f.write(out)


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--source", required=True, help="directory holding Character_Enemy.glb")
    ap.add_argument("--out", default=os.path.join("assets", "characters", "toon_enemy"))
    args = ap.parse_args()

    src = os.path.join(args.source, "Character_Enemy.glb")
    raw, g, binchunk = read_glb(src)
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit("Character_Enemy.glb sha256 %s, expected %s (another upstream version?)" % (digest, SOURCE_SHA256))

    nodes = g["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i

    def local(i):
        n = nodes[i]
        return trs(n.get("translation", [0, 0, 0]), n.get("rotation", [0, 0, 0, 1]), n.get("scale", [1, 1, 1]))

    def world(i):
        m = local(i)
        while i in parent:
            i = parent[i]
            m = mat_mul(local(i), m)
        return m

    def ibms(skin):
        rows = accessor(g, binchunk, skin["inverseBindMatrices"])
        return [[[r[c * 4 + rr] for c in range(4)] for rr in range(4)] for r in rows]

    skins = g["skins"]
    joints = skins[1]["joints"]
    names = [nodes[j]["name"] for j in joints]
    if [nodes[j]["name"] for j in skins[0]["joints"]] != names:
        sys.exit("the two skins name different joints")
    jointOf = {n: k for k, n in enumerate(joints)}
    ibm = ibms(skins[1])

    # The mesh's export transform. Upstream the bind pose (a T-pose) is not the nodes' rest pose (arms
    # down): bind(joint) = meshWorld x inverseBind^-1. Both skins and both mesh nodes agree.
    mesh_nodes = [i for i, n in enumerate(nodes) if n.get("name") in ("Character_Enemy", "Character_Enemy_Head")]
    bind = world(mesh_nodes[0])
    worst = max(abs(world(m)[r][c] - bind[r][c]) for m in mesh_nodes for r in range(4) for c in range(4))
    worst = max([worst] + [abs(a - b) for x, y in zip(ibms(skins[0]), ibm) for ra, rb in zip(x, y) for a, b in zip(ra, rb)])
    if worst > 1e-5:
        sys.exit("the skins or mesh nodes differ (%g)" % worst)

    # The skeleton's root: its world transform (armature included), unscaled, turned to face -Z.
    root = joints[0]
    turn = quat_axis_angle([0.0, 1.0, 0.0], 180.0)
    rt, rr, rs = decompose(world(root))
    if max(abs(v - rs[0]) for v in rs) > 1e-3 * rs[0]:
        sys.exit("the armature's scale is not uniform")
    # Fitted to the character controller: the top of the head at HEIGHT (upstream: 2.28 m).
    top = max(mat_apply(bind, x)[1] for m in mesh_nodes
              for p in g["meshes"][nodes[m]["mesh"]]["primitives"] for x in accessor(g, binchunk, p["attributes"]["POSITION"]))
    fit = HEIGHT / top
    rt = [v * fit for v in rt]
    scale = rs[0] * fit   # upstream joint translations to metres, fitted
    jparent = {}
    for k, j in enumerate(joints):
        p = parent.get(j)
        while p is not None and p not in jointOf:
            p = parent.get(p)
        jparent[k] = jointOf[p] if p is not None else -1
    if any(jparent[k] < 0 for k in range(1, len(joints))) or jparent[0] != -1:
        sys.exit("expected one root joint, first in the skin")
    rest_t, rest_r, rest_s = [], [], []
    for k, j in enumerate(joints):
        n = nodes[j]
        if k == 0:
            rest_t.append(quat_rotate(turn, rt))
            rest_r.append(quat_norm(quat_mul(turn, rr)))
        else:
            rest_t.append([v * scale for v in n.get("translation", [0, 0, 0])])
            rest_r.append(quat_norm(n.get("rotation", [0, 0, 0, 1])))
        rest_s.append([1.0, 1.0, 1.0])
    gl = []
    for k in range(len(joints)):
        m = trs(rest_t[k], rest_r[k], rest_s[k])
        gl.append(m if jparent[k] < 0 else mat_mul(gl[jparent[k]], m))
    # The joints now hold G' = turn x G x scale^-1; vertices are turn x meshWorld x v, so the inverse
    # binds become scale x inverseBind x meshWorld^-1 x turn^-1.
    # With the fit F: G'' = F turn G scale^-1 F^-1, vertices F turn meshWorld v, inverse binds
    # F scale inverseBind meshWorld^-1 turn^-1 F^-1.
    unturn = trs([0, 0, 0], quat_conj(turn), [1, 1, 1])
    F = trs([0, 0, 0], [0, 0, 0, 1], [fit] * 3)
    S = trs([0, 0, 0], [0, 0, 0, 1], [rs[0] * fit] * 3)
    new_ibm = [mat_mul(S, mat_mul(m, mat_mul(mat_inv(bind), mat_mul(unturn, mat_inv(F))))) for m in ibm]
    model_q = []   # each joint's model-space rotation at rest
    for k in range(len(joints)):
        q = rest_r[k]
        model_q.append(q if jparent[k] < 0 else quat_norm(quat_mul(model_q[jparent[k]], q)))

    vert_m = mat_mul(F, mat_mul(trs([0, 0, 0], turn, [1, 1, 1]), bind))
    vert_n = normal_matrix(vert_m)

    out = Glb()
    prims = []
    materials, matIndex = [], {}

    def material(i):
        if i not in matIndex:
            matIndex[i] = len(materials)
            materials.append(g["materials"][i])
        return matIndex[i]

    lo, hi = [1e9] * 3, [-1e9] * 3
    for meshNode in ("Character_Enemy", "Character_Enemy_Head"):
        mesh = g["meshes"][next(n["mesh"] for n in nodes if n.get("name") == meshNode)]
        for p in mesh["primitives"]:
            at = p["attributes"]
            pos = [[f32(v) for v in mat_apply(vert_m, x)] for x in accessor(g, binchunk, at["POSITION"])]
            nrm = [[f32(v) for v in norm3([sum(vert_n[r][k] * x[k] for k in range(3)) for r in range(3)])]
                   for x in accessor(g, binchunk, at["NORMAL"])]
            uv = accessor(g, binchunk, at["TEXCOORD_0"])
            jnt = [[int(v) for v in x] for x in accessor(g, binchunk, at["JOINTS_0"])]
            wgt = [[f32(v) for v in x] for x in accessor(g, binchunk, at["WEIGHTS_0"])]
            idx = [[int(x[0])] for x in accessor(g, binchunk, p["indices"])]
            for x in pos:
                lo = [min(a, b) for a, b in zip(lo, x)]
                hi = [max(a, b) for a, b in zip(hi, x)]
            prims.append({
                "attributes": {
                    "POSITION": out.add(pos, 5126, "VEC3", 34962, True),
                    "NORMAL": out.add(nrm, 5126, "VEC3", 34962),
                    "TEXCOORD_0": out.add(uv, 5126, "VEC2", 34962),
                    "JOINTS_0": out.add(jnt, 5121, "VEC4", 34962),
                    "WEIGHTS_0": out.add(wgt, 5126, "VEC4", 34962),
                },
                "indices": out.add(idx, 5123, "SCALAR", 34963),
                "material": material(p["material"]),
            })
    print("enemy bind box  lo %s  hi %s" % (["%.3f" % v for v in lo], ["%.3f" % v for v in hi]))

    # Clips. Channels keyed by our joint index; translations scaled to metres.
    def channels_of(anim):
        out_ch = {}
        for ch in anim["channels"]:
            node = ch["target"]["node"]
            if node not in jointOf:
                continue
            k = jointOf[node]
            sm = anim["samplers"][ch["sampler"]]
            if sm.get("interpolation", "LINEAR") != "LINEAR":
                sys.exit("unexpected interpolation")
            times = [x[0] for x in accessor(g, binchunk, sm["input"])]
            vals = accessor(g, binchunk, sm["output"])
            path = ch["target"]["path"]
            if path == "translation":
                if k == 0:
                    sys.exit("the root joint is animated: bake its turn into the keys")
                vals = [[v * scale for v in x] for x in vals]
            elif path == "rotation":
                vals = [quat_norm(x) for x in vals]
            else:
                sys.exit("unexpected path " + path)
            out_ch[(k, path)] = (times, vals)
        return out_ch

    anims = {a["name"]: a for a in g["animations"]}
    clips = []   # (name, {(joint, path): (times, values)})
    for up, ours in KEEP_CLIPS:
        clips.append((ours, channels_of(anims[up])))

    def held(src, name):
        ch = {key: ([0.0, 1.0], [vals[0], vals[0]]) for key, (times, vals) in src.items()}
        clips.append((name, ch))

    def relative(src, name):
        # rest x first^-1 x key: the clip's motion from its first frame, on the rest pose.
        ch = {}
        for (k, path), (times, vals) in src.items():
            if path == "rotation":
                d0 = quat_conj(vals[0])
                ch[(k, path)] = (times, [quat_norm(quat_mul(rest_r[k], quat_mul(d0, v))) for v in vals])
            else:
                ch[(k, path)] = (times, [[rest_t[k][i] + v[i] - vals[0][i] for i in range(3)] for v in vals])
        clips.append((name, ch))

    def posed(name, keys, times):
        # keys: joint name -> list (per time) of (axis, degrees) world rotations at the rest pose.
        ch = {}
        for jn, per in keys.items():
            k = names.index(jn)
            vals = []
            for axis, deg in per:
                d = quat_mul(quat_conj(model_q[k]), quat_mul(quat_axis_angle(axis, deg), model_q[k]))
                vals.append(quat_norm(quat_mul(rest_r[k], d)))
            ch[(k, "rotation")] = (times, vals)
        clips.append((name, ch))

    shoot = channels_of(anims["Idle_Shoot"])
    held(shoot, "aim_idle")
    relative(shoot, "fire")
    relative(channels_of(anims["HitReact"]), "hit")
    X, Y, Z = [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]
    posed("rest", {"Hips": [(X, 0.0), (X, 0.0)]}, [0.0, 1.0])
    for name, sign in (("aim_up", 1.0), ("aim_down", -1.0)):
        posed(name, {"Abdomen": [(X, 20 * sign)] * 2, "Torso": [(X, 25 * sign)] * 2, "Neck": [(X, 15 * sign)] * 2},
              [0.0, 1.0])
    # Reload, 2 s: the gun dips and turns in, the left arm drops to the magazine (mag_out 0.6 s),
    # reaches the pouch, comes back up to seat it (mag_in 1.3 s), the bolt (1.75 s), back.
    t = [0.0, 0.3, 0.6, 1.0, 1.3, 1.6, 1.75, 2.0]
    posed("reload", {
        "Torso": [(X, 0), (X, -12), (X, -14), (X, -12), (X, -10), (X, -8), (X, -4), (X, 0)],
        "Neck": [(X, 0), (X, -10), (X, -12), (X, -10), (X, -10), (X, -6), (X, -3), (X, 0)],
        "UpperArm.L": [(X, 0), (X, -15), (X, -45), (X, -55), (X, -20), (X, -10), (X, -5), (X, 0)],
        "LowerArm.L": [(Z, 0), (Z, 10), (Z, 25), (Z, 35), (Z, 15), (Z, 5), (Z, 2), (Z, 0)],
    }, t)

    anim_out = []
    for name, ch in clips:
        samplers, chans = [], []
        for (k, path), (times, vals) in sorted(ch.items()):
            ti = out.add([[f32(v)] for v in times], 5126, "SCALAR", None, True)
            vo = out.add([[f32(v) for v in x] for x in vals], 5126, "VEC4" if path == "rotation" else "VEC3")
            samplers.append({"input": ti, "output": vo, "interpolation": "LINEAR"})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": k, "path": path}})
        anim_out.append({"name": name, "samplers": samplers, "channels": chans})
        if any(p == "translation" for (k, p) in ch):
            body = ch.get((names.index("Body"), "translation"))
            drift = [body[1][-1][i] - body[1][0][i] for i in range(3)] if body else [0, 0, 0]
        else:
            drift = [0, 0, 0]
        dur = max(times[-1] for times, _ in ch.values())
        print("clip %-9s %5.3f s  %2d channels  Body drift %s" % (name, dur, len(ch), ["%.3f" % v for v in drift]))

    ibm_acc = out.add([[f32(m[r][c]) for c in range(4) for r in range(4)] for m in new_ibm], 5126, "MAT4")
    jnodes = []
    for k in range(len(joints)):
        node = {"name": names[k], "translation": [f32(v) for v in rest_t[k]],
                "rotation": [f32(v) for v in rest_r[k]]}
        kids = [c for c in range(len(joints)) if jparent[c] == k]
        if kids:
            node["children"] = kids
        jnodes.append(node)
    mesh_node = len(jnodes)
    doc = {
        "asset": {"version": "2.0", "generator": "ohao tools/anim/make_enemy.py",
                  "copyright": "CC0 1.0: Quaternius, Toon Shooter Game Kit (see README.upstream.md)"},
        "scene": 0,
        "scenes": [{"name": "Enemy", "nodes": [0, mesh_node]}],
        "nodes": jnodes + [{"name": "Enemy", "mesh": 0, "skin": 0}],
        "meshes": [{"name": "Enemy", "primitives": prims}],
        "materials": materials,
        "skins": [{"name": "EnemySkin", "joints": list(range(len(joints))), "skeleton": 0,
                   "inverseBindMatrices": ibm_acc}],
        "animations": anim_out,
    }
    os.makedirs(args.out, exist_ok=True)
    out.write(os.path.join(args.out, "enemy.glb"), doc)

    # The rifle, in the grip joint's frame at the bind pose.
    rifle_node = next(i for i, n in enumerate(nodes) if n.get("name") == RIFLE_MESH and "mesh" in n)
    grip = names.index(GRIP_JOINT)
    to_grip = mat_mul(mat_inv(gl[grip]), mat_mul(F, mat_mul(trs([0, 0, 0], turn, [1, 1, 1]), world(rifle_node))))
    to_grip_n = normal_matrix(to_grip)
    rout = Glb()
    rprims, rmats, rmatIndex = [], [], {}
    rlo, rhi = [1e9] * 3, [-1e9] * 3
    allpos = []
    for p in g["meshes"][nodes[rifle_node]["mesh"]]["primitives"]:
        at = p["attributes"]
        pos = [[f32(v) for v in mat_apply(to_grip, x)] for x in accessor(g, binchunk, at["POSITION"])]
        nrm = [[f32(v) for v in norm3([sum(to_grip_n[r][k] * x[k] for k in range(3)) for r in range(3)])]
               for x in accessor(g, binchunk, at["NORMAL"])]
        uv = accessor(g, binchunk, at["TEXCOORD_0"])
        idx = [[int(x[0])] for x in accessor(g, binchunk, p["indices"])]
        allpos += pos
        for x in pos:
            rlo = [min(a, b) for a, b in zip(rlo, x)]
            rhi = [max(a, b) for a, b in zip(rhi, x)]
        if p["material"] not in rmatIndex:
            rmatIndex[p["material"]] = len(rmats)
            rmats.append(g["materials"][p["material"]])
        rprims.append({
            "attributes": {"POSITION": rout.add(pos, 5126, "VEC3", 34962, True),
                           "NORMAL": rout.add(nrm, 5126, "VEC3", 34962),
                           "TEXCOORD_0": rout.add(uv, 5126, "VEC2", 34962)},
            "indices": rout.add(idx, 5123, "SCALAR", 34963),
            "material": rmatIndex[p["material"]],
        })
    rout.write(os.path.join(args.out, "rifle.glb"), {
        "asset": {"version": "2.0", "generator": "ohao tools/anim/make_enemy.py",
                  "copyright": "CC0 1.0: Quaternius, Toon Shooter Game Kit (see README.upstream.md)"},
        "scene": 0, "scenes": [{"name": "Rifle", "nodes": [0]}],
        "nodes": [{"name": "Rifle", "mesh": 0}],
        "meshes": [{"name": "Rifle", "primitives": rprims}],
        "materials": rmats,
    })
    # The barrel's tip: the rifle's longest axis, the centre of the vertices within 1 cm of its end.
    ext = [rhi[i] - rlo[i] for i in range(3)]
    ax = ext.index(max(ext))
    # The muzzle is the end farther from the grip (the joint's origin).
    end = rhi[ax] if abs(rhi[ax]) > abs(rlo[ax]) else rlo[ax]
    tip = [x for x in allpos if abs(x[ax] - end) < 0.01]
    c = [sum(x[i] for x in tip) / len(tip) for i in range(3)]
    print("rifle box lo %s hi %s; muzzle %s (axis %d)" % (["%.3f" % v for v in rlo], ["%.3f" % v for v in rhi],
                                                        ["%.4f" % v for v in c], ax))
    # Where the grip joint and the muzzle are on the character at the bind pose (the root's frame).
    gp = [gl[grip][r][3] for r in range(3)]
    mw = mat_apply(gl[grip], c)
    print("grip at rest %s; muzzle at rest %s" % (["%.3f" % v for v in gp], ["%.3f" % v for v in mw]))


if __name__ == "__main__":
    main()
