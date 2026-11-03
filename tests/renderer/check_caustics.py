"""Caustics (sub-project 24, docs/specs/2026-11-03-caustics-design.md): light traced from the sun through
smooth surfaces onto matte ones, the camera side dropping exactly what the light pass owns.

c1, a mirror's closed form. A matte patch R (Lambertian, albedo 0.5, no specular) is the only matte
surface. A black wall (albedo 0, no specular) keeps the sun off R. A vertical mirror (metal, base colour
1, roughness 0) reflects the sun onto R. The sun travels down at 45 degrees toward -Z with irradiance E,
the mirror faces +Z at z = -1, and R spans z in [0, 1]. The reflected beam reaches R at 45 degrees, so
R's radiance is

    L = 0.5 / pi * E * F * cos 45,   F = 1 (a white metal's F82 Fresnel is 1 at every angle).

Every path that lights R is one the light pass owns (sun -> mirror -> R -> camera), so:
  c1_split_only: the camera side alone (--caustics with OHAO_CAUSTICS_SPLIT_ONLY=1, the pass not run)
                 leaves R at exactly 0;
  c1_mirror:     the pass gives L within 2% (task 5).

  python tests/renderer/check_caustics.py [CASE...]   # exit 0 pass, 1 fail; last line the JSON summary
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
OUT = os.path.join(ROOT, "build", "caustics")
W, H = 320, 180
SUN_E = 3.0


def quat_x(deg):
    h = math.radians(deg) / 2
    return [math.sin(h), 0.0, 0.0, math.cos(h)]


def ent(i, **c):
    return dict({"id": "c0c00000000000%02x" % i}, **c)


def black(lo, hi):
    return {"MeshSource": {"source": {"BoxPrim": {"lo": lo, "hi": hi, "color": [0, 0, 0]}}},
            "Material": {"base_color": [0, 0, 0], "specular_weight": 0, "specular_roughness": 1.0}}


def mirror_scene():
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Light": 1, "Material": 1, "MeshSource": 1, "Name": 1,
                       "Transform": 1},
        "entities": [
            # Straight down onto R's centre (0, 0, 0.5).
            ent(1, Name={"value": "Camera"}, Transform={"position": [0, 4.0, 0.5], "rotation": quat_x(-90)},
                Camera={"fovDeg": 30}),
            ent(2, Name={"value": "R"},
                MeshSource={"source": {"QuadPrim": {"a": [-1, 0, 0], "b": [1, 0, 0], "c": [1, 0, 1], "d": [-1, 0, 1],
                                                     "normal": [0, 1, 0], "color": [0.5, 0.5, 0.5]}}},
                Material={"base_color": [0.5, 0.5, 0.5], "specular_weight": 0, "specular_roughness": 1.0}),
            # The mirror: facing +Z at z = -1, from 0.8 m up (so R never sees its own reflection).
            ent(3, Name={"value": "Mirror"},
                MeshSource={"source": {"QuadPrim": {"a": [-3, 0.8, -1], "b": [3, 0.8, -1], "c": [3, 4, -1], "d": [-3, 4, -1],
                                                     "normal": [0, 0, 1], "color": [1, 1, 1]}}},
                Material={"base_color": [1, 1, 1], "base_metalness": 1, "specular_roughness": 0.0}),
            # The wall: a direct sun ray reaching R at z crosses z = 1.2 at height 1.2 - z, under 3 m.
            ent(4, Name={"value": "Wall"}, **black([-4, 0, 1.2], [4, 3, 1.3])),
            ent(5, Name={"value": "Sun"}, Transform={"rotation": quat_x(-45)},
                Light={"type": "Directional", "color": [1, 1, 1], "intensity": SUN_E}),
            ent(6, Name={"value": "Environment"}, Environment={"sky": False}),
        ],
    }


def render(name, scene, spp, extra=(), env=None):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".scene.json")
    with open(path, "w") as f:
        json.dump(scene, f)
    hdr = os.path.join(OUT, name + ".pfm")
    cmd = [OHAO, "run", path, "--render", os.path.join(OUT, name + ".png"), "--hdr", hdr, "--mode", "rt_offline",
           "--spp", str(spp), "--denoise", "none", "--size", "%dx%d" % (W, H)] + list(extra)
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run(cmd, capture_output=True, text=True, env=e, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError("%s: ohao run exit %d\n%s" % (name, r.returncode, r.stderr[-2000:]))
    return read_pfm(hdr), r.stdout


def r_mean(img):
    # R's middle: the camera looks straight down at its centre; 40 x 40 pixels well inside it.
    h, w, _ = img.shape
    return float(img[h // 2 - 20:h // 2 + 20, w // 2 - 20:w // 2 + 20].mean())


def c1_split_only(failures):
    img, _ = render("c1_split_only", mirror_scene(), 64, ["--caustics"], {"OHAO_CAUSTICS_SPLIT_ONLY": "1"})
    m = r_mean(img)
    ok = m < 1e-6
    print("c1_split_only: R mean %.3g (want 0)" % m)
    if not ok:
        failures.append("c1_split_only: R mean %.3g, want 0: the camera side kept a term the light pass owns" % m)
    return {"r_mean": m}


CASES = {"c1_split_only": c1_split_only}


def main(argv):
    names = argv[1:] or list(CASES)
    failures, results = [], {}
    for n in names:
        results[n] = CASES[n](failures)
    print(json.dumps({"suite": "check_caustics", "passed": len(names) - len(failures), "failed": len(failures),
                      "failures": failures, "results": results}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
