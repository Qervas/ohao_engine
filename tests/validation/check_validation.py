#!/usr/bin/env python3
"""
check-validation -- run real scenes under VK_LAYER_KHRONOS_validation and fail on
any validation message (warning or error).

Unit tests exercise passes in isolation; this exercises whole frames the way the
engine records them -- scene upload, G-buffer, CSM, RT shadows, RT GI, lighting,
post -- which is where object-lifetime bugs live. One object destroyed while a
command buffer still references it invalidates the whole buffer and the layer
then reports every later command, so the FIRST message is the one to read; this
script prints it.

The engine installs its own debug messenger (OHAO_VALIDATION=1), which replaces
the layer's default printing. Every message goes to the logger's `validation`
channel and arrives on stderr as
    W validation: <VUID> | <message>        (E for an error-severity message)
with identical consecutive messages folded into one line followed by
    W validation: ... repeated <N> more times
and the engine confirms the layer loaded with "Validation layers ENABLED". A run
without that confirmation did not check anything and is reported as such.

Usage (from repo root):
  python tests/validation/check_validation.py        # all scenes
  python tests/validation/check_validation.py -v     # also list every distinct VUID
  python tests/validation/check_validation.py --json FILE   # also one object per scene

The --json object: {"suite": "check_validation", "passed", "failed", "skipped", "scenes":
[{name, status: pass|fail|skip, messages, distinct, first?: {vuid, text}, counts?, exit?}]}.
The last stdout line is the shared JSON summary {"suite", "passed", "failed", "failures"}.

Exit 0 when every scene is clean, 1 otherwise, 2 if validation did not run.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

SCENES = [
    ("cornell_box deferred",
     ["build/Release/cornell_box.exe", "{out}", "4", "deferred", "--denoise=none"]),
    ("damaged_helmet deferred",
     ["build/Release/model_viewer.exe", "assets/test_models/DamagedHelmet.glb", "{out}", "4",
      "deferred", "--denoise=none"]),
    # The path tracer: offline, realtime, and realtime through NRD (its push-descriptor path) and DLSS RR.
    ("cornell_box rt_offline",
     ["build/Release/cornell_box.exe", "{out}", "4", "rt_offline", "--denoise=none"]),
    ("cornell_box rt_realtime",
     ["build/Release/cornell_box.exe", "{out}", "4", "rt_realtime", "--denoise=none"]),
    ("cornell_box rt_realtime nrd",
     ["build/Release/cornell_box.exe", "{out}", "4", "rt_realtime", "--denoise=nrd"]),
    # DLSS Ray Reconstruction (NGX): its guides, the feature's evaluation and its tonemap. Where the SDK
    # is absent (tools/fetch_dlss.py) --denoise dlss falls back to none and the case still runs.
    ("cornell_box rt_realtime dlss",
     ["build/Release/ohao.exe", "run", "assets/scenes/cornell_box.scene.json", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "8", "--denoise", "dlss", "--size", "640x360"]),
    ("damaged_helmet rt_offline",
     ["build/Release/model_viewer.exe", "assets/test_models/DamagedHelmet.glb", "{out}", "4",
      "rt_offline", "--denoise=none"]),
    # A skinned instance (sub-project 10): the skin, bounds and BLAS refit passes, then every
    # consumer of the deformed mesh (G-buffer, RT shadows and GI, the path tracer, NRD).
    ("anim_rig deferred",
     ["build/Release/ohao.exe", "run", "assets/scenes/anim_rig.scene.json", "--render", "{out}",
      "--mode", "deferred", "--frames", "4", "--size", "640x360"]),
    ("anim_rig rt_offline",
     ["build/Release/ohao.exe", "run", "assets/scenes/anim_rig.scene.json", "--render", "{out}",
      "--mode", "rt_offline", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
    ("anim_rig rt_realtime",
     ["build/Release/ohao.exe", "run", "assets/scenes/anim_rig.scene.json", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
    ("anim_rig rt_realtime nrd",
     ["build/Release/ohao.exe", "run", "assets/scenes/anim_rig.scene.json", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "4", "--denoise", "nrd", "--size", "640x360"]),
    # The play scene's two skinned enemies (sub-project 10 step 2) a second into the fight: attacking,
    # their rifles and muzzle lights in hand, through every renderer.
    ("play deferred",
     ["build/Release/ohao.exe", "run", "assets/scenes/blockout_play.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "deferred", "--frames", "4", "--size", "640x360", "--hud"]),
    ("play rt_offline",
     ["build/Release/ohao.exe", "run", "assets/scenes/blockout_play.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_offline", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
    ("play rt_realtime",
     ["build/Release/ohao.exe", "run", "assets/scenes/blockout_play.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
    ("play rt_realtime nrd",
     ["build/Release/ohao.exe", "run", "assets/scenes/blockout_play.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "4", "--denoise", "nrd", "--size", "640x360"]),
    ("play rt_realtime dlss",
     ["build/Release/ohao.exe", "run", "assets/scenes/blockout_play.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "8", "--denoise", "dlss", "--size", "640x360"]),
    # sub-project 24: caustics' light pass (its own pipeline, the splat buffer and its barriers).
    ("caustics rt_offline",
     ["build/Release/ohao.exe", "run", "assets/scenes/caustics.scene.json", "--render", "{out}",
      "--mode", "rt_offline", "--spp", "4", "--denoise", "none", "--size", "640x360", "--caustics"]),
    # sub-project 12: the doorway's smoke (a VfxEmitter's spheres in the shadow and GI traces, its particles
    # and the forward pass), through the raster and both path-traced modes.
    ("smoke doorway deferred",
     ["build/Release/ohao.exe", "run", "assets/scenes/smoke_doorway.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "deferred", "--frames", "4", "--size", "640x360", "--hud"]),
    ("smoke doorway rt_offline",
     ["build/Release/ohao.exe", "run", "assets/scenes/smoke_doorway.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_offline", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
    ("smoke doorway rt_realtime",
     ["build/Release/ohao.exe", "run", "assets/scenes/smoke_doorway.scene.json", "--steps", "60", "--render", "{out}",
      "--mode", "rt_realtime", "--spp", "4", "--denoise", "none", "--size", "640x360"]),
]

LINE = re.compile(r"^[TDIWE] validation: (.*?)\r?$", re.M)
MSG = re.compile(r"^([^|]*?) \| (.*)$")
REPEAT = re.compile(r"^\.\.\. repeated (\d+) more times$")


def messages(log):
    """-> [(vuid, text)], one per message, repeats unfolded."""
    out = []
    for body in LINE.findall(log):
        m = MSG.match(body)
        if m:
            out.append((m.group(1), m.group(2)))
            continue
        r = REPEAT.match(body)
        if r and out:
            out.extend([out[-1]] * int(r.group(1)))
    return out


def run(name, argv, verbose):
    """-> (ok, message, record): ok is None when the layer did not load."""
    with tempfile.TemporaryDirectory() as tmp:
        argv = [a.replace("{out}", os.path.join(tmp, "out.png")) for a in argv]
        argv[0] = os.path.abspath(os.path.normpath(argv[0]))   # CreateProcess wants a real path
        env = dict(os.environ, OHAO_VALIDATION="1")
        r = subprocess.run(argv, env=env, capture_output=True, text=True, errors="replace")
    log = r.stdout + r.stderr
    if "Validation layers ENABLED" not in log:
        return None, f"{name}: validation layer did not load -- check NOT run", {"name": name, "status": "skip"}
    msgs = messages(log)
    counts = {}
    for v, _ in msgs:
        counts[v] = counts.get(v, 0) + 1
    rec = {"name": name, "messages": len(msgs), "distinct": len(counts)}
    if msgs:
        rec["first"] = {"vuid": msgs[0][0].strip(), "text": msgs[0][1].strip()[:260]}
        rec["counts"] = dict(sorted(counts.items(), key=lambda kv: -kv[1])[:12])
    if r.returncode != 0:
        rec.update(status="fail", exit=r.returncode)
        return False, f"{name}: exited {r.returncode} ({len(msgs)} validation messages)", rec
    if not msgs:
        rec["status"] = "pass"
        return True, f"{name}: clean", rec
    rec["status"] = "fail"
    vuid, text = msgs[0]
    nl = chr(10)
    out = (f"{name}: {len(msgs)} validation messages, {len(counts)} distinct{nl}"
           f"      first: {vuid}{nl}             {text.strip()[:260]}")
    if verbose:
        out += nl + nl.join(f"      {n:4d}  {v}" for v, n in sorted(counts.items(), key=lambda kv: -kv[1]))
    return False, out, rec


def main():
    verbose = "-v" in sys.argv
    json_path = None
    if "--json" in sys.argv:
        i = sys.argv.index("--json")
        if i + 1 >= len(sys.argv):
            sys.exit("--json needs a file")
        json_path = sys.argv[i + 1]
    results = [run(n, a, verbose) for n, a in SCENES]
    for ok, msg, _ in results:
        tag = "PASS" if ok else ("SKIP" if ok is None else "FAIL")
        print(f"[{tag}]  {msg}")
    records = [rec for _, _, rec in results]
    passed = sum(1 for r in records if r["status"] == "pass")
    failed = sum(1 for r in records if r["status"] == "fail")
    skipped = sum(1 for r in records if r["status"] == "skip")
    if json_path:
        os.makedirs(os.path.dirname(os.path.abspath(json_path)), exist_ok=True)
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump({"suite": "check_validation", "passed": passed, "failed": failed, "skipped": skipped,
                       "scenes": records}, f, indent=1)
    print(json.dumps({"suite": "check_validation", "passed": passed, "failed": failed + skipped,
                      "failures": [r["name"] for r in records if r["status"] != "pass"]}, separators=(",", ":")))
    if skipped:
        return 2
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
