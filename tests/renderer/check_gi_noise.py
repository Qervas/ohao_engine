"""RT GI's noise on a flat wall: the helmet room (assets/scenes/helmet_game.scene.json), whose walls are untextured.

The deferred frame at realtime quality (RT GI at half resolution, 2 rays a pixel, its denoiser) against the same
frame at reference quality (full resolution, no denoiser, the running mean of REFERENCE_FRAMES frames), in linear
radiance (`ohao run --hdr`), on a 240x240 patch of the left wall. Blotchiness is the low-frequency mottling a still
camera keeps: the standard deviation of (9x9 box - 41x41 box) over the patch's mean.

  frame 10      blotchiness under BLOTCH_10
  frame 60      blotchiness under BLOTCH_60, the patch's mean within MEAN_TOL of the reference's

What it guards: a denoiser whose spatial filter is too narrow for the trace's noise (two a-trous steps left blotches
some 20 pixels wide: 4.1% at frame 60), and a history clamp that darkens (clamped to the short history's range the
wall settled at 0.897 of the reference). The fix measured 0.80% at frame 10, 0.41% at frame 60, and 0.981 of the
reference's mean (docs/changes/2026-10-31-gi-denoise.md); the reference itself measures 0.5%.

Writes build/gi_noise/: each capture's PNG and PFM, and wall.png (frame 10 | frame 60 | reference, the patch at 1:1).

  python tests/renderer/check_gi_noise.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import os
import subprocess
import sys

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_parity import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe")
SCENE = "assets/scenes/helmet_game.scene.json"
OUT = "build/gi_noise"
SIZE = "1280x720"
WALL = (slice(100, 340), slice(40, 280))   # rows, columns: the left wall, nothing else
REFERENCE_FRAMES = 256
BLOTCH_10 = 0.015
BLOTCH_60 = 0.01
MEAN_TOL = 0.03


def lum(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def capture(name, args):
    png, pfm = "%s/%s.png" % (OUT, name), "%s/%s.pfm" % (OUT, name)
    cmd = [OHAO, "run", SCENE, "--render", png, "--hdr", pfm, "--mode", "deferred", "--size", SIZE] + args
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (name, r.returncode, r.stderr[-2000:]))
    return lum(read_pfm(os.path.join(ROOT, pfm)))[WALL]


def box(a, k):
    return sliding_window_view(np.pad(a, k // 2, mode="edge"), (k, k)).mean(axis=(2, 3))


def blotchiness(a):
    return float((box(a, 9) - box(a, 41))[20:-20, 20:-20].std() / a.mean())


def sheet(names):
    tiles = [Image.open(os.path.join(ROOT, OUT, n + ".png")).convert("RGB").crop(
        (WALL[1].start, WALL[0].start, WALL[1].stop, WALL[0].stop)) for n in names]
    w, h = tiles[0].size
    out = Image.new("RGB", (len(tiles) * (w + 8) - 8, h), "white")
    for k, t in enumerate(tiles):
        out.paste(t, (k * (w + 8), 0))
    try:
        out.save(os.path.join(ROOT, OUT, "wall.png"))
    except OSError as e:   # an image viewer holding the last one open
        print("warning: wall.png not written: %s" % e)


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    failures = []
    ref = capture("reference", ["--frames", str(REFERENCE_FRAMES), "--quality", "reference"])
    a10 = capture("realtime_10", ["--frames", "10"])
    a60 = capture("realtime_60", ["--frames", "60"])
    b10, b60 = blotchiness(a10), blotchiness(a60)
    mean = float(a60.mean() / ref.mean())
    results = {"frame10_blotch": round(b10, 5), "frame60_blotch": round(b60, 5), "frame60_mean": round(mean, 4),
               "reference_blotch": round(blotchiness(ref), 5)}
    print("blotchiness: frame 10 %.2f%%, frame 60 %.2f%%, reference %.2f%%; frame 60's mean x%.3f" % (
        100 * b10, 100 * b60, 100 * results["reference_blotch"], mean))
    if b10 > BLOTCH_10:
        failures.append("frame 10's blotchiness %.2f%% over %.1f%%" % (100 * b10, 100 * BLOTCH_10))
    if b60 > BLOTCH_60:
        failures.append("frame 60's blotchiness %.2f%% over %.1f%%" % (100 * b60, 100 * BLOTCH_60))
    if abs(mean - 1.0) > MEAN_TOL:
        failures.append("frame 60's wall mean x%.3f of the reference (tolerance %d%%)" % (mean, MEAN_TOL * 100))
    sheet(["realtime_10", "realtime_60", "reference"])
    with open(os.path.join(ROOT, OUT, "gi_noise.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_gi_noise", "passed": 3 - len(failures), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
