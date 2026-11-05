"""Both renderers against closed-form radiance: the anchor the referee (check_parity.py) stands on.

A white Lambertian floor (albedo 0.5, roughness 1) under one light and nothing else, viewed
straight down: no other surface, so no bounce for GI or the path tracer to add. The radiance the
floor reflects toward the camera has a closed form:

  directional light, intensity I, at elevation e:          L = a / pi * I * sin(e)
  an emissive rectangle (a mesh) of radiance Le, parallel to the floor h above it:
                                                         L = a / pi * Le * pi * F, F the rectangle's form factor
                                                         (the corner formula, by inclusion and exclusion; frontier
                                                         F3: emissive triangles are lights)
  point light, intensity I, height h above the probed point: L = a / pi * I / h^2
  spot light pointing straight down, the point on its axis:  L = a / pi * I / h^2

Each case renders deferred (32 frames) and path traced (256 samples, no denoiser) through
`ohao run --hdr` and reads the linear radiance at the probed point (a 6x6-pixel mean). Both must be
within 5% of the closed form. What stays inside it: the deferred frame's Burley diffuse against
Lambert, the path tracer's (1 - F) on the diffuse lobe (a dielectric's 4% at normal incidence).

A point light's radius must not change what it delivers (its emitter is a sphere of that radius,
I / (pi r^2), sampled by the path tracer): the point case runs at 1 cm and 20 cm.

The bounce sees every light: a spot aimed at a wall lights the floor in front of it only by RT GI's
bounce, so in the deferred frame n spots of intensity I/n at one place must light the floor as one
spot of I does, and twenty spots of twenty strengths spread along the wall as the twenty do alone,
each within 1%. These run at reference quality (full resolution, the running mean, no denoiser):
the realtime denoiser's neighbourhood clamp is not linear, and twenty lights together came out 1.5%
over the sum of each alone. With 6 and 12 the trace weighs every light (kGIWeighedLights is 16); with 20
it draws candidates by power and resamples them. Until 2026-10-29 the bounce saw the 4 brightest:
12 spots bounced a third of the light.

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
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + (".exe" if os.name == "nt" else ""))
OUT = "build/analytic"
ALBEDO = 0.5
CAMERA_H = 3.0
FOV = 30.0
W, H = 320, 180
TOLERANCE = 0.05
BOUNCE_TOLERANCE = 0.01
BOUNCE_I = 60.0


def quat_x(deg):
    h = math.radians(deg) / 2
    return [math.sin(h), 0.0, 0.0, math.cos(h)]


def scene(light):
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Light": 1, "Material": 1, "MeshSource": 1, "Name": 1,
                       "Transform": 1},
        "entities": [
            {"id": "a0a0000000000001", "Name": {"value": "Camera"},
             "Transform": {"position": [0, CAMERA_H, 0], "rotation": quat_x(-90)}, "Camera": {"fovDeg": FOV}},
            {"id": "a0a0000000000002", "Name": {"value": "Floor"},
             "MeshSource": {"source": {"BoxPrim": {"lo": [-50, -0.1, -50], "hi": [50, 0, 50],
                                                    "color": [ALBEDO, ALBEDO, ALBEDO]}}},
             "Material": {"base_color": [ALBEDO, ALBEDO, ALBEDO], "specular_roughness": 1.0}},
            dict(light, id="a0a0000000000003"),
            # No environment: the closed form counts the light alone (a sun would otherwise bring the sky).
            {"id": "a0a0000000000004", "Name": {"value": "Environment"}, "Environment": {"sky": False}},
        ],
    }


def probe(img, x):
    # The floor point (x, 0, 0) seen from straight above: right is +X, the frame's half width at the
    # floor CAMERA_H tan(FOV / 2) * aspect.
    h, w, _ = img.shape
    half = CAMERA_H * math.tan(math.radians(FOV / 2)) * w / h
    cx = w // 2 + int(round(x / half * (w / 2)))
    return float(img[h // 2 - 3:h // 2 + 3, cx - 3:cx + 3].reshape(-1, 3).mean())


def corner_factor(x, y):
    # The form factor from a floor point to a parallel rectangle above it with one corner straight
    # overhead, its sides x and y in units of the height (odd in each).
    a, b = math.sqrt(1 + x * x), math.sqrt(1 + y * y)
    return (x / a * math.atan(y / a) + y / b * math.atan(x / b)) / (2 * math.pi)


def rectangle_factor(px, x1, x2, z1, z2, h):
    # The form factor from the floor point (px, 0, 0) to the rectangle [x1, x2] x [z1, z2] at height h.
    f = lambda x, z: corner_factor((x - px) / h, z / h)
    return f(x2, z2) - f(x1, z2) - f(x2, z1) + f(x1, z1)


def emissive_quad(le, h, x1, x2, z1, z2):
    # A rectangle mesh of radiance le (emission_color white x emission_luminance), facing the floor.
    return {"Name": {"value": "Glow"},
            "MeshSource": {"source": {"QuadPrim": {"a": [x1, h, z1], "b": [x2, h, z1], "c": [x2, h, z2], "d": [x1, h, z2],
                                                    "normal": [0, -1, 0], "color": [0, 0, 0]}}},
            "Material": {"base_color": [0, 0, 0], "specular_weight": 0, "emission_luminance": le}}


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
    # An emissive rectangle 1 m up, beside the view (so it hides none of the floor seen), the floor
    # probed 0.6 m from its edge: lit by the rectangle's triangles as lights.
    x1, x2, z1, z2, h, le = 1.6, 2.6, -0.5, 0.5, 1.0, 20.0
    yield ("emissive_quad", 1.0, emissive_quad(le, h, x1, x2, z1, z2),
           lam * le * math.pi * rectangle_factor(1.0, x1, x2, z1, z2, h))


def spot(i, x, intensity):
    # A spot 1.5 m up, aimed north at the wall (a spot shines along -Z unrotated): its cone (40
    # degrees) meets the wall above 0.2 m and never the floor.
    return {"id": "a0a00000000001%02x" % i, "Name": {"value": "Spot%d" % i}, "Transform": {"position": [x, 1.5, 0]},
            "Light": {"type": "Spot", "color": [1, 1, 1], "intensity": intensity, "range": 1000, "radius": 0.05,
                      "innerConeDeg": 30, "outerConeDeg": 40}}


def bounce_floor(name, spots, failures):
    # A floor, a wall 2 m north (-Z) facing it, the spots; the camera looks straight down at the floor
    # between them, which only the bounce off the wall lights. The mean of the frame's middle 60x60
    # pixels, or None when the capture failed.
    s = scene(spots[0])
    s["entities"][0]["Transform"]["position"] = [0, CAMERA_H, -0.6]
    s["components"].pop("Environment")   # spots only: no sun, so no sky either way
    s["entities"][1:] = [s["entities"][1],
                         {"id": "a0a0000000000004", "Name": {"value": "Wall"},
                          "MeshSource": {"source": {"BoxPrim": {"lo": [-50, 0, -2.1], "hi": [50, 10, -2],
                                                                 "color": [0.8, 0.8, 0.8]}}},
                          "Material": {"base_color": [0.8, 0.8, 0.8], "specular_roughness": 1.0}}] + spots
    rel = "%s/%s" % (OUT, name)
    with open(os.path.join(ROOT, rel + ".scene.json"), "w", newline="\n") as f:
        json.dump(s, f, indent=1)
    cmd = [OHAO, "run", rel + ".scene.json", "--render", rel + ".png", "--hdr", rel + ".pfm", "--mode", "deferred",
           "--size", "%dx%d" % (W, H), "--frames", "32", "--quality", "reference"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        failures.append("%s: exit %d" % (name, r.returncode))
        return None
    img = read_pfm(os.path.join(ROOT, rel + ".pfm"))
    return float(img[H // 2 - 30:H // 2 + 30, W // 2 - 30:W // 2 + 30].mean())


def bounce_check(name, got, expected, tolerance, what, failures, results):
    ratio = got / expected
    results[name] = round(ratio, 4)
    ok = abs(ratio - 1.0) <= tolerance
    print("%-18s floor %.4f, x%.4f of %s %s" % (name, got, ratio, what, "ok" if ok else "FAIL"))
    if not ok:
        failures.append("%s: the floor x%.4f of %s (tolerance %g%%)" % (name, ratio, what, tolerance * 100))


def bounce_checks(failures, results):
    checks = 0
    one = bounce_floor("bounce_1", [spot(0, 0.0, BOUNCE_I)], failures)
    if one is not None and not (one > 1e-3):
        failures.append("bounce_1: the floor got no bounce light (%.5f)" % one)
        one = None
    # n spots at one place, BOUNCE_I / n each, against one of BOUNCE_I.
    for n in (6, 12, 20):
        checks += 1
        got = bounce_floor("bounce_%d" % n, [spot(i, 0.0, BOUNCE_I / n) for i in range(n)], failures)
        if one is not None and got is not None:
            bounce_check("bounce_%d" % n, got, one, BOUNCE_TOLERANCE, "one spot of their sum", failures, results)
    # Twenty spots of twenty strengths spread along the wall, against the sum of each alone.
    spread = [spot(i, -2.375 + 0.25 * i, 1.0 + i) for i in range(20)]
    alone = [bounce_floor("bounce_spread_%d" % i, [sp], failures) for i, sp in enumerate(spread)]
    checks += 1
    together = bounce_floor("bounce_spread", spread, failures)
    if together is not None and None not in alone:
        bounce_check("bounce_spread", together, sum(alone), BOUNCE_TOLERANCE, "the twenty alone", failures, results)
    return checks


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    failures, results, checks = [], {}, 0
    for name, x, light, expected in cases():
        rel = "%s/%s" % (OUT, name)
        with open(os.path.join(ROOT, rel + ".scene.json"), "w", newline="\n") as f:
            json.dump(scene(light), f, indent=1)
        results[name] = {"expected": round(expected, 5)}
        # The emissive rectangle reaches deferred's floor through RT GI's rays only: at reference quality
        # (realtime GI's denoiser keeps 85% of so small and bright a source: frontier F5's to fix).
        deferred = ["--frames", "32"] + (["--quality", "reference"] if name == "emissive_quad" else [])
        for mode, args in (("deferred", deferred), ("rt_offline", ["--spp", "256", "--denoise", "none"])):
            cmd = [OHAO, "run", rel + ".scene.json", "--render", "%s.%s.png" % (rel, mode), "--hdr",
                   "%s.%s.pfm" % (rel, mode), "--mode", mode, "--size", "%dx%d" % (W, H)] + args
            if "--no-caustics" in sys.argv and mode == "rt_offline":
                cmd.append("--no-caustics")   # without caustics' light pass (sub-project 24); the bars unchanged
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
    checks += bounce_checks(failures, results)
    with open(os.path.join(ROOT, OUT, "analytic.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_analytic", "passed": checks - len(failures), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
