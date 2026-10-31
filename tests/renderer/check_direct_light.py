"""RT direct light on a showcase: Sponza at night, 37 coloured lamps (assets/scenes/sponza_night.scene.json).

The deferred frame at realtime quality (ReSTIR DI at half resolution, its denoiser, RT GI) against the
same frame at reference quality (full resolution, no reuse, the running mean of 256 frames), in linear
radiance (`ohao run --hdr`), from the lower arcade looking down it: pools of coloured light, the arches'
lamp shadows, the dim gallery lit from far away. The lamps' own spheres (radiance over 50) are left out.

  frame 1       the whole frame's mean within MEAN_TOL of the reference's (spatial reuse alone)
  frame 30      the whole frame's mean within MEAN_TOL, every 4x4 region's within REGION_TOL, and the
                relative MSE, mean((a - r)^2 / (r^2 + 0.01)), under REL_MSE_MAX

What it guards: a bias that grows with the frames (temporal reuse's Z), outliers that reuse spreads
(spatial reuse's pairwise MIS, the boiling filter, the shade bound), a denoiser that darkens (SVGF's stop, symmetric). F4's
full resolution frame measured 0.883 at frame 1 and 0.964 at frame 30; the half-resolution frame 0.974
and 0.983 under 1/Z (docs/changes/2026-10-31-frontier-f4b.md), 0.982 and 0.988 with spatial reuse's
pairwise MIS (docs/changes/2026-10-31-di-pairwise.md).

Writes build/direct_light/: each capture's PNG and PFM, and sheet.png (realtime frame 1 | frame 30 |
reference | |frame 30 - reference| / reference).

  python tests/renderer/check_direct_light.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_parity import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe")
SCENE = "assets/scenes/sponza_night.scene.json"
OUT = "build/direct_light"
SIZE = "960x540"
REFERENCE_FRAMES = 256
MEAN_TOL = 0.03
REGION_TOL = 0.08
REL_MSE_MAX = 0.04
EMITTER = 50.0


def lum(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def capture(name, args):
    png, pfm = "%s/%s.png" % (OUT, name), "%s/%s.pfm" % (OUT, name)
    cmd = [OHAO, "run", SCENE, "--render", png, "--hdr", pfm, "--mode", "deferred", "--size", SIZE] + args
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (name, r.returncode, r.stderr[-2000:]))
    return lum(read_pfm(os.path.join(ROOT, pfm)))


def regions(a, r, mask):
    h, w = r.shape
    out = []
    for j in range(4):
        for i in range(4):
            sl = (slice(j * h // 4, (j + 1) * h // 4), slice(i * w // 4, (i + 1) * w // 4))
            ref = r[sl][mask[sl]].sum()
            out.append(float(a[sl][mask[sl]].sum() / ref) if ref > 1e-6 else 1.0)
    return out


def sheet(names, ref, a30):
    tiles = [Image.open(os.path.join(ROOT, OUT, n + ".png")).convert("RGB") for n in names]
    err = np.clip(np.abs(a30 - ref) / np.maximum(ref, 0.01), 0.0, 1.0)
    tiles.append(Image.fromarray((np.sqrt(err) * 255).astype(np.uint8)).convert("RGB"))
    w, h = tiles[0].size
    out = Image.new("RGB", (w * 2 + 8, h * 2 + 8), "white")
    for k, t in enumerate(tiles):
        out.paste(t, ((k % 2) * (w + 8), (k // 2) * (h + 8)))
    try:
        out.save(os.path.join(ROOT, OUT, "sheet.png"))
    except OSError as e:   # an image viewer holding the last one open
        print("warning: sheet.png not written: %s" % e)


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    failures, results = [], {}
    ref = capture("reference", ["--frames", str(REFERENCE_FRAMES), "--quality", "reference"])
    mask = ref < EMITTER
    total = ref[mask].sum()
    a1 = capture("realtime_1", ["--frames", "1"])
    a30 = capture("realtime_30", ["--frames", "30"])

    m1, m30 = float(a1[mask].sum() / total), float(a30[mask].sum() / total)
    reg = regions(a30, ref, mask)
    rel = float((((a30 - ref) ** 2) / (ref ** 2 + 0.01))[mask].mean())
    results = {"frame1_mean": round(m1, 4), "frame30_mean": round(m30, 4), "frame30_regions": [round(min(reg), 4),
               round(max(reg), 4)], "frame30_rel_mse": round(rel, 5)}
    print("frame 1: mean x%.3f   frame 30: mean x%.3f, regions x%.3f..x%.3f, relative MSE %.4f" % (
        m1, m30, min(reg), max(reg), rel))
    if abs(m1 - 1.0) > MEAN_TOL:
        failures.append("frame 1's mean x%.3f of the reference (tolerance %d%%)" % (m1, MEAN_TOL * 100))
    if abs(m30 - 1.0) > MEAN_TOL:
        failures.append("frame 30's mean x%.3f of the reference (tolerance %d%%)" % (m30, MEAN_TOL * 100))
    bad = [k for k, v in enumerate(reg) if abs(v - 1.0) > REGION_TOL]
    if bad:
        failures.append("frame 30's regions %s outside %d%% (x%.3f..x%.3f)" % (bad, REGION_TOL * 100, min(reg),
                                                                              max(reg)))
    if rel > REL_MSE_MAX:
        failures.append("frame 30's relative MSE %.4f over %.2f" % (rel, REL_MSE_MAX))
    sheet(["realtime_1", "realtime_30", "reference"], ref, a30)
    with open(os.path.join(ROOT, OUT, "direct_light.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_direct_light", "passed": 4 - len(failures), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
