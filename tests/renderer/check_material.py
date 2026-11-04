"""OpenPBR in the renderers, against closed forms (docs/plans/2026-10-29-openpbr-step1.md, T3-T5).

White furnace: a sphere under a uniform environment of radiance 1, no lights. A white material that
neither absorbs nor emits (a dielectric of base_color 1 over its diffuse, a metal of base_color 1)
reflects all it receives, so the sphere is invisible: radiance 1 on its face and its rim. Each
material renders path traced (256 samples) and deferred (32 frames: RT GI's escaped rays and the split
sum of the prefiltered environment, weighed by OpenPBR's own directional albedos), and the mean over
the sphere's disc and over its rim must be within 1% of 1.

The closed box: six walls of Lambertian albedo rho (specular_weight 0, diffuse roughness 0) each
emitting Le, the camera inside, no lights. Every point's radiance is Le / (1 - rho) with every
bounce. With N bounces (0: direct light only, which here is the walls' light reflected once) it is
Le (1 - rho^(N+2)) / (1 - rho). Path traced: 2 within 1% (rho 0.5, Le 1, the reference's bounces),
1.75 at --bounces 1, 1.5 at --bounces 0; the realtime profile (the same tracer at 2 bounces, then the
radiance cache) 2 within 3%, the same bits captured twice in one process (across processes the driver
may build the BVH differently: a ray on a shared edge takes either triangle, its hit moves by an ulp,
and since emissive triangles are lights (F3) that can change the cache's integer sums). Deferred: RT GI with its radiance cache behind each
hit (frontier F5a: every bounce, one more a frame) gives Le / (1 - rho), 2, within 3% once converged
(256 frames: at this size a cell takes about one sample a frame, and each bounce waits on the one before;
32 frames gave 0.937 of it); one bounce gave 1.5

  python tests/renderer/check_material.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import math
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_parity import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe")
OUT = "build/material"
W, H = 320, 180
FOV = 30.0
CAM_D = 3.0
SPHERE_R = 0.5
FURNACE_TOLERANCE = 0.01
BOX_TOLERANCE = 0.01
BOX_DEFERRED_TOLERANCE = 0.03
BOX_CACHE_TOLERANCE = 0.03   # the radiance cache is a biased estimator: 256 frames reach 1.4% under 2


def quat_x(deg):
    h = math.radians(deg) / 2
    return [math.sin(h), 0.0, 0.0, math.cos(h)]


def write_white_hdr(path, w=16, h=8):
    # Radiance RGBE, flat scanlines: (128, 128, 128, 129) is exactly 1.0 in every channel.
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (h, w))
        f.write(bytes([128, 128, 128, 129]) * (w * h))


# --caustics: every rt_offline capture with caustics' light pass (sub-project 24); each case's bar unchanged.
CAUSTICS = "--caustics" in sys.argv


def capture(name, scene, mode, args):
    rel = "%s/%s" % (OUT, name)
    with open(os.path.join(ROOT, rel + ".scene.json"), "w", newline="\n") as f:
        json.dump(scene, f, indent=1)
    cmd = [OHAO, "run", rel + ".scene.json", "--render", "%s.%s.png" % (rel, mode), "--hdr", "%s.%s.pfm" % (rel, mode),
           "--mode", mode, "--size", "%dx%d" % (W, H)] + args
    if CAUSTICS and mode == "rt_offline":
        cmd.append("--caustics")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        return None, "exit %d: %s" % (r.returncode, r.stdout.strip()[-200:])
    return read_pfm(os.path.join(ROOT, "%s.%s.pfm" % (rel, mode))), None


def capture_twice(name, scene, mode, spp):
    # Two captures of one world in one process, through a script.
    rel = "%s/%s" % (OUT, name)
    with open(os.path.join(ROOT, rel + ".scene.json"), "w", newline="\n") as f:
        json.dump(scene, f, indent=1)
    with open(os.path.join(ROOT, rel + ".jsonl"), "w", newline="\n") as f:
        for tag in "ab":
            f.write(json.dumps({"method": "render.capture",
                                "params": {"path": "%s_%s.png" % (rel, tag), "hdr": "%s_%s.pfm" % (rel, tag),
                                           "mode": mode, "spp": spp, "denoise": "none", "width": W,
                                           "height": H}}) + "\n")
    r = subprocess.run([OHAO, "run", rel + ".scene.json", "--script", rel + ".jsonl"], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, None, "exit %d: %s" % (r.returncode, r.stdout.strip()[-200:])
    return (read_pfm(os.path.join(ROOT, rel + "_a.pfm")), read_pfm(os.path.join(ROOT, rel + "_b.pfm")), None)


def furnace_scene(material):
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Material": 1, "MeshSource": 1, "Name": 1, "Transform": 1},
        "entities": [
            {"id": "f0f0000000000001", "Name": {"value": "Camera"}, "Transform": {"position": [0, 0, CAM_D]},
             "Camera": {"fovDeg": FOV}},
            {"id": "f0f0000000000002", "Name": {"value": "Sky"},
             "Environment": {"hdri": {"path": OUT + "/white.hdr"}}},
            {"id": "f0f0000000000003", "Name": {"value": "Sphere"},
             "MeshSource": {"source": {"SpherePrim": {"radius": SPHERE_R, "sectors": 96, "stacks": 48}}},
             "Material": material},
        ],
    }


def sphere_masks():
    # The sphere's disc in the frame (camera on +Z looking at it): its face (within 70% of the
    # disc's radius) and its rim (85-97%).
    ys, xs = np.mgrid[0:H, 0:W]
    focal = (H / 2) / math.tan(math.radians(FOV / 2))
    disc = focal * math.tan(math.asin(SPHERE_R / CAM_D))
    r = np.hypot(xs + 0.5 - W / 2, ys + 0.5 - H / 2) / disc
    return r < 0.7, (r > 0.85) & (r < 0.97)


FURNACE = [
    ("dielectric_r03", {"base_color": [1, 1, 1], "specular_roughness": 0.3}),
    ("dielectric_r1_diff1", {"base_color": [1, 1, 1], "specular_roughness": 1.0, "base_diffuse_roughness": 1.0}),
    ("dielectric_ior3_r06", {"base_color": [1, 1, 1], "specular_roughness": 0.6, "specular_ior": 3.0}),
    ("metal_r03", {"base_color": [1, 1, 1], "base_metalness": 1.0, "specular_roughness": 0.3}),
    ("metal_r1", {"base_color": [1, 1, 1], "base_metalness": 1.0, "specular_roughness": 1.0}),
    # OpenPBR step 2: a clear coat over a dielectric and over a metal, fuzz over a dielectric, an
    # anisotropic metal (docs/plans/2026-10-31-openpbr-step2.md, T3 and T4).
    ("coat_dielectric", {"base_color": [1, 1, 1], "coat_weight": 1.0, "coat_roughness": 0.1}),
    ("coat_metal_r05", {"base_color": [1, 1, 1], "base_metalness": 1.0, "specular_roughness": 0.5,
                        "coat_weight": 1.0, "coat_roughness": 0.3}),
    ("fuzz_dielectric", {"base_color": [1, 1, 1], "fuzz_weight": 1.0, "fuzz_roughness": 0.5}),
    ("aniso_metal", {"base_color": [1, 1, 1], "base_metalness": 1.0, "specular_roughness": 0.4,
                     "specular_roughness_anisotropy": 0.7}),
    # OpenPBR step 3: white glass, smooth and rough, solid and thin-walled
    # (docs/plans/2026-10-31-openpbr-step3.md, T3 and T4): what it reflects and refracts is all it gets.
    ("glass_r005", {"base_color": [1, 1, 1], "specular_roughness": 0.05, "transmission_weight": 1.0}),
    ("glass_r05", {"base_color": [1, 1, 1], "specular_roughness": 0.5, "transmission_weight": 1.0}),
    ("glass_thin", {"base_color": [1, 1, 1], "specular_roughness": 0.2, "transmission_weight": 1.0,
                    "geometry_thin_walled": True}),
    # OpenPBR step 4: a white subsurface, a short, a long and a dense mean free path (the path tracer
    # walks it; deferred shades its local lobe), docs/plans/2026-11-01-openpbr-step4.md T2. A white
    # volume never absorbs, so a walk ends only by leaving: its step count's tail (~ N^-1/2) passes
    # the walk's 1024-step cap for free paths under ~1/50 of the object, where a capped walk leaves
    # along its direction (the dense case: blue 1.25 mm in this 1 m sphere, 1/800; read 0.986 at 1/200
    # when a capped walk was dropped).
    ("subsurface_short", {"base_color": [1, 1, 1], "specular_roughness": 0.4, "subsurface_weight": 1.0,
                          "subsurface_color": [1, 1, 1], "subsurface_radius": 0.05}),
    ("subsurface_long", {"base_color": [1, 1, 1], "specular_roughness": 0.4, "subsurface_weight": 1.0,
                         "subsurface_color": [1, 1, 1], "subsurface_radius": 0.5, "subsurface_scatter_anisotropy": 0.5}),
    ("subsurface_dense", {"base_color": [1, 1, 1], "specular_roughness": 0.4, "subsurface_weight": 1.0,
                          "subsurface_color": [1, 1, 1], "subsurface_radius": 0.005}),
]


def furnace(modes, failures, results):
    write_white_hdr(os.path.join(ROOT, OUT, "white.hdr"))
    face, rim = sphere_masks()
    checks = 0
    for name, material in FURNACE:
        for mode, args in modes:
            checks += 1
            img, err = capture("furnace_" + name, furnace_scene(material), mode, args)
            if err:
                failures.append("furnace %s %s: %s" % (name, mode, err))
                continue
            lum = img @ np.array([0.2126, 0.7152, 0.0722])
            f, g = float(lum[face].mean()), float(lum[rim].mean())
            results["furnace_%s_%s" % (name, mode)] = [round(f, 4), round(g, 4)]
            ok = abs(f - 1.0) <= FURNACE_TOLERANCE and abs(g - 1.0) <= FURNACE_TOLERANCE
            print("furnace %-22s %-10s face %.4f rim %.4f %s" % (name, mode, f, g, "ok" if ok else "FAIL"))
            if not ok:
                failures.append("furnace %s %s: face %.4f rim %.4f, not 1 within %g%%" % (name, mode, f, g,
                                                                                      FURNACE_TOLERANCE * 100))
    return checks


def box_scene(rho, le):
    return {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Material": 1, "MeshSource": 1, "Name": 1, "Transform": 1},
        "entities": [
            {"id": "b0b0000000000001", "Name": {"value": "Camera"}, "Transform": {"position": [0, 0, 0.5]},
             "Camera": {"fovDeg": 60}},
            {"id": "b0b0000000000002", "Name": {"value": "Box"},
             "MeshSource": {"source": {"BoxPrim": {"lo": [-2, -2, -2], "hi": [2, 2, 2], "color": [rho, rho, rho]}}},
             "Material": {"base_color": [rho, rho, rho], "specular_weight": 0, "base_diffuse_roughness": 0,
                          "emission_luminance": le}},
        ],
    }


def closed_box(failures, results):
    rho, le = 0.5, 1.0
    checks = 0
    cases = [("rt_offline", None, le / (1 - rho)), ("rt_offline", 1, le * (1 + rho + rho * rho)),
             ("rt_offline", 0, le * (1 + rho)),
             # The realtime profile: 2 bounces, then the radiance cache, which carries every later
             # bounce (render/rt/radiance_cache.hpp): the whole closed form, within the cache's bias.
             ("rt_realtime", None, le / (1 - rho))]
    for mode, bounces, expected in cases:
        checks += 1
        args = ["--spp", "256", "--denoise", "none"] + (["--bounces", str(bounces)] if bounces is not None else [])
        tag = ("all" if bounces is None else str(bounces)) + ("" if mode == "rt_offline" else "_realtime")
        img, err = capture("box_%s" % tag, box_scene(rho, le), mode, args)
        if err:
            failures.append("closed box, bounces %s: %s" % (tag, err))
            continue
        got = float((img @ np.array([0.2126, 0.7152, 0.0722])).mean())
        ratio = got / expected
        results["box_%s" % tag] = round(ratio, 4)
        ok = abs(ratio - 1.0) <= (BOX_TOLERANCE if mode == "rt_offline" else BOX_CACHE_TOLERANCE)
        print("closed box  bounces %-4s expected %.4f got %.4f (x%.4f) %s" % (tag, expected, got, ratio,
                                                                          "ok" if ok else "FAIL"))
        if not ok:
            failures.append("closed box, bounces %s: x%.4f of %.4f" % (tag, ratio, expected))
    # The cache sums integers and is emptied by the capture's reset: two captures in one process (one
    # BVH) are the same bits.
    checks += 1
    a, b, err = capture_twice("box_repeat", box_scene(rho, le), "rt_realtime", 16)
    same = a is not None and b is not None and np.array_equal(a, b)
    print("closed box  realtime twice: %s" % ("the same bits" if same else "DIFFERENT"))
    if not same:
        failures.append("closed box, realtime captured twice: %s" % (err or "different images"))
    checks += 1
    expected = le / (1 - rho)
    img, err = capture("box_deferred", box_scene(rho, le), "deferred", ["--frames", "256"])
    if err:
        failures.append("closed box, deferred: %s" % err)
        return checks
    got = float((img @ np.array([0.2126, 0.7152, 0.0722])).mean())
    ratio = got / expected
    results["box_deferred"] = round(ratio, 4)
    ok = abs(ratio - 1.0) <= BOX_DEFERRED_TOLERANCE
    print("closed box  deferred     expected %.4f got %.4f (x%.4f) %s" % (expected, got, ratio, "ok" if ok else "FAIL"))
    if not ok:
        failures.append("closed box, deferred: x%.4f of %.4f" % (ratio, expected))
    return checks


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    failures, results = [], {}
    # Both path tracer profiles run one tracer (rt/path_tracer.slang): the realtime one answers as
    # the reference does where its bounces reach (a convex sphere under the sky: one).
    checks = furnace([("rt_offline", ["--spp", "256", "--denoise", "none"]),
                      ("rt_realtime", ["--spp", "256", "--denoise", "none"]),
                      ("deferred", ["--frames", "32"])], failures, results)
    checks += closed_box(failures, results)
    with open(os.path.join(ROOT, OUT, "material.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_material", "passed": checks - len(failures), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
