"""The environment map's orientation in every renderer (ROADMAP row 16, the upside-down HDRI).

An equirectangular HDRI's first row is the zenith: stb_image loads row 0 first, so +Y must map to
v = 0 and -Y to v = 1. This writes a 64x32 HDRI whose upper half (the sky) is blue and whose lower
half (the ground) is orange, and a white floor under it, and captures it through `ohao serve` three
ways: deferred with RT GI (gi_trace's escaped rays), deferred with RT GI off (the prefiltered
environment and its irradiance), and the path tracer (its miss, the CDF's importance sampling). The
floor, lit by the upper hemisphere, and the sky above the horizon behind it must come out blue in all
three: mean blue over mean red at least 1.5. Upside down they come out orange.

The scene has no light, so this also holds the renderers to the same scene: deferred lights a scene
without lights by its environment alone (no fallback sun) and draws the environment behind it (not its
procedural sky), as the path tracer does.

  python tests/renderer/check_env_orientation.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import math
import os
import queue
import subprocess
import sys
import threading

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + (".exe" if os.name == "nt" else ""))
OUT = "build/check_env_orientation"
SKY = (0.2, 0.4, 1.0)       # upper half: elevation > 0
GROUND = (1.0, 0.4, 0.2)    # lower half
W, H = 64, 32
MIN_RATIO = 1.5             # blue over red, both regions
SIZE = {"width": 320, "height": 180}
CAPTURES = [
    ("deferred_gi", {"rtGI": {"enabled": True}}, dict(SIZE, mode="deferred", frames=16)),
    ("deferred_ibl", {"rtGI": {"enabled": False}}, dict(SIZE, mode="deferred", frames=1)),
    ("path_traced", {}, dict(SIZE, mode="rt_offline", spp=64, denoise="none")),
]


def rgbe(c):
    m = max(c)
    if m < 1e-32:
        return bytes(4)
    f, e = math.frexp(m)
    s = f * 256.0 / m
    return bytes([int(c[0] * s), int(c[1] * s), int(c[2] * s), e + 128])


def write_hdr(path):
    # Flat (not run-length encoded) scanlines; the first pixel's bytes are not the RLE marker (2, 2).
    rows = [rgbe(SKY if y < H // 2 else GROUND) * W for y in range(H)]
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (H, W))
        f.write(b"".join(rows))


def write_scene(path, hdr):
    # The camera at 2 m, pitched 20 degrees down: the floor below the middle, the sky in the top rows
    # (the frame's top edge looks 7.5 degrees above the horizon).
    half = math.radians(-20.0) / 2.0
    scene = {
        "format": "ohao.scene", "version": 1,
        "components": {"Camera": 1, "Environment": 1, "Material": 1, "MeshSource": 1, "Name": 1,
                       "Transform": 1},
        "entities": [
            {"id": "e0e0000000000001", "Name": {"value": "Camera"},
             "Transform": {"position": [0, 2, 4], "rotation": [math.sin(half), 0, 0, math.cos(half)]},
             "Camera": {"fovDeg": 55}},
            {"id": "e0e0000000000002", "Name": {"value": "Environment"},
             "Environment": {"hdri": {"path": hdr}}},
            {"id": "e0e0000000000003", "Name": {"value": "Floor"},
             "MeshSource": {"source": {"BoxPrim": {"lo": [-6, -0.1, -8], "hi": [6, 0, 6], "color": [0.8, 0.8, 0.8]}}},
             "Material": {"base_color": [0.8, 0.8, 0.8], "specular_roughness": 0.9}},
        ],
    }
    with open(path, "w", newline="\n") as f:
        json.dump(scene, f, indent=2)


class Serve:
    def __init__(self):
        self.err = open(os.path.join(ROOT, OUT, "serve.stderr.txt"), "wb")
        self.proc = subprocess.Popen([OHAO, "serve", "--out", OUT], cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.err)
        self.lines = queue.Queue()
        self.next = 0
        threading.Thread(target=self._pump, daemon=True).start()
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "check_env_orientation", "version": "1"}})

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def request(self, method, params):
        self.next += 1
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method,
                                           "params": params}) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            line = self.lines.get(timeout=300)
            if line is None:
                raise RuntimeError("ohao serve ended")
            reply = json.loads(line)
            if "method" not in reply and reply.get("id") == self.next:
                if "error" in reply:
                    raise RuntimeError("%s: %s" % (method, reply["error"]))
                return reply["result"]

    def tool(self, name, args):
        res = self.request("tools/call", {"name": name, "arguments": args})
        text = next(c["text"] for c in res["content"] if c["type"] == "text")
        if res.get("isError"):
            raise RuntimeError("%s: %s" % (name, text))
        return json.loads(text)

    def close(self):
        self.proc.stdin.close()
        code = self.proc.wait(timeout=120)
        self.err.close()
        return code


def blue_over_red(region):
    mean = region.reshape(-1, 3).mean(axis=0)
    return float(mean[2] / max(mean[0], 1e-6)), [round(float(v), 1) for v in mean]


def main():
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    hdr, scene = OUT + "/sky.hdr", OUT + "/env_orientation.scene.json"
    write_hdr(os.path.join(ROOT, hdr))
    write_scene(os.path.join(ROOT, scene), hdr)
    failures, results, checks = [], {}, 0
    s = Serve()
    try:
        s.tool("scene_load", {"path": scene})
        for name, settings, capture in CAPTURES:
            if settings:
                s.tool("render_settings", {"set": settings})
            png = "%s/%s.png" % (OUT, name)
            s.tool("render_capture", dict(capture, path=png))
            img = np.asarray(Image.open(os.path.join(ROOT, png)).convert("RGB"), dtype=np.float64)
            h, w = img.shape[:2]
            regions = {"floor": img[h * 3 // 4:, w // 3: w * 2 // 3], "sky": img[: h // 12, :]}
            results[name] = {}
            for region, pixels in regions.items():
                ratio, mean = blue_over_red(pixels)
                results[name][region] = {"blueOverRed": round(ratio, 3), "meanRGB": mean}
                print("%-13s %-5s blue/red %6.3f  mean RGB %s" % (name, region, ratio, mean))
                checks += 1
                if ratio < MIN_RATIO:
                    failures.append("%s %s: blue/red %.3f under %.1f (the environment is upside down)"
                                    % (name, region, ratio, MIN_RATIO))
    finally:
        code = s.close()
    if code != 0:
        failures.append("ohao serve exit %d" % code)
    with open(os.path.join(ROOT, OUT, "results.json"), "w", newline="\n") as f:
        json.dump(results, f, indent=2)
    print(json.dumps({"suite": "check_env_orientation", "passed": checks - min(checks, len(failures)),
                      "failed": len(failures), "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
