#!/usr/bin/env python3
"""check_level_assets -- the level pipeline's checked-in assets (sub-project 5, test 1).

    python tests/level/check_level_assets.py

Last line is a JSON summary; exit 0 = pass. A check that needs Blender reports SKIP
without it (a skip is neither a pass nor a failure, and is listed in the summary).
  l1_* every assets/levels/*.glb node's extras validate against tests/level/extras_schema.json;
  the blockout's nodes and hints are the contract later steps assert against; a Blender
  rebuild of the blockout (tools/level/blockout_room.py) is byte-identical to the checked-in
  file; the blockout scene references it. Needs `jsonschema`.
"""
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

import jsonschema

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEVEL = os.path.join(ROOT, "assets", "levels", "blockout_room.glb")
SCENE = os.path.join(ROOT, "assets", "scenes", "blockout_room.scene.json")
SCHEMA = os.path.join(ROOT, "tests", "level", "extras_schema.json")

passed, failed, skipped, failures = 0, 0, [], []


class Skip(Exception):
    pass


def run(fn):
    global passed, failed
    print(f"  {fn.__name__:<62}", end="", flush=True)
    try:
        fn()
    except Skip as e:
        skipped.append(f"{fn.__name__}: {e}")
        print(f"SKIP ({e})")
        return
    except Exception as e:   # an assertion, or anything else the check tripped on
        failed += 1
        failures.append(f"{fn.__name__}: {e}")
        print(f"FAIL\n    {e}")
        return
    passed += 1
    print("PASS")


def gltf_json(path):
    """The JSON chunk of a .glb (chunk 0)."""
    data = open(path, "rb").read()
    magic, version, length = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF" and version == 2 and length == len(data), f"{path}: not a glTF 2 binary"
    clen, ctype = struct.unpack_from("<I4s", data, 12)
    assert ctype == b"JSON", f"{path}: chunk 0 is not JSON"
    return json.loads(data[20:20 + clen])


def l1_every_checked_in_level_has_valid_hints():
    schema = json.load(open(SCHEMA))
    jsonschema.Draft202012Validator.check_schema(schema)
    levels = sorted(glob.glob(os.path.join(ROOT, "assets", "levels", "*.glb")))
    assert levels, "no checked-in level under assets/levels"
    for path in levels:
        for i, node in enumerate(gltf_json(path)["nodes"]):
            if "extras" in node:
                try:
                    jsonschema.validate(node["extras"], schema)
                except jsonschema.ValidationError as e:
                    raise AssertionError(f"{os.path.basename(path)} node {i} ({node.get('name')}): {e.message}")


# The blockout's contract: name -> (translation, extras). Later steps' collision and spawn tests
# assert against these numbers (docs/plans/2026-10-26-level-pipeline.md, Task 1).
BLOCKOUT = {
    "Floor": ([0, -0.1, 0], {"col": "box"}),
    "WallNorth": ([0, 1.5, -3.1], {"col": "box"}),
    "WallEast": ([4.1, 1.5, 0], {"col": "box"}),
    "WallWest": ([-4.1, 1.5, 0], {"col": "box"}),
    "WallSouthA": ([-1.65, 1.5, 3.1], {"col": "box"}),
    "WallSouthB": ([3.15, 1.5, 3.1], {"col": "box"}),
    "WallSouthLintel": ([1.5, 2.6, 3.1], {"col": "box"}),
    "Step": ([-2.25, 0.15, -2], {"col": "box"}),
    "Platform": ([-3.5, 0.3, -2], {"col": "box"}),
    "Ramp": ([2.7, 0.3, -1], {"col": "mesh"}),
    "Pillar": ([0, 1.5, -0.5], {"col": "box"}),
    "CrateA": ([1.3, 0.3, -1.7], {"col": "box", "body": "dynamic", "mass": 20.0}),
    "CrateB": ([1.3, 0.8, -1.7], {"col": "box", "body": "dynamic", "mass": 8.0}),
    "SpawnPlayer": ([1.5, 0, 2.2], {"spawn": "player"}),
    "SpawnEnemyA": ([-2.5, 0.3, -2], {"spawn": "enemy"}),
    "SpawnEnemyB": ([3, 0.6, -2], {"spawn": "enemy"}),
    "LevelCamera": ([1.5, 1.7, 5.5], None),
}


def l1_blockout_hints_are_the_contract():
    doc = gltf_json(LEVEL)
    nodes = {n.get("name"): n for n in doc["nodes"]}
    assert sorted(nodes) == sorted(BLOCKOUT), f"node names {sorted(nodes)}"
    for name, (t, extras) in BLOCKOUT.items():
        n = nodes[name]
        got = n.get("translation", [0, 0, 0])
        assert all(abs(a - b) < 1e-5 for a, b in zip(got, t)), f"{name}: translation {got}, expected {t}"
        assert n.get("extras") == extras, f"{name}: extras {n.get('extras')}, expected {extras}"
    assert len(doc["meshes"]) == 13, f"{len(doc['meshes'])} meshes"
    assert "camera" in nodes["LevelCamera"], "LevelCamera has no camera"
    assert not doc.get("extensionsUsed"), f"extensions {doc.get('extensionsUsed')}"


def blender():
    for c in (os.environ.get("OHAO_BLENDER"), r"C:/Users/djmax/scoop/shims/blender.exe", shutil.which("blender")):
        if c and os.path.exists(c):
            return c
    return None


def l1_blockout_rebuilds_byte_identical():
    exe = blender()
    if not exe:
        raise Skip("Blender not found (OHAO_BLENDER, the scoop shim or PATH)")
    out_dir = os.path.join(ROOT, "build", "level_check")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"blockout_room_{os.getpid()}.glb")
    script = os.path.join(ROOT, "tools", "level", "blockout_room.py")
    r = subprocess.run([exe, "-b", "--factory-startup", "--python", script, "--", out],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0 and os.path.exists(out), f"Blender exit {r.returncode}: {r.stderr[-400:]}"
    ver = next((line for line in r.stdout.splitlines() if line.startswith("Blender ")), "Blender ?")
    rebuilt, checked_in = open(out, "rb").read(), open(LEVEL, "rb").read()
    os.remove(out)
    if not ver.startswith("Blender 5.2.2"):
        raise Skip(f"{ver.strip()}: the checked-in file was made by Blender 5.2.2")
    assert rebuilt == checked_in, f"rebuild differs ({len(rebuilt)} vs {len(checked_in)} bytes): regenerate and look"


def l1_blockout_scene_references_the_level():
    scene = json.load(open(SCENE))
    paths = [e["MeshSource"]["source"]["ModelAsset"]["asset"]["path"] for e in scene["entities"]
             if "ModelAsset" in e.get("MeshSource", {}).get("source", {})]
    assert paths == ["assets/levels/blockout_room.glb"], f"model paths {paths}"
    assert any("Camera" in e for e in scene["entities"]), "no camera"


def main():
    for fn in (l1_every_checked_in_level_has_valid_hints, l1_blockout_hints_are_the_contract,
               l1_blockout_rebuilds_byte_identical, l1_blockout_scene_references_the_level):
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed, {len(skipped)} skipped")
    print(json.dumps({"suite": "check_level_assets", "passed": passed, "failed": failed,
                      "failures": failures, "skipped": skipped}))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
