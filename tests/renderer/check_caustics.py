"""Caustics (sub-project 24, docs/specs/2026-11-03-caustics-design.md): light traced from the sun through
smooth surfaces onto matte ones, the camera side dropping exactly what the light pass owns.

c1, a mirror's closed form. A matte patch R (Lambertian, albedo 0.5, no specular) is the only matte
surface. A black wall (albedo 0, no specular) keeps the sun off R. A vertical mirror (metal, base colour
1, roughness 0) reflects the sun onto R. The sun travels down at 45 degrees toward -Z with irradiance E,
the mirror faces +Z at z = -1, and R spans z in [0, 1]. The reflected beam reaches R at 45 degrees, so
R's radiance is

    L = 0.5 / pi * E * F * cos 45,   F = 1 (a white metal's F82 Fresnel is 1 at every angle).

Every path that lights R is one the light pass owns (sun -> mirror -> R -> camera), so:
  c1_split_only: the camera side alone (OHAO_CAUSTICS_SPLIT_ONLY=1, the pass not run)
                 leaves R at exactly 0;
  c1_mirror:     the pass gives L within 2%.

c3, glass (c3_slab_guided: the same with path guiding, on caustics' guided program). A slab of clear glass (6 x 6 m, IOR 1.5, roughness 0, no absorption, 0.1 m thick) 1 m over a matte
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
import re
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pfm import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + (".exe" if os.name == "nt" else ""))
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
    img, _ = render("c1_split_only", mirror_scene(), 64, [], {"OHAO_CAUSTICS_SPLIT_ONLY": "1"})
    m = r_mean(img)
    ok = m < 1e-6
    print("c1_split_only: R mean %.3g (want 0)" % m)
    if not ok:
        failures.append("c1_split_only: R mean %.3g, want 0: the camera side kept a term the light pass owns" % m)
    return {"r_mean": m}


def c1_mirror(failures):
    img, _ = render("c1_mirror", mirror_scene(), 1024)
    m = r_mean(img)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(45))
    off, _ = render("c1_mirror_off", mirror_scene(), 1024, ["--no-caustics"])
    m_off = r_mean(off)
    err = abs(m - want) / want
    print("c1_mirror: R %.4f, closed form %.4f (%.2f%%); without the pass %.4f" % (m, want, 100 * err, m_off))
    if not err <= 0.02:
        failures.append("c1_mirror: R %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (m, want, 100 * err))
    return {"r_mean": m, "closed_form": want, "error": err, "without_pass": m_off}


def c3_slab(failures, name="c3_slab", extra=(), logs=None):
    os.makedirs(OUT, exist_ok=True)
    img, log = render(name, slab_scene(), 1024, ["--bounces", "1"] + list(extra))
    if logs is not None:
        logs.append(log)
    h, w, _ = img.shape
    m = float(img[h // 4:3 * h // 4, w // 4:3 * w // 4].mean())   # the middle half: under the slab's middle
    clamped = clamped_from(log)
    if clamped != 0:
        failures.append("%s: %s splats clamped (want 0)" % (name, clamped))
    f = fresnel(math.cos(math.radians(30)), 1.5)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(30)) * (1 - f) / (1 + f)
    err = abs(m - want) / want
    print("%s: floor %.4f, closed form %.4f (F %.4f, %.2f%%)" % (name, m, want, f, 100 * err))
    if not err <= 0.02:
        failures.append("%s: floor %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (name, m, want, 100 * err))
    return {"floor": m, "closed_form": want, "error": err}


# c3_slab with path guiding (sub-project 23), on caustics' guided program (raygenCausticsGuided, beside causticTrace):
# guiding is unbiased, so the closed form holds; the log shows that program built.
def c3_slab_guided(failures):
    logs = []
    r = c3_slab(failures, "c3_slab_guided", ["--guiding"], logs)
    if not any("path_tracer_caustics_guided pipeline" in log for log in logs):
        failures.append("c3_slab_guided: raygenCausticsGuided was never built")
    return r



# ---- c3_sphere: a glass ball's caustic against an independent ray simulation ----------------------
# A clear ball (radius 0.5, IOR 1.5, roughness 0) moved to (-0.7, 0.5, 0) (a transformed caster: the
# light pass once aimed at a caster's untransformed mesh and lit half of this caustic), the sun 35
# degrees up travelling +Z, seen from straight above. The reference traces parallel sun rays through
# the sphere with numpy (Snell, unpolarised Fresnel at each surface, four internal reflections) onto
# the floor and bins them into the camera's pixels. In the shadow (eroded 3 pixels, so the edge is
# no question) the caustic's light over the lit floor's must agree within 5% in total, and the two
# halves of the render (the scene is mirror-symmetric) within 3% of each other.

SPHERE_C = (-0.7, 0.5, 0.0)
SPHERE_SUN_Q = [0, 0.953717, 0.3007058, 0]   # yaw 180, pitch -35: the sun travels +Z, 35 degrees up
SPHERE_CAM = (-0.7, 4.0, 0.6)
SPHERE_FOV = 40.0


def sphere_scene():
    h = math.radians(-90) / 2
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Light": 1, "Material": 1, "MeshSource": 1, "Name": 1,
                       "Transform": 1},
        "entities": [
            ent(1, Name={"value": "Camera"}, Transform={"position": list(SPHERE_CAM), "rotation": [math.sin(h), 0, 0, math.cos(h)]},
                Camera={"fovDeg": SPHERE_FOV}),
            ent(2, Name={"value": "Floor"},
                MeshSource={"source": {"BoxPrim": {"lo": [-8, -0.1, -8], "hi": [8, 0, 8], "color": [0.6, 0.6, 0.6]}}},
                Material={"base_color": [0.6, 0.6, 0.6], "specular_weight": 0, "specular_roughness": 1.0}),
            ent(3, Name={"value": "Glass"}, Transform={"position": list(SPHERE_C)},
                MeshSource={"source": {"SpherePrim": {"radius": 0.5, "sectors": 96, "stacks": 48}}},
                Material={"base_color": [1, 1, 1], "specular_roughness": 0.0, "transmission_weight": 1}),
            ent(5, Name={"value": "Sun"}, Transform={"rotation": SPHERE_SUN_Q},
                Light={"type": "Directional", "color": [1, 1, 1], "intensity": SUN_E}),
            ent(6, Name={"value": "Environment"}, Environment={"sky": False}),
        ],
    }


def sphere_reference(w, h, orders=5, n_glass=1.5):
    # orders: the paths through the ball kept, by internal reflections (1: in and out only, step 2b's K = 2). n_glass:
    # the IOR the ball refracts at (OpenPBR's, after specular_weight: modulated_ior).
    import numpy as np
    c = np.array(SPHERE_C)
    rad = 0.5
    x, y, z, qw = SPHERE_SUN_Q
    u = np.array([x, y, z])
    v = np.array([0.0, 0.0, -1.0])
    sun = 2 * np.dot(u, v) * u + (qw * qw - np.dot(u, u)) * v + 2 * qw * np.cross(u, v)
    sun /= np.linalg.norm(sun)

    def fresnel(ci, eta):
        s2 = (1 - ci ** 2) / eta ** 2
        ct = np.sqrt(np.clip(1 - s2, 0, 1))
        rs = (ci - eta * ct) / (ci + eta * ct)
        rp = (eta * ci - ct) / (eta * ci + ct)
        return np.where(s2 >= 1, 1.0, 0.5 * (rs * rs + rp * rp)), ct

    def refract(d, nrm, eta, ci, ct):
        return d / eta + nrm * (ci / eta - ct)[:, None]

    def far(p, d):
        oc = p - c
        b = np.sum(oc * d, 1)
        return p + d * (-b + np.sqrt(np.maximum(b * b - (np.sum(oc * oc, 1) - rad * rad), 0)))[:, None]

    rng = np.random.default_rng(1)
    n = 4_000_000
    t1 = np.cross(sun, [0, 1, 0]); t1 /= np.linalg.norm(t1)
    t2 = np.cross(sun, t1)
    r = rad * np.sqrt(rng.random(n)); phi = 2 * math.pi * rng.random(n)
    o = c + t1[None] * (r * np.cos(phi))[:, None] + t2[None] * (r * np.sin(phi))[:, None] - sun * 3
    d = np.tile(sun, (n, 1))
    oc = o - c; b = np.sum(oc * d, 1)
    p = o + d * (-b - np.sqrt(np.maximum(b * b - (np.sum(oc * oc, 1) - rad * rad), 0)))[:, None]
    nrm = (p - c) / rad
    ci = -np.sum(d * nrm, 1)
    f, ct = fresnel(ci, n_glass)
    wgt = 1 - f
    d = refract(d, nrm, n_glass, ci, ct)
    tan_y = math.tan(math.radians(SPHERE_FOV / 2)); tan_x = tan_y * w / h
    hgt = SPHERE_CAM[1]
    e = np.zeros((h, w))
    for _ in range(orders):
        q = far(p, d)
        nin = -(q - c) / rad
        ci = -np.sum(d * nin, 1)
        f, ct = fresnel(ci, 1 / n_glass)
        out = refract(d, nin, 1 / n_glass, ci, ct)
        down = out[:, 1] < 0
        fp = q[down] + out[down] * (-q[down, 1] / out[down, 1])[:, None]
        px = (((fp[:, 0] - SPHERE_CAM[0]) / (hgt * tan_x)) * 0.5 + 0.5) * w
        py = (((fp[:, 2] - SPHERE_CAM[2]) / (hgt * tan_y)) * 0.5 + 0.5) * h
        ok = (px >= 0) & (px < w) & (py >= 0) & (py < h)
        np.add.at(e, (py[ok].astype(int), px[ok].astype(int)), (wgt * (1 - f))[down][ok])
        d = d - 2 * np.sum(d * (-nin), 1)[:, None] * (-nin)
        p = q
        wgt = wgt * f
    e *= (math.pi * rad * rad / n) / ((2 * hgt * tan_x / w) * (2 * hgt * tan_y / h))
    ref = e / -sun[1]   # over the lit floor's irradiance, E cos
    # The shadow, by geometry, and the camera's view of the floor not hidden by the ball.
    yy, xx = np.mgrid[0:h, 0:w]
    fx = SPHERE_CAM[0] + hgt * ((xx + 0.5) / w * 2 - 1) * tan_x
    fz = SPHERE_CAM[2] + hgt * ((yy + 0.5) / h * 2 - 1) * tan_y
    pts = np.stack([fx, np.zeros_like(fx), fz], -1)
    oc = pts - c; b = np.sum(oc * (-sun), -1); cc = np.sum(oc * oc, -1) - rad * rad
    shadow = ((b * b - cc) > 0) & ((-b - np.sqrt(np.maximum(b * b - cc, 0))) > 0)
    for _ in range(3):   # erode
        shadow = shadow & np.roll(shadow, 1, 0) & np.roll(shadow, -1, 0) & np.roll(shadow, 1, 1) & np.roll(shadow, -1, 1)
    # Hidden by the ball from the camera: the ball's disc seen from above, with a margin.
    toc = np.stack([fx - SPHERE_CAM[0], -hgt * np.ones_like(fx), fz - SPHERE_CAM[2]], -1)
    toc /= np.linalg.norm(toc, axis=-1, keepdims=True)
    ocam = np.array(SPHERE_CAM) - c
    bb = np.sum(ocam * toc, -1)
    hidden = (bb * bb - (np.dot(ocam, ocam) - (rad * 1.05) ** 2)) > 0
    return ref, shadow & ~hidden


def c3_sphere(failures):
    import numpy as np
    w, h = 320, 180
    # Direct light only, as the simulation (and c3_slab): with every bounce the ball's outside also reflects the floor
    # under it into its shadow, the caustic included once step 2b lights a point seen in a reflection (+1.8%).
    img, log = render("c3_sphere", sphere_scene(), 2048, ["--bounces", "1"])
    lum = img.mean(2)
    lit = float(np.median(lum[5:30, 5:60]))
    got = lum / lit
    ref, mask = sphere_reference(w, h)
    e_ref, e_got = float(ref[mask].sum()), float(got[mask].sum())
    err = abs(e_got - e_ref) / e_ref
    # Mirror symmetry about the ball's column (the camera is above it).
    cx = w // 2
    left = float(got[:, :cx][mask[:, :cx]].sum())
    right = float(got[:, cx:][mask[:, cx:]].sum())
    asym = abs(left - right) / max(left + right, 1e-9) * 2
    print("c3_sphere: caustic in the shadow %.1f, reference %.1f (%.2f%%); halves %.1f / %.1f (%.2f%%)"
          % (e_got, e_ref, 100 * err, left, right, 100 * asym))
    if not err <= 0.05:
        failures.append("c3_sphere: the caustic's energy %.1f against the ray-simulated %.1f (%.2f%%, bar 5%%)"
                        % (e_got, e_ref, 100 * err))
    if not asym <= 0.03:
        failures.append("c3_sphere: the halves differ by %.2f%% (bar 3%%): a mirror-symmetric scene" % (100 * asym))
    return {"energy": e_got, "reference": e_ref, "error": err, "asymmetry": asym}



# ---- c2_split: every path counted once ----------------------------------------------------------
# The c1 room with reflectors the camera side can converge on alone: a metal at roughness 0.15 and a
# rough base (0.35) under a coat at 0.15 (both smooth parts at the cut 0.25). The camera side alone at
# 2^16 spp against the split with the light pass at 2^12, at 160 x 90: R's mean within 1%.

def c2_scene(material):
    s = mirror_scene()
    for e in s["entities"]:
        if e["Name"]["value"] == "Mirror":
            e["Material"] = material
    return s


def c2_split(failures):
    global W, H
    w0, h0 = W, H
    W, H = 160, 90
    out = {}
    try:
        for tag, mat in (("metal", {"base_color": [0.9, 0.9, 0.9], "base_metalness": 1, "specular_roughness": 0.15}),
                         ("coated", {"base_color": [0.7, 0.7, 0.7], "specular_roughness": 0.35, "coat_weight": 1,
                                     "coat_roughness": 0.15})):
            ref, _ = render("c2_%s_camera" % tag, c2_scene(mat), 65536, ["--no-caustics"])
            got, _ = render("c2_%s_split" % tag, c2_scene(mat), 4096)
            a, b = r_mean(ref), r_mean(got)
            err = abs(b - a) / max(a, 1e-9)
            print("c2_split %s: camera side alone %.4f, split %.4f (%.2f%%)" % (tag, a, b, 100 * err))
            if not err <= 0.01:
                failures.append("c2_split %s: the split %.4f against the camera side alone %.4f (%.2f%%, bar 1%%)"
                                % (tag, b, a, 100 * err))
            out[tag] = {"camera": a, "split": b, "error": err}
    finally:
        W, H = w0, h0
    return out


# ---- c4_through_glass: R lit by the mirror, seen only through a clear slab -----------------------
# The c1 room with a clear slab (roughness 0) between camera and R. R's first hit is the glass, so the
# light pass owns nothing there; the mirror's light reaches R at camera depth 2 (the slab's two faces
# first), a term step 2a's manifold connection owns. The closed form: c1's R times the slab's
# transmittance head-on, (1 - F)^2 / (1 - F^2) with its reflections inside (those past depth 2 stay with
# the camera side, which cannot find a mirror's delta sun: (1 - F)^2 is what step 2a alone can give,
# 0.16% under). Without caustics R misses the mirror's light.

def c4_scene():
    s = mirror_scene()
    s["entities"].append(ent(9, Name={"value": "Window"},
                             # Just under the camera, inside its view cone and clear of the sun's way to the
                             # mirror (a sun ray to the mirror crosses y = 3.8 at z >= 0.8).
                             MeshSource={"source": {"BoxPrim": {"lo": [-0.3, 3.8, 0.3], "hi": [0.3, 3.85, 0.7],
                                                                "color": [1, 1, 1]}}},
                             Material={"base_color": [1, 1, 1], "specular_roughness": 0.0, "transmission_weight": 1}))
    return s


def c4_through_glass(failures):
    off, _ = render("c4_off", c4_scene(), 1024, ["--no-caustics"])
    on, _ = render("c4_on", c4_scene(), 1024)
    a, b = r_mean(off), r_mean(on)
    f = fresnel(1.0, 1.5)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(45)) * (1 - f) ** 2 / (1 - f * f)
    err = abs(b - want) / want
    print("c4_through_glass: R %.4f, closed form %.4f (%.2f%%); without caustics %.4f" % (b, want, 100 * err, a))
    if not err <= 0.01:
        failures.append("c4_through_glass: R %.4f against the closed form %.4f (%.2f%%, bar 1%%)" % (b, want, 100 * err))
    return {"r_mean": b, "closed_form": want, "error": err, "without_caustics": a}



# ---- c5_showcase (--showcase; CTest check_caustics_showcase, label slow) -------------------------
# Sponza by day with the helmet on its plinth and the glass, amber, gold, car-paint and bubble balls
# (assets/scenes/caustics_showcase.scene.json) at 480 x 270: a reference with the pass at 8192 spp, then
# the pass at 512 spp against no pass at 704 (the same time: the pass costs ~1.37x a frame). Dots are pixels
# over 4x the reference (+0.01): sharp features the reference has are not dots. With the pass the whole
# image's relMSE is at most a tenth of the one without (ruling C4's measure: 190x lower), no more dots,
# and the clear ball's dot (295, 261), 1,283x its neighbours without the pass, under 2x.

def c5_showcase(failures):
    import numpy as np
    global W, H
    w0, h0 = W, H
    W, H = 480, 270
    try:
        scene = json.load(open(os.path.join(ROOT, "assets", "scenes", "caustics_showcase.scene.json")))
        ref, _ = render("c5_reference", scene, 8192)
        on, _ = render("c5_with", scene, 512)
        off, _ = render("c5_without", scene, 704, ["--no-caustics"])
    finally:
        W, H = w0, h0
    r = ref.mean(2)
    out = {}
    for tag, img in (("with", on), ("without", off)):
        x = img.mean(2)
        rel = float(((x - r) ** 2 / (r ** 2 + 1e-3)).mean())
        dots = int((x > 4 * r + 0.01).sum())
        m5 = float(np.median(x[259:264, 293:298]))   # the 5 x 5 median at the clear ball's pixel
        glass = float(x[261, 295] / max(m5, 1e-6))
        out[tag] = {"relMSE": rel, "dots": dots, "glass_dot": glass}
        print("c5_showcase %s: relMSE %.5f, dots %d, the glass ball's pixel %.1fx its neighbours" % (tag, rel, dots, glass))
    if not out["with"]["relMSE"] <= 0.1 * out["without"]["relMSE"]:
        failures.append("c5_showcase: relMSE %.5f with the pass, %.5f without (bar: a tenth)" % (out["with"]["relMSE"], out["without"]["relMSE"]))
    if not out["with"]["dots"] <= out["without"]["dots"]:
        failures.append("c5_showcase: %d dots with the pass, %d without" % (out["with"]["dots"], out["without"]["dots"]))
    if not out["with"]["glass_dot"] < 2.0:
        failures.append("c5_showcase: the glass ball's dot is %.1fx its neighbours with the pass (bar 2)" % out["with"]["glass_dot"])
    return out



# ---- c1_emitter: a light's mesh in the sun's way is not a shadow -------------------------------------
# The c1 room with a point light's sphere (radius 0.3, nearly dark) where the sun's rays to the mirror pass:
# the camera side's sun shadow ray does not see emitter meshes, so neither may the light pass's first segment,
# or the pass drops a shadow-shaped piece of a term the camera side subtracted. R's closed form is unchanged.

def c1_emitter(failures):
    s = mirror_scene()
    # A sun ray reaching the mirror at (0, 1.5, -1) (which lights R's middle) comes from (0, 1.5 + t, -1 + t).
    s["entities"].append(ent(10, Name={"value": "Lamp"}, Transform={"position": [0, 2.5, 0.0]},
                             Light={"type": "Point", "color": [1, 1, 1], "intensity": 1e-4, "range": 10, "radius": 0.3}))
    img, _ = render("c1_emitter", s, 1024)
    m = r_mean(img)
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(45))
    err = abs(m - want) / want
    print("c1_emitter: R %.4f, closed form %.4f (%.2f%%)" % (m, want, 100 * err))
    if not err <= 0.02:
        failures.append("c1_emitter: R %.4f against %.4f (%.2f%%): a light's mesh shadowed the light pass" % (m, want, 100 * err))
    return {"r_mean": m, "closed_form": want, "error": err}


def c6_repeat(failures):
    out = serve_twice("c6", mirror_scene(), {})
    print("c6_repeat: two captures in one process %s" % ("identical" if out["identical"] else "DIFFER"))
    if not out["identical"]:
        failures.append("c6_repeat: two captures with caustics in one process differ (%s)" % out.get("errors"))
    return {"identical": out["identical"]}


def serve_twice(name, scene, extra):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".scene.json")
    with open(path, "w") as fh:
        json.dump(scene, fh)
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
        params = {"path": out + "/%s_%d.png" % (name, i), "hdr": out + "/%s_%d.pfm" % (name, i), "mode": "rt_offline",
                  "spp": 64, "width": W, "height": H, "caustics": True}
        params.update(extra)
        replies.append(call(2 + i, "render.capture", params))
    proc.stdin.close()
    proc.wait(timeout=1800)
    hdr = [os.path.join(ROOT, out, "%s_%d.pfm" % (name, i)) for i in (0, 1)]
    errors = [r.get("error") for r in replies if r is None or "error" in r]
    same = not errors and all(os.path.exists(h) for h in hdr) and open(hdr[0], "rb").read() == open(hdr[1], "rb").read()
    return {"identical": same, "errors": errors}


# ---- caustics step 2a (docs/specs/2026-11-04-caustics-step2-design.md): the sun's smooth reflection seen past the
# first hit. m1: c1's room (the mirror M1 lights R, the black wall keeps the sun off it) and a second, vertical white
# mirror M2 at x = -1.5 facing +x, which the sun's light runs parallel to (it reflects none of it). The camera, above
# and to the side, sees R directly (depth 0: step 1's term, L) and in M2 (R at depth 1, the sun through M1 at depth 2:
# step 2a's term, F2 L = L for a white metal). With --bounces 2 nothing else reaches either view, so both are
# L = 0.5 / pi E cos 45 within 1%. Without step 2a the mirrored R is missing (the camera side cannot find a mirror's
# reflection of a delta sun).

M1_CAM = (2.5, 3.0, 0.5)
M1_LOOK = (-1.5, 0.3, 0.5)
M1_FOV = 60.0


def look_quat(eye, target):
    import numpy as np
    f = np.array(target, float) - np.array(eye, float)
    f /= np.linalg.norm(f)
    r = np.cross(f, [0.0, 1.0, 0.0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    m = np.stack([r, u, -f], axis=1)   # columns: the camera's x, y, z (it looks down -z)
    t = np.trace(m)
    if t > 0:
        s = 0.5 / math.sqrt(t + 1.0)
        return [float((m[2, 1] - m[1, 2]) * s), float((m[0, 2] - m[2, 0]) * s), float((m[1, 0] - m[0, 1]) * s),
                float(0.25 / s)]
    i = int(np.argmax(np.diag(m)))
    j, k = (i + 1) % 3, (i + 2) % 3
    s = 2.0 * math.sqrt(1.0 + m[i, i] - m[j, j] - m[k, k])
    q = [0.0, 0.0, 0.0, 0.0]
    q[i] = 0.25 * s
    q[j] = (m[j, i] + m[i, j]) / s
    q[k] = (m[k, i] + m[i, k]) / s
    q[3] = (m[k, j] - m[j, k]) / s
    return [float(v) for v in q]


def mirror_seen_scene(m1_rough=0.0):
    s = mirror_scene()
    s["entities"] = [e for e in s["entities"] if e.get("Name", {}).get("value") != "Camera"]
    for e in s["entities"]:
        if e.get("Name", {}).get("value") == "Mirror":
            e["Material"]["specular_roughness"] = m1_rough
    s["entities"].append(ent(1, Name={"value": "Camera"}, Transform={"position": list(M1_CAM), "rotation": look_quat(M1_CAM, M1_LOOK)},
                             Camera={"fovDeg": M1_FOV}))
    s["entities"].append(ent(11, Name={"value": "Mirror2"},
                             MeshSource={"source": {"QuadPrim": {"a": [-1.5, 0, -0.5], "b": [-1.5, 0, 1.5], "c": [-1.5, 2, 1.5],
                                                                  "d": [-1.5, 2, -0.5], "normal": [1, 0, 0], "color": [1, 1, 1]}}},
                             Material={"base_color": [1, 1, 1], "base_metalness": 1, "specular_roughness": 0.0}))
    return s


def m1_masks(margin=0.06):
    """Pixels whose whole footprint (its corners) meets R directly, and those meeting R through M2, well inside both."""
    import numpy as np
    eye = np.array(M1_CAM, float)
    f = np.array(M1_LOOK, float) - eye
    f /= np.linalg.norm(f)
    r = np.cross(f, [0.0, 1.0, 0.0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    tan_y = math.tan(math.radians(M1_FOV) / 2)
    tan_x = tan_y * W / H
    direct = np.ones((H, W), bool)
    mirrored = np.ones((H, W), bool)
    for ox, oy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        xs = (np.arange(W)[None, :] + ox) / W * 2 - 1
        ys = (np.arange(H)[:, None] + oy) / H * 2 - 1
        d = f[None, None] + r[None, None] * (xs * tan_x)[..., None] - u[None, None] * (ys * tan_y)[..., None]
        d = d / np.linalg.norm(d, axis=2, keepdims=True)
        # Directly on R (y = 0, x in [-1, 1], z in [0, 1]).
        t = -eye[1] / d[..., 1]
        hit = eye + d * t[..., None]
        in_r = (t > 0) & (np.abs(hit[..., 0]) < 1 - margin) & (hit[..., 2] > margin) & (hit[..., 2] < 1 - margin)
        # M2 first (x = -1.5, y in [0, 2], z in [-0.5, 1.5]), then reflected onto R.
        t2 = (-1.5 - eye[0]) / d[..., 0]
        h2 = eye + d * t2[..., None]
        on_m2 = (t2 > 0) & (h2[..., 1] > margin) & (h2[..., 1] < 2 - margin) & (h2[..., 2] > -0.5 + margin) & (h2[..., 2] < 1.5 - margin)
        d2 = d * np.array([-1.0, 1.0, 1.0])
        t3 = -h2[..., 1] / d2[..., 1]
        h3 = h2 + d2 * t3[..., None]
        via = on_m2 & (t3 > 0) & (np.abs(h3[..., 0]) < 1 - margin) & (h3[..., 2] > margin) & (h3[..., 2] < 1 - margin)
        direct &= in_r & ~on_m2
        mirrored &= via
    return direct, mirrored


def m1_mirror_seen(failures):
    want = 0.5 / math.pi * SUN_E * math.cos(math.radians(45))
    direct, mirrored = m1_masks()
    img, log = render("m1_mirror_seen", mirror_seen_scene(), 256, ["--bounces", "2"])
    off, _ = render("m1_mirror_seen_off", mirror_seen_scene(), 256, ["--bounces", "2"], {"OHAO_MANIFOLD_OFF": "1"})
    x, xo = img.mean(2), off.mean(2)
    out = {"direct_px": int(direct.sum()), "mirrored_px": int(mirrored.sum()), "closed_form": want}
    for tag, mask in (("direct", direct), ("mirrored", mirrored)):
        m, mo = float(x[mask].mean()), float(xo[mask].mean())
        err = abs(m - want) / want
        out[tag] = {"mean": m, "error": err, "without_step2": mo}
        print("m1_mirror_seen %s (%d px): %.4f, closed form %.4f (%.2f%%); without step 2a %.4f"
              % (tag, int(mask.sum()), m, want, 100 * err, mo))
        if not err <= 0.01:
            failures.append("m1_mirror_seen %s: %.4f against %.4f (%.2f%%, bar 1%%)" % (tag, m, want, 100 * err))
    if not out["mirrored"]["without_step2"] < 0.05 * want:
        failures.append("m1_mirror_seen: without step 2a the mirrored R is %.4f, not missing" % out["mirrored"]["without_step2"])
    if out["mirrored"]["mean"] > 0:
        out["mirrored"]["pixel_std"] = float(x[mirrored].std() / want)
    for line in log.splitlines():
        if "manifold pass" in line:
            out["counters"] = line.strip()
            print("  " + line.strip())
    return out


# m2: m1's room with M1 at roughness 0.1. R's irradiance from a glossy mirror large against its lobe has no closed form;
# manifold_reference integrates OpenPBR's own BSDF (its CPU twin) over the half vector, and splits it at the core.
# The mirrored R within 1% of 0.5 / pi E x the integral; the manifold pass's own light (OHAO_PT_CLASS=128) within 1% of
# the core's share, the camera side's sun after a rough bounce (class 2) the tail's.

def m2_reference(rough):
    exe = os.path.join(ROOT, "build", "Release", "manifold_reference" + (".exe" if os.name == "nt" else ""))
    r = subprocess.run([exe, str(rough)], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError("manifold_reference: " + r.stderr)
    return json.loads(r.stdout)


def m2_glossy_mirror(failures):
    ref = m2_reference(0.1)
    k = 0.5 / math.pi * SUN_E
    want, want_core, want_tail = k * ref["total"], k * ref["core"], k * ref["tail"]
    direct, mirrored = m1_masks(0.12)
    scene = mirror_seen_scene(0.1)
    img, log = render("m2_glossy", scene, 512, ["--bounces", "2"])
    core, _ = render("m2_glossy_core", scene, 512, ["--bounces", "2"], {"OHAO_PT_CLASS": "128"})
    tail, _ = render("m2_glossy_tail", scene, 512, ["--bounces", "2"], {"OHAO_PT_CLASS": "2"})
    x, xc, xt = img.mean(2), core.mean(2), tail.mean(2)
    out = {"reference": ref, "closed_form": want}
    for tag, mask in (("direct", direct), ("mirrored", mirrored)):
        m = float(x[mask].mean())
        err = abs(m - want) / want
        out[tag] = {"mean": m, "error": err, "px": int(mask.sum())}
        print("m2_glossy_mirror %s (%d px): %.4f, reference %.4f (%.2f%%)" % (tag, int(mask.sum()), m, want, 100 * err))
        if not err <= 0.01:
            failures.append("m2_glossy_mirror %s: %.4f against %.4f (%.2f%%, bar 1%%)" % (tag, m, want, 100 * err))
    mc, mt = float(xc[mirrored].mean()), float(xt[mirrored].mean())
    ec = abs(mc - want_core) / want
    et = abs(mt - want_tail) / want
    out["core"] = {"mean": mc, "reference": want_core, "error": ec}
    out["tail"] = {"mean": mt, "reference": want_tail, "error": et}
    print("m2_glossy_mirror mirrored split: the pass %.4f (core %.4f), the camera side %.4f (tail %.4f)" % (mc, want_core, mt, want_tail))
    if not (ec <= 0.01 and et <= 0.01):
        failures.append("m2_glossy_mirror: the split %.4f / %.4f against the core %.4f / tail %.4f" % (mc, mt, want_core, want_tail))
    for line in log.splitlines():
        if "manifold pass" in line:
            out["counters"] = line.strip()
            print("  " + line.strip())
    return out


# ---- m4-m6: step 2b, the sun through a solid seen past the first hit ---------------------------------
# The camera side never renders it (its light samples look above a surface, its shadow rays stop at solids, no BSDF
# ray meets the sun), so without the pass these floors take no light through the solid. Step 2b takes the two
# refractions only (K = 2): every closed form below is that order, (1 - F)^2 through the slab; the orders with internal
# reflections (F^2 ~ 0.002 of it) are no one's. 2b is opt-in (OHAO_MANIFOLD_GLASS=1): its tests turn it on (GLASS).

GLASS = {"OHAO_MANIFOLD_GLASS": "1"}   # step 2b's tests turn it on (opt-in until its work is bounded per dispatch)


def slab_floor():
    f = fresnel(math.cos(math.radians(30)), 1.5)
    return 0.5 / math.pi * SUN_E * math.cos(math.radians(30)) * (1 - f) ** 2


def camera_rays(eye, look, fov, ox, oy):
    """Each pixel's ray through its corner (ox, oy) in [0, 1] (W x H x 3, unit), as the renderer's camera lays them."""
    import numpy as np
    eye = np.array(eye, float)
    f = np.array(look, float) - eye
    f /= np.linalg.norm(f)
    r = np.cross(f, [0.0, 1.0, 0.0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    tan_y = math.tan(math.radians(fov) / 2)
    tan_x = tan_y * W / H
    xs = (np.arange(W)[None, :] + ox) / W * 2 - 1
    ys = (np.arange(H)[:, None] + oy) / H * 2 - 1
    d = f[None, None] + r[None, None] * (xs * tan_x)[..., None] - u[None, None] * (ys * tan_y)[..., None]
    return d / np.linalg.norm(d, axis=2, keepdims=True)


def in_box(p, lo, hi, axes):
    ok = True
    for a in axes:
        ok = ok & (p[..., a] > lo[a]) & (p[..., a] < hi[a])
    return ok


# m4: under c3_slab's slab a mirror (white metal, F 1) stands at x = 2 facing -x, edge-on to the sun (it travels in
# the y-z plane), so it neither shadows the floor nor reflects the sun. The camera under the slab looks into it and
# sees the floor under the slab's middle at camera depth 1: the closed form times the mirror's 1.
M4_CAM, M4_LOOK, M4_FOV = (0.0, 0.6, 0.0), (2.0, 0.3, 0.0), 50.0


def m4_scene():
    s = slab_scene()
    s["entities"] = [e for e in s["entities"] if e.get("Name", {}).get("value") != "Camera"]
    s["entities"].append(ent(1, Name={"value": "Camera"}, Transform={"position": list(M4_CAM), "rotation": look_quat(M4_CAM, M4_LOOK)},
                             Camera={"fovDeg": M4_FOV}))
    s["entities"].append(ent(7, Name={"value": "Mirror"},
                             MeshSource={"source": {"QuadPrim": {"a": [2, 0.05, -1.2], "b": [2, 0.05, 1.2], "c": [2, 0.95, 1.2],
                                                                  "d": [2, 0.95, -1.2], "normal": [-1, 0, 0], "color": [1, 1, 1]}}},
                             Material={"base_color": [1, 1, 1], "base_metalness": 1, "specular_roughness": 0.0}))
    return s


def m4_mask(margin=0.06):
    import numpy as np
    eye = np.array(M4_CAM, float)
    mask = np.ones((H, W), bool)
    for ox, oy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        d = camera_rays(M4_CAM, M4_LOOK, M4_FOV, ox, oy)
        t = (2.0 - eye[0]) / d[..., 0]
        hm = eye + d * t[..., None]
        on = (t > 0) & in_box(hm, [0, 0.05 + margin, -1.2 + margin], [0, 0.95 - margin, 1.2 - margin], (1, 2))
        d2 = d * np.array([-1.0, 1.0, 1.0])
        t2 = -hm[..., 1] / d2[..., 1]
        hf = hm + d2 * t2[..., None]
        mask &= on & (t2 > 0) & in_box(hf, [-2.0, 0, -2.0], [1.7, 0, 1.7], (0, 2))
    return mask


def m4_slab_seen(failures):
    want = slab_floor()
    mask = m4_mask()
    img, _ = render("m4_slab_seen", m4_scene(), 256, ["--bounces", "2"], GLASS)
    off, _ = render("m4_slab_seen_off", m4_scene(), 64, ["--bounces", "2"], {"OHAO_MANIFOLD_OFF": "1"})
    m, mo = float(img.mean(2)[mask].mean()), float(off.mean(2)[mask].mean())
    err = abs(m - want) / want
    print("m4_slab_seen (%d px): the floor in the mirror %.4f, closed form %.4f (%.2f%%); without the pass %.4f"
          % (int(mask.sum()), m, want, 100 * err, mo))
    if not err <= 0.02:
        failures.append("m4_slab_seen: %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (m, want, 100 * err))
    if not mo < 0.01 * want:
        failures.append("m4_slab_seen: without the pass the mirrored floor is %.4f, not missing" % mo)
    return {"px": int(mask.sum()), "mean": m, "closed_form": want, "error": err, "without_pass": mo}


# m5: c3_sphere's ball seen through a periscope: a mirror at 45 degrees (y = z + 2.4, facing down and +z, clear of every
# sun ray to the floor in view) and a camera at (-0.7, 3, 1.6) looking -z into it, whose mirror image is c3_sphere's
# camera exactly; the picture is c3's upside down. The floor is camera depth 1. Against the same numpy simulation kept
# to the paths straight through (order 0), normalised by the lit floor, as c3_sphere: the energy in the shadow within 5%.
M5_CAM = (-0.7, 3.0, 1.6)


def m5_scene():
    s = sphere_scene()
    s["entities"] = [e for e in s["entities"] if e.get("Name", {}).get("value") != "Camera"]
    s["entities"].append(ent(1, Name={"value": "Camera"}, Transform={"position": list(M5_CAM), "rotation": [0, 0, 0, 1]},
                             Camera={"fovDeg": SPHERE_FOV}))
    k = math.sqrt(0.5)
    s["entities"].append(ent(7, Name={"value": "Periscope"},
                             MeshSource={"source": {"QuadPrim": {"a": [-1.9, 2.3, -0.1], "b": [0.5, 2.3, -0.1], "c": [0.5, 3.4, 1.0],
                                                                  "d": [-1.9, 3.4, 1.0], "normal": [0, -k, k], "color": [1, 1, 1]}}},
                             Material={"base_color": [1, 1, 1], "base_metalness": 1, "specular_roughness": 0.0}))
    return s


def ball_seen(failures, name, scene, n_glass=1.5, logs=None):
    # m5's measure: the caustic's energy in the ball's shadow, seen through the periscope, against the simulation.
    import numpy as np
    img, log = render(name, scene, 2048, ["--bounces", "2"], GLASS)
    if logs is not None:
        logs.append(log)
    lum = img.mean(2)[::-1]   # c3_sphere's orientation
    lit = float(np.median(lum[5:30, 5:60]))
    got = lum / lit
    ref, mask = sphere_reference(W, H, orders=1, n_glass=n_glass)
    e_ref, e_got = float(ref[mask].sum()), float(got[mask].sum())
    err = abs(e_got - e_ref) / e_ref
    print("%s: the caustic in the mirrored shadow %.1f, reference (straight through) %.1f (%.2f%%); lit floor %.4f"
          % (name, e_got, e_ref, 100 * err, lit))
    if not err <= 0.05:
        failures.append("%s: the caustic's energy %.1f against the ray-simulated %.1f (%.2f%%, bar 5%%)"
                        % (name, e_got, e_ref, 100 * err))
    return {"energy": e_got, "reference": e_ref, "error": err, "lit": lit}


def m5_ball_seen(failures):
    return ball_seen(failures, "m5_ball_seen", m5_scene())


def modulated_ior(ior, weight):
    # OpenPBR's IOR at a specular_weight (shaders/material/openpbr.slang modulatedIor): F0 scaled by the weight.
    f0 = ((1 - ior) / (1 + ior)) ** 2
    eps = math.copysign(math.sqrt(min(max(weight, 0.0), 1.0 / f0) * f0), ior - 1)
    return (1 + eps) / (1 - eps)


# m9: m5's ball at specular_ior 1.8 and specular_weight 0.5. The BSDF refracts at modulated_ior(1.8, 0.5) = 1.506, so
# must the walk: at the material's raw 1.8 its paths miss the BSDF's peak and the caustic is dark.
def m9_ball_weight(failures):
    s = m5_scene()
    for e in s["entities"]:
        if e.get("Name", {}).get("value") == "Glass":
            e["Material"]["specular_ior"] = 1.8
            e["Material"]["specular_weight"] = 0.5
    return ball_seen(failures, "m9_ball_weight", s, n_glass=modulated_ior(1.8, 0.5))


# m10: m5's ball at IOR 1.33, whose caustic focuses onto the floor. A path near the fold is found again by few seeds and
# weighs much; with the re-find trials capped at 64 the caustic read 30% dark, at any spp. The trials' ceiling (4,096)
# must reach every path here (the log's count at the ceiling 0).
def m10_ball_focus(failures):
    s = m5_scene()
    for e in s["entities"]:
        if e.get("Name", {}).get("value") == "Glass":
            e["Material"]["specular_ior"] = 1.33
    logs = []
    r = ball_seen(failures, "m10_ball_focus", s, n_glass=1.33, logs=logs)
    ceiling = re.search(r"(\d+) at the trials' ceiling", "".join(logs))
    r["at_ceiling"] = int(ceiling.group(1)) if ceiling else None
    if r["at_ceiling"] != 0:
        failures.append("m10_ball_focus: %s paths at the re-find trials' ceiling (want 0)" % r["at_ceiling"])
    return r


# m7: m5's ball as one model of three primitives, a glass top half, a 1 mm opaque triangle at the centre and a glass
# bottom half (build/caustics/split_ball.gltf, written here): two draws of one instance with one glass material. Step
# 2b walks the whole instance, so it must run once for the pair, not once a draw (which read the caustic twice).
def write_split_ball(path, sectors=96, stacks=48):
    import base64
    import struct
    verts, norms = [], []
    for i in range(stacks + 1):
        th = math.pi * i / stacks
        for j in range(sectors + 1):
            ph = 2 * math.pi * j / sectors
            nrm = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            norms.append(nrm)
            verts.append(tuple(0.5 * c for c in nrm))

    def band(i0, i1):
        idx = []
        for i in range(i0, i1):
            for j in range(sectors):
                a, b = i * (sectors + 1) + j, (i + 1) * (sectors + 1) + j
                idx += [a, a + 1, b, a + 1, b + 1, b]   # outward (counter-clockwise seen from outside)
        return idx
    top, bottom = band(0, stacks // 2), band(stacks // 2, stacks)
    tri = [(0.0, 0.0, 0.0), (0.001, 0.0, 0.0), (0.0, 0.0, 0.001)]
    blob = b"".join(struct.pack("<3f", *v) for v in verts) + b"".join(struct.pack("<3f", *n) for n in norms)
    blob += b"".join(struct.pack("<3f", *v) for v in tri) + b"".join(struct.pack("<3f", 0, 1, 0) for _ in tri)
    ib = len(blob)
    blob += struct.pack("<%dI" % len(top), *top) + struct.pack("<%dI" % len(bottom), *bottom) + struct.pack("<3I", 0, 1, 2)
    nv = len(verts)
    views = [{"buffer": 0, "byteOffset": 0, "byteLength": 12 * nv}, {"buffer": 0, "byteOffset": 12 * nv, "byteLength": 12 * nv},
             {"buffer": 0, "byteOffset": 24 * nv, "byteLength": 36}, {"buffer": 0, "byteOffset": 24 * nv + 36, "byteLength": 36},
             {"buffer": 0, "byteOffset": ib, "byteLength": 4 * len(top)},
             {"buffer": 0, "byteOffset": ib + 4 * len(top), "byteLength": 4 * len(bottom)},
             {"buffer": 0, "byteOffset": ib + 4 * (len(top) + len(bottom)), "byteLength": 12}]
    acc = [{"bufferView": 0, "componentType": 5126, "count": nv, "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
           {"bufferView": 1, "componentType": 5126, "count": nv, "type": "VEC3"},
           {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [0.001, 0, 0.001]},
           {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3"},
           {"bufferView": 4, "componentType": 5125, "count": len(top), "type": "SCALAR"},
           {"bufferView": 5, "componentType": 5125, "count": len(bottom), "type": "SCALAR"},
           {"bufferView": 6, "componentType": 5125, "count": 3, "type": "SCALAR"}]
    glass = {"name": "glass", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0},
             "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1}, "KHR_materials_ior": {"ior": 1.5}}}
    opaque = {"name": "opaque", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1], "metallicFactor": 0, "roughnessFactor": 1}}
    gltf = {"asset": {"version": "2.0"}, "extensionsUsed": ["KHR_materials_transmission", "KHR_materials_ior"],
            "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
            "bufferViews": views, "accessors": acc, "materials": [glass, opaque],
            "meshes": [{"name": "top", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 4, "material": 0}]},
                       {"name": "inside", "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 3}, "indices": 6, "material": 1}]},
                       {"name": "bottom", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 5, "material": 0}]}],
            "nodes": [{"mesh": 0}, {"mesh": 1}, {"mesh": 2}], "scenes": [{"nodes": [0, 1, 2]}], "scene": 0}
    with open(path, "w") as f:
        json.dump(gltf, f)


def m7_split_ball(failures):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "split_ball.gltf")
    write_split_ball(path)
    s = m5_scene()
    for e in s["entities"]:
        if e.get("Name", {}).get("value") == "Glass":
            e["MeshSource"] = {"source": {"ModelAsset": {"asset": {"path": os.path.relpath(path, ROOT)}}}}
            e.pop("Material", None)
    return ball_seen(failures, "m7_split_ball", s)


# m6: c3_slab's floor seen through glass: a clear pane (a 5 cm box at x = 3.9 .. 3.95, edge-on to the sun, outside the
# slab) between the camera at (4.5, 0.6, 0) and the floor under the slab, which the camera meets at depth 2 (both
# refractions crossings). Each pixel's closed form is the floor under the slab times the pane's transmittance plus the
# sunlit floor behind the camera (no slab over it) times the pane's reflectance, at its ray's incidence, with the
# pane's inside reflections: T = (1 - F) / (1 + F), R = 2F / (1 + F). Without the pass only the reflection is there, and
# one more term no form here has: the floor under the slab lit by sunlit floor whose light reflected inside the slab on
# the way (glass events are no bounces, so --bounces 1 lets the path through): about 0.003 by a path simulation, within
# the checks' bars. m4's 0.0031 without the pass is the same term.
M6_CAM, M6_LOOK, M6_FOV = (4.5, 0.6, 0.0), (0.5, 0.0, 0.0), 50.0


def m6_scene():
    s = slab_scene()
    s["entities"] = [e for e in s["entities"] if e.get("Name", {}).get("value") != "Camera"]
    s["entities"].append(ent(1, Name={"value": "Camera"}, Transform={"position": list(M6_CAM), "rotation": look_quat(M6_CAM, M6_LOOK)},
                             Camera={"fovDeg": M6_FOV}))
    s["entities"].append(ent(7, Name={"value": "Pane"},
                             MeshSource={"source": {"BoxPrim": {"lo": [3.9, 0.0, -1.5], "hi": [3.95, 1.0, 1.5], "color": [1, 1, 1]}}},
                             Material={"base_color": [1, 1, 1], "specular_roughness": 0.0, "transmission_weight": 1}))
    return s


def m6_through_glass(failures):
    import numpy as np
    eye = np.array(M6_CAM, float)
    mask = np.ones((H, W), bool)
    for ox, oy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        d = camera_rays(M6_CAM, M6_LOOK, M6_FOV, ox, oy)
        t = (3.95 - eye[0]) / d[..., 0]
        hp = eye + d * t[..., None]
        tf = -eye[1] / d[..., 1]
        hf = eye + d * tf[..., None]
        mask &= (t > 0) & in_box(hp, [0, 0.1, -1.3], [0, 0.9, 1.3], (1, 2)) & (tf > 0) & in_box(hf, [-2.0, 0, -2.0], [2.5, 0, 1.7], (0, 2))
    d = camera_rays(M6_CAM, M6_LOOK, M6_FOV, 0.5, 0.5)
    cos_i = np.abs(d[..., 0])
    f = np.vectorize(lambda c: fresnel(float(c), 1.5))(cos_i)
    lit = slab_floor() / (1 - fresnel(math.cos(math.radians(30)), 1.5)) ** 2   # the floor in the sun, no slab
    through, reflected = slab_floor() * (1 - f) / (1 + f), lit * 2 * f / (1 + f)
    want = through + reflected
    img, _ = render("m6_through_glass", m6_scene(), 256, ["--bounces", "1"], GLASS)
    off, _ = render("m6_through_glass_off", m6_scene(), 64, ["--bounces", "1"], {"OHAO_MANIFOLD_OFF": "1"})
    m, w_, mo = float(img.mean(2)[mask].mean()), float(want[mask].mean()), float(off.mean(2)[mask].mean())
    wr = float(reflected[mask].mean())
    err = abs(m - w_) / w_
    print("m6_through_glass (%d px): the floor through the pane %.4f, closed form %.4f (%.2f%%); without the pass %.4f, "
          "the reflection alone %.4f" % (int(mask.sum()), m, w_, 100 * err, mo, wr))
    if not err <= 0.02:
        failures.append("m6_through_glass: %.4f against the closed form %.4f (%.2f%%, bar 2%%)" % (m, w_, 100 * err))
    # Without the pass the light through the pane is missing: what is left is the reflection, within 1% of the whole.
    if not abs(mo - wr) <= 0.01 * w_:
        failures.append("m6_through_glass: without the pass %.4f, the reflection alone %.4f: the floor through the pane "
                        "is not missing" % (mo, wr))
    return {"px": int(mask.sum()), "mean": m, "closed_form": w_, "error": err, "without_pass": mo, "reflection": wr}


def m8_repeat(failures):
    out = serve_twice("m8", mirror_seen_scene(0.1), {"bounces": 2})
    print("m8_repeat: two captures in one process %s" % ("identical" if out["identical"] else "DIFFER"))
    if not out["identical"]:
        failures.append("m8_repeat: two captures with the manifold pass in one process differ (%s)" % out.get("errors"))
    return out


CASES = {"c1_split_only": c1_split_only, "c1_mirror": c1_mirror, "c1_emitter": c1_emitter, "c3_slab": c3_slab, "c3_slab_guided": c3_slab_guided, "c3_sphere": c3_sphere, "c6_repeat": c6_repeat, "c2_split": c2_split, "c4_through_glass": c4_through_glass, "c5_showcase": c5_showcase, "m1_mirror_seen": m1_mirror_seen, "m2_glossy_mirror": m2_glossy_mirror, "m4_slab_seen": m4_slab_seen, "m5_ball_seen": m5_ball_seen, "m6_through_glass": m6_through_glass, "m7_split_ball": m7_split_ball, "m8_repeat": m8_repeat, "m9_ball_weight": m9_ball_weight, "m10_ball_focus": m10_ball_focus}


# Step 2b through a ball at 2,048 spp: some frames' re-find walks ran for seconds (m9: over 15 minutes), a dispatch the
# GPU cannot preempt, until that work is bounded per dispatch. Run only by name or with --ball, one at a time.
BALL = ["m5_ball_seen", "m7_split_ball", "m9_ball_weight", "m10_ball_focus"]


def main(argv):
    # --showcase runs c5 alone (slow: a 8192 spp reference), --ball the ball cases; without arguments every other case.
    if "--showcase" in argv:
        names = ["c5_showcase"]
    elif "--ball" in argv:
        names = BALL
    else:
        names = argv[1:] or [n for n in CASES if n != "c5_showcase" and n not in BALL]
        if len(argv) == 1:
            print("check_caustics: left out %s (--ball runs them) and c5_showcase (--showcase)" % ", ".join(BALL))
    failures, results = [], {}
    for n in names:
        results[n] = CASES[n](failures)
    print(json.dumps({"suite": "check_caustics", "passed": len(names) - len(failures), "failed": len(failures),
                      "failures": failures, "results": results}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
