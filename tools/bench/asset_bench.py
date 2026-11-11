"""asset_bench: what loading a scene's assets costs (docs/plans/2026-11-11-step5-assets.md, 5a).

    python tools/bench/asset_bench.py [--only NAME,...] [--out build/asset_bench.json]

For each scene a new `ohao serve` (a cold engine; the OS file cache is warm after the first run): the time from the
process's start to the scene loaded (a level expanded and its colliders built, which imports their meshes; no GPU
yet) and to its first frame (640x360, deferred: every drawn asset imported, uploaded and its BLAS built), then what
`render.stats` says the assets hold on the GPU, and the process's resident memory, now and at its peak. One process
at a time; a tool run by hand. The first run after a build is about 2 s slower on every scene (the driver's
caches): read the second.
kit2k is generated first when it is missing (tools/level/kit_level.py); flat42 is skipped when it is not staged.
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


def main(argv):
    only, out = None, "build/asset_bench.json"
    for i, a in enumerate(argv):
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
