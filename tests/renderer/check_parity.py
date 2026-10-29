"""The referee: the deferred frame against the path tracer, in linear radiance, scene by scene.

The path tracer is the engine's ground truth for light transport; the deferred frame is what games
ship. This renders each reference scene both ways through `ohao run --hdr` (the frame's linear
radiance before exposure, post and tonemapping: deferred's lit image, the path tracer's accumulation)
at one size and view, and measures how far deferred is from the path tracer:

- `stops`: the mean |log2(deferred / path tracer)| over 8x8-pixel blocks (luminance, blocks where
  either is under 1e-3 or the path tracer is over 20, a light's emitter, left out). 0 is the same
  light; 1 is a factor of two on average.
- `ratio`: deferred's mean luminance over the path tracer's, over the same blocks.
- `grid`: that ratio on a 4x4 grid of the frame, so a gap has a place.

It writes `build/parity/parity.json` and, per scene, `<scene>.error.png`: blue where deferred is
darker than the path tracer, red where brighter, saturated at 2 stops. It is a report (exit 0)
until the gaps are small enough to gate; it fails only when a capture fails.

Deferred: realtime quality, 32 frames from a reset (RT GI's history converged), no post. Path
tracer: rt_offline, 256 samples a pixel, no denoiser (block means average the noise).

  python tests/renderer/check_parity.py [--only SCENE] [--spp N]   # last line the JSON summary
"""
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe")
OUT = "build/parity"
SIZE = "640x360"
BLOCK = 8
SCENES = ["cornell_box", "damaged_helmet", "khronos_metal_rough_spheres", "area_light", "khronos_sponza",
          "blockout_room"]
LUMA = np.array([0.2126, 0.7152, 0.0722])


def read_pfm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"PF":
            raise ValueError("%s is not an RGB PFM" % path)
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
    return data.reshape(h, w, 3)[::-1].astype(np.float64)


def capture(scene, mode, args):
    pfm = "%s/%s.%s.pfm" % (OUT, scene, mode)
    png = "%s/%s.%s.png" % (OUT, scene, mode)
    cmd = [OHAO, "run", "assets/scenes/%s.scene.json" % scene, "--render", png, "--hdr", pfm, "--mode", mode,
           "--size", SIZE] + args
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("%s %s: exit %d: %s" % (scene, mode, r.returncode, r.stdout.strip()[-300:]))
    return read_pfm(os.path.join(ROOT, pfm))


def blocks(lum):
    h, w = lum.shape
    h, w = h - h % BLOCK, w - w % BLOCK
    return lum[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(1, 3))


def compare(deferred, traced):
    d, p = blocks(deferred @ LUMA), blocks(traced @ LUMA)
    keep = (d > 1e-3) & (p > 1e-3) & (p < 20.0)
    log2 = np.log2(np.where(keep, d, 1.0) / np.where(keep, p, 1.0))
    gh, gw = d.shape
    grid = []
    for gy in range(4):
        row = []
        for gx in range(4):
            sl = (slice(gy * gh // 4, (gy + 1) * gh // 4), slice(gx * gw // 4, (gx + 1) * gw // 4))
            k = keep[sl]
            row.append(round(float(d[sl][k].mean() / p[sl][k].mean()), 3) if k.any() else None)
        grid.append(row)
    return {
        "stops": round(float(np.abs(log2[keep]).mean()), 3),
        "ratio": round(float(d[keep].mean() / p[keep].mean()), 3),
        "blocks": int(keep.sum()),
        "grid": grid,
    }, log2, keep


def error_image(log2, keep, path):
    t = np.clip(log2 / 2.0, -1.0, 1.0)
    img = np.zeros(log2.shape + (3,))
    img[..., 0] = np.where(t > 0, t, 0)
    img[..., 2] = np.where(t < 0, -t, 0)
    img[..., 1] = 0.0
    img[~keep] = 0.25
    Image.fromarray((img * 255).astype(np.uint8)).resize((log2.shape[1] * BLOCK, log2.shape[0] * BLOCK),
                                                         Image.NEAREST).save(path)


def main():
    argv = sys.argv[1:]
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    spp = argv[argv.index("--spp") + 1] if "--spp" in argv else "256"
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    report, failures = {}, []
    for scene in SCENES:
        if only and scene != only:
            continue
        try:
            deferred = capture(scene, "deferred", ["--frames", "32"])
            traced = capture(scene, "rt_offline", ["--spp", spp, "--denoise", "none"])
        except RuntimeError as e:
            failures.append(str(e))
            continue
        result, log2, keep = compare(deferred, traced)
        report[scene] = result
        error_image(log2, keep, os.path.join(ROOT, OUT, scene + ".error.png"))
        print("%-30s %5.2f stops  deferred/PT %5.3f  (%d blocks)" % (scene, result["stops"], result["ratio"],
                                                                      result["blocks"]))
        for row in result["grid"]:
            print("    " + " ".join("  -  " if v is None else "%5.2f" % v for v in row))
    with open(os.path.join(ROOT, OUT, "parity.json"), "w", newline="\n") as f:
        json.dump(report, f, indent=2)
    print(json.dumps({"suite": "check_parity", "passed": len(report), "failed": len(failures), "failures": failures},
                     separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
