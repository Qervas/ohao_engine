#!/usr/bin/env python3
"""check_level_assets -- the level pipeline's checked-in assets (sub-project 5, test 1).

    python tests/level/check_level_assets.py

Last line is a JSON summary; exit 0 = pass. A check that needs Blender reports SKIP
without it (a skip is neither a pass nor a failure, and is listed in the summary).
  l1_* every assets/levels/*.glb node's extras validate against tests/level/extras_schema.json;
  the blockout's nodes and hints are the contract later steps assert against; a Blender
  rebuild of the blockout (tools/level/blockout_room.py) is byte-identical to the checked-in
  file; the blockout scene references it. Needs `jsonschema`.
  a3_* the player's arms and rifle (sub-project 10 step 3): a Blender rebuild
  (tools/anim/make_fps_arms.py) is byte-identical to assets/characters/fps_arms/fps_arms.glb.
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
ARMS = os.path.join(ROOT, "assets", "characters", "fps_arms", "fps_arms.glb")

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
    "Floor": ([0, -0.1, 0], {"col": "box", "surf": "concrete"}),
    # The outdoor ground: 80 x 80 m, its top 5 cm below the Floor's (audio spec ruling 21).
    "Ground": ([0, -0.15, 0], {"col": "box", "surf": "concrete"}),
    "WallNorth": ([0, 1.5, -3.1], {"col": "box"}),
    "WallEast": ([4.1, 1.5, 0], {"col": "box"}),
    "WallWest": ([-4.1, 1.5, 0], {"col": "box"}),
    "WallSouthA": ([-1.65, 1.5, 3.1], {"col": "box"}),
    "WallSouthB": ([3.15, 1.5, 3.1], {"col": "box"}),
    "WallSouthLintel": ([1.5, 2.6, 3.1], {"col": "box"}),
    "Step": ([-2.25, 0.15, -2], {"col": "box", "surf": "metal"}),
    "Platform": ([-3.5, 0.3, -2], {"col": "box", "surf": "metal"}),
    "Ramp": ([2.7, 0.3, -1], {"col": "mesh", "surf": "metal"}),
    "Pillar": ([0, 1.5, -0.5], {"col": "box", "surf": "metal"}),
    "CrateA": ([1.3, 0.3, -1.7], {"col": "box", "body": "dynamic", "mass": 20.0, "surf": "wood"}),
    "CrateB": ([1.3, 0.8, -1.7], {"col": "box", "body": "dynamic", "mass": 8.0, "surf": "wood"}),
    "SpawnPlayer": ([1.5, 0, 2.2], {"spawn": "player"}),
    "SpawnEnemyA": ([-2.5, 0.3, -2], {"spawn": "enemy"}),
    "SpawnEnemyB": ([3, 0.5, -2], {"spawn": "enemy"}),   # on the ramp (step 4: was 0.6, 0.1 m above it)
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
    assert len(doc["meshes"]) == 14, f"{len(doc['meshes'])} meshes"
    assert "camera" in nodes["LevelCamera"], "LevelCamera has no camera"
    assert not doc.get("extensionsUsed"), f"extensions {doc.get('extensionsUsed')}"


def blender():
    for c in (os.environ.get("OHAO_BLENDER"), r"C:/Users/djmax/scoop/shims/blender.exe", shutil.which("blender")):
        if c and os.path.exists(c):
            return c
    return None


def rebuild_byte_identical(script, checked_in_path):
    """Runs a Blender generator into build/level_check and compares its bytes with the checked-in file."""
    exe = blender()
    if not exe:
        raise Skip("Blender not found (OHAO_BLENDER, the scoop shim or PATH)")
    out_dir = os.path.join(ROOT, "build", "level_check")
    os.makedirs(out_dir, exist_ok=True)
    name = os.path.splitext(os.path.basename(checked_in_path))[0]
    out = os.path.join(out_dir, f"{name}_{os.getpid()}.glb")
    r = subprocess.run([exe, "-b", "--factory-startup", "--python", os.path.join(ROOT, script), "--", out],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0 and os.path.exists(out), f"Blender exit {r.returncode}: {r.stderr[-400:]}"
    ver = next((line for line in r.stdout.splitlines() if line.startswith("Blender ")), "Blender ?")
    rebuilt, checked_in = open(out, "rb").read(), open(checked_in_path, "rb").read()
    os.remove(out)
    if not ver.startswith("Blender 5.2.2"):
        raise Skip(f"{ver.strip()}: the checked-in file was made by Blender 5.2.2")
    assert rebuilt == checked_in, f"rebuild differs ({len(rebuilt)} vs {len(checked_in)} bytes): regenerate and look"


def l1_blockout_rebuilds_byte_identical():
    rebuild_byte_identical(os.path.join("tools", "level", "blockout_room.py"), LEVEL)


def a3_fps_arms_rebuild_byte_identical():
    # Sub-project 10 step 3: the player's arms and rifle, made by our Blender script.
    rebuild_byte_identical(os.path.join("tools", "anim", "make_fps_arms.py"), ARMS)


def l1_blockout_scene_references_the_level():
    # Step 3: the level is a Level component (one entity per node at load); nothing loads
    # the file as a merged ModelAsset any more.
    scene = json.load(open(SCENE))
    paths = [e["Level"]["asset"]["path"] for e in scene["entities"] if "Level" in e]
    assert paths == ["assets/levels/blockout_room.glb"], f"level paths {paths}"
    assert not any("MeshSource" in e for e in scene["entities"]), "the level is also loaded as a mesh"
    assert any("Camera" in e for e in scene["entities"]), "no camera"


AUDIO_FIXTURE = os.path.join(ROOT, "tests", "level", "fixtures", "level_audio.gltf")


def t35_vfx_hint_schema():
    # Sub-project 12 step 4: the `vfx` hint (a placed effect's name) is in the extras schema; a name is a
    # non-empty string of at most 32 characters, and anything else does not validate.
    schema = json.load(open(SCHEMA))
    assert "vfx" in schema.get("properties", {}), "the extras schema has no vfx"
    jsonschema.validate({"vfx": "smoke_cloud"}, schema)
    for bad in ({"vfx": 3}, {"vfx": ""}, {"vfx": "x" * 33}):
        try:
            jsonschema.validate(bad, schema)
        except jsonschema.ValidationError:
            continue
        raise AssertionError(f"{bad} validates")


def t27_verb_hint_schema():
    # Sub-project 11 step 2: the `verb` hint (a ReverbZone's preset) is in the extras schema; the
    # reverb-zone fixture's nodes validate; a verb that is not a non-empty string does not.
    schema = json.load(open(SCHEMA))
    assert "verb" in schema.get("properties", {}), "the extras schema has no verb"
    for i, node in enumerate(json.load(open(AUDIO_FIXTURE))["nodes"]):
        try:
            jsonschema.validate(node.get("extras", {}), schema)
        except jsonschema.ValidationError as e:
            raise AssertionError(f"level_audio.gltf node {i} ({node.get('name')}): {e.message}")
    for bad in ({"verb": 3}, {"verb": ""}):
        try:
            jsonschema.validate(bad, schema)
        except jsonschema.ValidationError:
            continue
        raise AssertionError(f"{bad} validates")
    # The fixture is generated: tests/level/make_audio_fixture.py reproduces it byte for byte.
    out_dir = os.path.join(ROOT, "build", "level_check")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"level_audio_{os.getpid()}.gltf")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "level", "make_audio_fixture.py"), out],
                       capture_output=True, text=True, cwd=ROOT, timeout=60)
    assert r.returncode == 0, f"make_audio_fixture.py exit {r.returncode}: {r.stderr[-400:]}"
    rebuilt, checked_in = open(out, "rb").read(), open(AUDIO_FIXTURE, "rb").read()
    os.remove(out)
    assert rebuilt == checked_in, "level_audio.gltf differs from what make_audio_fixture.py writes: regenerate it"


def t32_alpha_card_rebuilds_byte_identical():
    # Sub-project 12 step 2: the transparency scene's MASK leaf card is generated (pure Python, fixed
    # numbers): tools/vfx/make_alpha_card.py --check compares what it writes with the checked-in file.
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "vfx", "make_alpha_card.py"), "--check"],
                       capture_output=True, text=True, cwd=ROOT, timeout=60)
    assert r.returncode == 0, f"assets/vfx/alpha_card.gltf differs from make_alpha_card.py's: {r.stdout[-300:]}"


def main():
    for fn in (l1_every_checked_in_level_has_valid_hints, l1_blockout_hints_are_the_contract,
               l1_blockout_rebuilds_byte_identical, l1_blockout_scene_references_the_level,
               a3_fps_arms_rebuild_byte_identical, t27_verb_hint_schema, t32_alpha_card_rebuilds_byte_identical,
               t35_vfx_hint_schema):
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed, {len(skipped)} skipped")
    print(json.dumps({"suite": "check_level_assets", "passed": passed, "failed": failed,
                      "failures": failures, "skipped": skipped}))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
