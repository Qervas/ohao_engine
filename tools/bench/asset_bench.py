"""asset_bench: what loading a scene's assets costs (docs/plans/2026-11-11-step5-assets.md, 5a).

    python tools/bench/asset_bench.py [--only NAME,...] [--out build/asset_bench.json]

For each scene a new `ohao serve` (a cold engine; the OS file cache is warm after the first run): the time from the
process's start to the scene loaded (a level expanded and its colliders built, which imports their meshes; no GPU
yet) and to its first frame (640x360, deferred: every drawn asset imported, uploaded and its BLAS built), then what
`render.stats` says the assets hold on the GPU, and the process's resident memory, now and at its peak. One process
at a time; a tool run by hand. The first run after a build is about 2 s slower on every scene (the driver's
caches): read the second.
kit2k is generated first when it is missing (tools/level/kit_level.py); flat42 is skipped when it is not staged.

    python tools/bench/asset_bench.py --loop N

One engine loads Sponza, the helmet and the Cornell box in turn, N times, a frame each; after each round, with the
Cornell box loaded, what the GPU holds (textures, meshes, BLASes, materials, their bytes) must be what it was after
the first Cornell box, and resident memory must not climb: exit 1 otherwise (docs/plans/2026-11-11-step5-assets.md,
5d).
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "common"))
from ohao_serve import Serve  # noqa: E402

SCENES = {
    "play": "assets/scenes/blockout_play.scene.json",
    "helmet": "assets/scenes/damaged_helmet.scene.json",
    "sponza": "assets/scenes/khronos_sponza.scene.json",
    "flat42": "assets/local/blockout_flat42_wide.scene.json",
    "kit2k": "build/kit2k/kit2k.scene.json",
}


def resident(pid):
    """(now, peak) in bytes, from /proc."""
    now = peak = 0
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                now = int(line.split()[1]) * 1024
            elif line.startswith("VmHWM:"):
                peak = int(line.split()[1]) * 1024
    return now, peak


def measure(name, scene):
    out = "build/asset_bench/" + name
    start = time.perf_counter()
    with Serve(out, ["--log-level", "warn"]) as s:
        s.load(scene)
        loaded = time.perf_counter()
        s.call("render.capture", {"path": out + "/frame.png", "width": 640, "height": 360})
        frame = time.perf_counter()
        memory = s.call("render.stats", {"passes": False})["memory"]
        now, peak = resident(s.proc.pid)
    return {"scene": scene, "loadMs": round((loaded - start) * 1e3, 1), "firstFrameMs": round((frame - start) * 1e3, 1),
            **memory, "residentBytes": now, "residentPeakBytes": peak}


LOOP = ["sponza", "helmet"]
REST = "assets/scenes/cornell_box.scene.json"
HELD = ["textures", "textureBytes", "meshes", "meshBytes", "blas", "blasBytes", "materials"]
RESIDENT_SLACK = 16 << 20   # the allocator's and the driver's own; a leaked Sponza is 250 MB a round


def loop(rounds):
    out = "build/asset_bench/loop"
    rows = []
    with Serve(out, ["--log-level", "warn"]) as s:
        def rest():
            s.load(REST)
            for _ in range(4):   # the frames in flight retire what the last scene held
                s.call("render.capture", {"path": out + "/frame.png", "width": 640, "height": 360})
            memory = s.call("render.stats", {"passes": False})["memory"]
            return {**{k: memory[k] for k in HELD}, "residentBytes": resident(s.proc.pid)[0]}
        rows.append(rest())
        for _ in range(rounds):
            arrive = {}
            for name in LOOP:
                # A scene arriving in a running engine: its load and the first frame that shows it.
                start = time.perf_counter()
                s.load(SCENES[name])
                s.call("render.capture", {"path": out + "/frame.png", "width": 640, "height": 360})
                arrive[name] = (time.perf_counter() - start) * 1e3
            rows.append({**rest(), "arrive": arrive})
    for i, r in enumerate(rows):
        print("round %2d  textures %3d %7.1f MB  meshes %3d %6.1f MB  blas %3d %6.1f MB  materials %4d  resident %6.1f MB"
              % (i, r["textures"], r["textureBytes"] / 1048576.0, r["meshes"], r["meshBytes"] / 1048576.0, r["blas"],
                 r["blasBytes"] / 1048576.0, r["materials"], r["residentBytes"] / 1048576.0)
              + "".join("  %s %.0f ms" % (n, ms) for n, ms in r.get("arrive", {}).items()))
    failures = ["%s: %d at rest, %d after round %d" % (k, rows[0][k], r[k], i)
                for i, r in enumerate(rows) for k in HELD if r[k] != rows[0][k]][:8]
    # Resident memory settles after the first round (caches fill once): it must not climb from there.
    if len(rows) > 2 and rows[-1]["residentBytes"] > rows[1]["residentBytes"] + RESIDENT_SLACK:
        failures.append("resident memory: %.1f MB after round 1, %.1f MB after round %d"
                        % (rows[1]["residentBytes"] / 1048576.0, rows[-1]["residentBytes"] / 1048576.0, len(rows) - 1))
    print(json.dumps({"suite": "asset_bench.loop", "passed": 0 if failures else 1, "failed": len(failures),
                      "failures": failures}))
    return 1 if failures else 0


def main(argv):
    only, out = None, "build/asset_bench.json"
    for i, a in enumerate(argv):
        if a == "--loop":
            sys.exit(loop(int(argv[i + 1])))
        if a == "--only":
            only = argv[i + 1].split(",")
        elif a == "--out":
            out = argv[i + 1]
    if not os.path.exists(os.path.join(ROOT, SCENES["kit2k"])) and (only is None or "kit2k" in only):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/level/kit_level.py")], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
    results = {}
    for name, scene in SCENES.items():
        if only is not None and name not in only:
            continue
        if not os.path.exists(os.path.join(ROOT, scene)):
            print("%-7s skipped: %s is not there" % (name, scene))
            continue
        r = results[name] = measure(name, scene)
        mb = lambda b: b / 1048576.0  # noqa: E731
        print("%-7s load %8.1f ms  first frame %8.1f ms  textures %3d %7.1f MB  meshes %6.1f MB  blas %4d %6.1f MB  "
              "resident %6.1f MB (peak %6.1f)" % (name, r["loadMs"], r["firstFrameMs"], r["textures"],
                                                    mb(r["textureBytes"]), mb(r["meshBytes"]), r["blas"],
                                                    mb(r["blasBytes"]), mb(r["residentBytes"]), mb(r["residentPeakBytes"])))
    with open(os.path.join(ROOT, out), "w") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "asset_bench", "out": out, "scenes": list(results)}))


if __name__ == "__main__":
    main(sys.argv[1:])
