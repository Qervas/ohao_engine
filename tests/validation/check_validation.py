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

One validated `ohao serve` renders every case in turn; messages are attributed to the case whose
capture was running, and the process's teardown is a case of its own.

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
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "common"))
from ohao_serve import ROOT, Serve  # noqa: E402

OUT = "build/validation"
W, H = 320, 180   # validation checks what is recorded, not how many pixels

# (name, scene, steps before the capture, render.capture params). One validated `ohao serve` renders them all in
# turn: the layer and the pipelines load once, and the process's teardown is validated at the end.
C, HELMET, RIG, PLAY = ("assets/scenes/%s.scene.json" % n for n in ("cornell_box", "damaged_helmet", "anim_rig",
                                                                    "blockout_play"))
OFFLINE, REALTIME = {"mode": "rt_offline", "spp": 4, "denoise": "none"}, {"mode": "rt_realtime", "spp": 4, "denoise": "none"}
NRD, DLSS = {**REALTIME, "denoise": "nrd"}, {**REALTIME, "spp": 8, "denoise": "dlss"}
DEFERRED = {"mode": "deferred", "frames": 4}
SCENES = [
    ("cornell_box deferred", C, 0, DEFERRED),
    ("damaged_helmet deferred", HELMET, 0, DEFERRED),
    # The path tracer: offline, realtime, and realtime through NRD (its push-descriptor path) and DLSS RR (where
    # the SDK is absent, tools/fetch_dlss.py, dlss falls back to none and the case still runs).
    ("cornell_box rt_offline", C, 0, OFFLINE),
    ("cornell_box rt_realtime", C, 0, REALTIME),
    ("cornell_box rt_realtime nrd", C, 0, NRD),
    ("cornell_box rt_realtime dlss", C, 0, DLSS),
    ("damaged_helmet rt_offline", HELMET, 0, OFFLINE),
    # A skinned instance (sub-project 10): the skin, bounds and BLAS refit passes, then every consumer of the
    # deformed mesh (G-buffer, RT shadows and GI, the path tracer, NRD).
    ("anim_rig deferred", RIG, 0, DEFERRED),
    ("anim_rig rt_offline", RIG, 0, OFFLINE),
    ("anim_rig rt_realtime", RIG, 0, REALTIME),
    ("anim_rig rt_realtime nrd", RIG, 0, NRD),
    # The play scene's two skinned enemies a second into the fight: attacking, their rifles and muzzle lights in
    # hand, through every renderer, the HUD on the raster frame.
    ("play deferred", PLAY, 60, {**DEFERRED, "hud": True}),
    ("play rt_offline", PLAY, 60, OFFLINE),
    ("play rt_realtime", PLAY, 60, REALTIME),
    ("play rt_realtime nrd", PLAY, 60, NRD),
    ("play rt_realtime dlss", PLAY, 60, DLSS),
    # Caustics' light pass (its own pipeline, the splat buffer and its barriers).
    ("caustics rt_offline", "assets/scenes/caustics.scene.json", 0, OFFLINE),
    # The doorway's smoke (a VfxEmitter's spheres in the shadow and GI traces, its particles and the forward pass).
    ("smoke doorway deferred", "assets/scenes/smoke_doorway.scene.json", 60, {**DEFERRED, "hud": True}),
    ("smoke doorway rt_offline", "assets/scenes/smoke_doorway.scene.json", 60, OFFLINE),
    ("smoke doorway rt_realtime", "assets/scenes/smoke_doorway.scene.json", 60, REALTIME),
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


def record(name, msgs, error, verbose):
    """-> (ok, message, record) for one case: its validation messages, or the error its capture raised."""
    counts = {}
    for v, _ in msgs:
        counts[v] = counts.get(v, 0) + 1
    rec = {"name": name, "messages": len(msgs), "distinct": len(counts)}
    if msgs:
        rec["first"] = {"vuid": msgs[0][0].strip(), "text": msgs[0][1].strip()[:260]}
        rec["counts"] = dict(sorted(counts.items(), key=lambda kv: -kv[1])[:12])
    if error:
        rec.update(status="fail", error=error)
        return False, f"{name}: {error} ({len(msgs)} validation messages)", rec
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


def run_all(verbose):
    """Every case in one validated serve process, then its teardown: [(ok, message, record)], or None when the
    layer did not load."""
    log_path = os.path.join(ROOT, OUT, "serve.stderr.txt")
    def log_from(offset):
        with open(log_path, "rb") as f:
            f.seek(offset)
            return f.read().decode(errors="replace")
    results = []
    s = Serve(OUT, env=dict(os.environ, OHAO_VALIDATION="1"))
    try:
        for name, scene, steps, params in SCENES:
            start = os.path.getsize(log_path)
            error = None
            try:
                s.load(scene)
                if steps:
                    s.call("world.step", {"frames": steps})
                s.call("render.capture", {"path": OUT + "/out.png", "width": W, "height": H, **params})
            except RuntimeError as e:
                error = str(e)[-200:]
            results.append(record(name, messages(log_from(start)), error, verbose))
    finally:
        start = os.path.getsize(log_path)
        code = s.close()
    if "Validation layers ENABLED" not in log_from(0):
        return None
    teardown = messages(log_from(start))
    results.append(record("teardown", teardown, None if code == 0 else f"ohao serve exited {code}", verbose))
    return results


def main():
    verbose = "-v" in sys.argv
    json_path = None
    if "--json" in sys.argv:
        i = sys.argv.index("--json")
        if i + 1 >= len(sys.argv):
            sys.exit("--json needs a file")
        json_path = sys.argv[i + 1]
    results = run_all(verbose)
    if results is None:
        print("[SKIP]  validation layer did not load -- check NOT run")
        print(json.dumps({"suite": "check_validation", "passed": 0, "failed": 1,
                          "failures": ["the validation layer did not load"]}, separators=(",", ":")))
        return 2
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
