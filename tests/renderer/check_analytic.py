"""Both renderers against closed-form radiance: the anchor the referee (check_parity.py) stands on.

A white Lambertian floor (albedo 0.5, roughness 1) under one light and nothing else, viewed
straight down: no other surface, so no bounce for GI or the path tracer to add. The radiance the
floor reflects toward the camera has a closed form:

  directional light, intensity I, at elevation e:          L = a / pi * I * sin(e)
  point light, intensity I, height h above the probed point: L = a / pi * I / h^2
  spot light pointing straight down, the point on its axis:  L = a / pi * I / h^2

Each case renders deferred (32 frames) and path traced (256 samples, no denoiser) through
`ohao run --hdr` and reads the linear radiance at the probed point (a 6x6-pixel mean). Both must be
within 5% of the closed form. What stays inside it: the deferred frame's Burley diffuse against
Lambert, the path tracer's (1 - F) on the diffuse lobe (a dielectric's 4% at normal incidence).

A point light's radius must not change what it delivers (its emitter is a sphere of that radius,
I / (pi r^2), sampled by the path tracer): the point case runs at 1 cm and 20 cm.

  python tests/renderer/check_analytic.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import math
import os
import subprocess
import sys


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_parity import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe")
OUT = "build/analytic"
ALBEDO = 0.5
CAMERA_H = 3.0
FOV = 30.0
W, H = 320, 180
TOLERANCE = 0.05


def quat_x(deg):
    h = math.radians(deg) / 2
    return [math.sin(h), 0.0, 0.0, math.cos(h)]


def scene(light):
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Light": 1, "MaterialOverride": 1, "MeshSource": 1, "Name": 1, "Transform": 1},
        "entities": [
            {"id": "a0a0000000000001", "Name": {"value": "Camera"},
             "Transform": {"position": [0, CAMERA_H, 0], "rotation": quat_x(-90)}, "Camera": {"fovDeg": FOV}},
            {"id": "a0a0000000000002", "Name": {"value": "Floor"},
             "MeshSource": {"source": {"BoxPrim": {"lo": [-50, -0.1, -50], "hi": [50, 0, 50],
                                                    "color": [ALBEDO, ALBEDO, ALBEDO]}}},
             "MaterialOverride": {"baseColor": [ALBEDO, ALBEDO, ALBEDO], "roughness": 1.0, "metallic": 0.0}},
            dict(light, id="a0a0000000000003"),
        ],
    }


def probe(img, x):
    # The floor point (x, 0, 0) seen from straight above: right is +X, the frame's half width at the
    # floor CAMERA_H tan(FOV / 2) * aspect.
    h, w, _ = img.shape
    half = CAMERA_H * math.tan(math.radians(FOV / 2)) * w / h
    cx = w // 2 + int(round(x / half * (w / 2)))
    return float(img[h // 2 - 3:h // 2 + 3, cx - 3:cx + 3].reshape(-1, 3).mean())


def cases():
    lam = ALBEDO / math.pi
    for elev in (90, 45):
        yield ("sun_%d" % elev, 0.0, {"Name": {"value": "Sun"}, "Transform": {"rotation": quat_x(-elev)},
                                      "Light": {"type": "Directional", "color": [1, 1, 1], "intensity": 2.0}},
               lam * 2.0 * math.sin(math.radians(elev)))
    for radius in (0.01, 0.2):
        yield ("point_r%g" % radius, 1.0, {"Name": {"value": "Lamp"}, "Transform": {"position": [1.0, 2.0, 0]},
                                           "Light": {"type": "Point", "color": [1, 1, 1], "intensity": 10.0,
                                                     "range": 1000, "radius": radius}},
               lam * 10.0 / 4.0)
    yield ("spot_down", 1.0, {"Name": {"value": "Spot"}, "Transform": {"position": [1.0, 2.0, 0], "rotation": quat_x(-90)},
                              "Light": {"type": "Spot", "color": [1, 1, 1], "intensity": 10.0, "range": 1000,
                                        "radius": 0.05, "innerConeDeg": 30, "outerConeDeg": 40}},
           lam * 10.0 / 4.0)


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    failures, results, checks = [], {}, 0
    for name, x, light, expected in cases():
        rel = "%s/%s" % (OUT, name)
        with open(os.path.join(ROOT, rel + ".scene.json"), "w", newline="\n") as f:
            json.dump(scene(light), f, indent=1)
        results[name] = {"expected": round(expected, 5)}
        for mode, args in (("deferred", ["--frames", "32"]), ("rt_offline", ["--spp", "256", "--denoise", "none"])):
            cmd = [OHAO, "run", rel + ".scene.json", "--render", "%s.%s.png" % (rel, mode), "--hdr",
                   "%s.%s.pfm" % (rel, mode), "--mode", mode, "--size", "%dx%d" % (W, H)] + args
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            checks += 1
            if r.returncode != 0:
                failures.append("%s %s: exit %d" % (name, mode, r.returncode))
                continue
            got = probe(read_pfm(os.path.join(ROOT, "%s.%s.pfm" % (rel, mode))), x)
            ratio = got / expected
            results[name][mode] = round(ratio, 4)
            ok = abs(ratio - 1.0) <= TOLERANCE
            print("%-12s %-10s expected %.4f got %.4f (x%.3f) %s" % (name, mode, expected, got, ratio,
                                                                     "ok" if ok else "FAIL"))
            if not ok:
                failures.append("%s %s: x%.3f of the closed form (tolerance %d%%)" % (name, mode, ratio,
                                                                                    TOLERANCE * 100))
    with open(os.path.join(ROOT, OUT, "analytic.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_analytic", "passed": checks - len(failures), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
