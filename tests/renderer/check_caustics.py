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
  c1_mirror:     the pass gives L within 2%.

c3, glass. A slab of clear glass (6 x 6 m, IOR 1.5, roughness 0, no absorption, 0.1 m thick) 1 m over a matte
floor (albedo 0.5, no specular), the sun 30 degrees from the zenith. Every internally reflected beam still
reaches the floor, so under the slab's middle the floor is lit uniformly, by every order of reflection
inside summed: E cos 30 (1 - F) / (1 + F), F the unpolarised dielectric Fresnel at 30 degrees (the same
at the inner face). The camera sits between slab and floor and sees the floor directly. With --bounces 1
the camera side's path stops before the floor can see itself in the slab's underside (no closed form for
that), and every order inside the slab is still reached (glass events are not bounces), so the floor's
radiance is exactly 0.5 / pi x that irradiance; within 2%, and no splat clamped.

c6, determinism: two captures with the pass in one `ohao serve` process are the same bits.

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
    return read_pfm(hdr), r.stdout + chr(10) + r.stderr


def slab_scene():
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Light": 1, "Material": 1, "MeshSource": 1, "Name": 1,
                       "Transform": 1},
        "entities": [
            # High under the slab with a wide view: the floor it sees is a good share of what the slab lights.
            ent(1, Name={"value": "Camera"}, Transform={"position": [0, 0.9, 0], "rotation": quat_x(-90)},
                Camera={"fovDeg": 90}),
            ent(2, Name={"value": "Floor"},
                MeshSource={"source": {"QuadPrim": {"a": [-15, 0, -15], "b": [15, 0, -15], "c": [15, 0, 15], "d": [-15, 0, 15],
                                                     "normal": [0, 1, 0], "color": [0.5, 0.5, 0.5]}}},
                Material={"base_color": [0.5, 0.5, 0.5], "specular_weight": 0, "specular_roughness": 1.0}),
            ent(3, Name={"value": "Slab"},
                MeshSource={"source": {"BoxPrim": {"lo": [-3, 1.0, -3], "hi": [3, 1.1, 3], "color": [1, 1, 1]}}},
                Material={"base_color": [1, 1, 1], "specular_roughness": 0.0, "transmission_weight": 1}),
            ent(5, Name={"value": "Sun"}, Transform={"rotation": quat_x(-60)},
                Light={"type": "Directional", "color": [1, 1, 1], "intensity": SUN_E}),
            ent(6, Name={"value": "Environment"}, Environment={"sky": False}),
        ],
    }


def fresnel(cos_i, eta):
    # The exact unpolarised dielectric Fresnel (the module's fresnelDielectric).
    s2 = (1.0 - cos_i * cos_i) / (eta * eta)
    if s2 >= 1.0:
        return 1.0
    cos_t = math.sqrt(1.0 - s2)
    rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t)
    rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t)
    return 0.5 * (rs * rs + rp * rp)


def clamped_from(stderr):
    for line in stderr.splitlines():
        if "caustics:" in line and "splats clamped" in line:
            return int(line.split("casters,")[1].split("splats")[0])
    return None


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


def c1_mirror(failures):
    img, _ = render("c1_mirror", mirror_scene(), 1024, ["--caustics"])
    m = r_mean(img)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(45))
    off, _ = render("c1_mirror_off", mirror_scene(), 1024)
    m_off = r_mean(off)
    err = abs(m - want) / want
    print("c1_mirror: R %.4f, closed form %.4f (%.2f%%); without the pass %.4f" % (m, want, 100 * err, m_off))
    if not err <= 0.02:
        failures.append("c1_mirror: R %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (m, want, 100 * err))
    return {"r_mean": m, "closed_form": want, "error": err, "without_pass": m_off}


def c3_slab(failures):
    os.makedirs(OUT, exist_ok=True)
    img, log = render("c3_slab", slab_scene(), 1024, ["--caustics", "--bounces", "1"])
    h, w, _ = img.shape
    m = float(img[h // 4:3 * h // 4, w // 4:3 * w // 4].mean())   # the middle half: under the slab's middle
    clamped = clamped_from(log)
    if clamped != 0:
        failures.append("c3_slab: %s splats clamped (want 0)" % clamped)
    f = fresnel(math.cos(math.radians(30)), 1.5)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(30)) * (1 - f) / (1 + f)
    err = abs(m - want) / want
    print("c3_slab: floor %.4f, closed form %.4f (F %.4f, %.2f%%)" % (m, want, f, 100 * err))
    if not err <= 0.02:
        failures.append("c3_slab: floor %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (m, want, 100 * err))
    return {"floor": m, "closed_form": want, "error": err}


def c6_repeat(failures):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "c6.scene.json")
    with open(path, "w") as fh:
        json.dump(mirror_scene(), fh)
    out = os.path.relpath(os.path.join(OUT, "serve"), ROOT).replace(os.sep, "/")
    proc = subprocess.Popen([OHAO, "serve", "--out", out], cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)

    def call(rid, method, params):
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n")
        proc.stdin.flush()
        for line in proc.stdout:
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg
        return None

    replies = [call(1, "scene.load", {"path": os.path.relpath(path, ROOT).replace(os.sep, "/")})]
    for i in (0, 1):
        replies.append(call(2 + i, "render.capture", {"path": out + "/c6_%d.png" % i, "hdr": out + "/c6_%d.pfm" % i,
                                                       "mode": "rt_offline", "spp": 64, "width": W, "height": H,
                                                       "caustics": True}))
    proc.stdin.close()
    proc.wait(timeout=120)
    hdr = [os.path.join(ROOT, out, "c6_%d.pfm" % i) for i in (0, 1)]
    errors = [r.get("error") for r in replies if r is None or "error" in r]
    same = not errors and all(os.path.exists(h) for h in hdr) and open(hdr[0], "rb").read() == open(hdr[1], "rb").read()
    print("c6_repeat: two captures in one process %s" % ("identical" if same else "DIFFER"))
    if not same:
        failures.append("c6_repeat: two captures with caustics in one process differ (%s)" % errors)
    return {"identical": same}


CASES = {"c1_split_only": c1_split_only, "c1_mirror": c1_mirror, "c3_slab": c3_slab, "c6_repeat": c6_repeat}


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
