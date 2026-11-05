"""RT GI fireflies under an outdoor sky (docs/changes/2026-10-28-rt-gi-fireflies.md).

The play scene from the player's spawn, looking west: the walls lit by the sun and the open sky,
the gun in view. `ohao serve` (headless) captures it with RT GI on and with RT GI off, both 4 frames
after a reset (what a moving camera shows: GI's history is that young after every move). The
difference of the two is what GI adds. A firefly is a pixel GI made a bright speck: its difference
more than 12 levels (8-bit luma) above the difference's 5x5 median (GI raised it, not its
neighbours), and the GI-on image itself more than 12 levels above its own 5x5 median (a speck, not
a thin feature GI lights differently from what is behind it, nor a dark speck of the GI-off
image). Edges and texture common to both images cancel.

With the HDRI the right way up (2026-10-29), the first test alone counted 363 on this view: 34 GI
specks, 76 dark specks of the GI-off image and 253 thin features (the rifle, fingers, the gun's
edge) lit by the real sky's bounce; Frank's ruling that day made the second test part of it.

With the HDRI's sun in the environment GI's rays read, this view had 441 of them, in clusters on
the sunward enemy, its rifle and the gun; with the sun separated (render/rt/env_sun.hpp: the
scene's directional light stands for it) 88, scattered along silhouettes where GI's
half-resolution upsample meets an edge. The gate is 150.

The engine log must also say the sun was found and taken out of the lighting environment.

  python tests/renderer/check_fireflies.py   # exit 0 pass, 1 fail; last line the JSON summary
"""
import json
import os
import queue
import subprocess
import sys
import threading

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + (".exe" if os.name == "nt" else ""))
OUT = "build/check_fireflies"
SCENE = "assets/scenes/blockout_play.scene.json"
VIEW = ([1.5, 0.0, 2.2], 90.0)      # the spawn, looking west
CAPTURE = {"frames": 4, "post": ["taa", "bloom"], "width": 1280, "height": 720}
SPIKE_LEVELS = 12.0
MAX_FIREFLIES = 150


class Serve:
    def __init__(self):
        os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
        self.err = open(os.path.join(ROOT, OUT, "serve.stderr.txt"), "wb")
        self.proc = subprocess.Popen([OHAO, "serve", "--out", OUT], cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.err)
        self.lines = queue.Queue()
        self.next = 0
        threading.Thread(target=self._pump, daemon=True).start()
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "check_fireflies", "version": "1"}})

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


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return a @ np.array([0.2126, 0.7152, 0.0722])


def median5(a):
    p = np.pad(a, 2, mode="edge")
    h, w = a.shape
    stack = np.stack([p[dy:dy + h, dx:dx + w] for dy in range(5) for dx in range(5)])
    return np.median(stack, axis=0)


def fireflies(on, off):
    a = luma(on)
    d = a - luma(off)
    raised = (d - median5(d)) > SPIKE_LEVELS
    speck = (a - median5(a)) > SPIKE_LEVELS
    return int((raised & speck).sum())


def main():
    failures = []
    s = Serve()
    try:
        s.tool("scene_load", {"path": SCENE, "seed": "0000000000000007"})
        s.tool("world_step", {"frames": 1})
        s.tool("player_teleport", {"pos": VIEW[0], "yawDeg": VIEW[1]})
        s.tool("world_step", {"frames": 1})
        on, off = OUT + "/gi_on.png", OUT + "/gi_off.png"
        s.tool("render_capture", dict(CAPTURE, path=on))
        s.tool("render_settings", {"set": {"rtGI": {"enabled": False}}})
        s.tool("render_capture", dict(CAPTURE, path=off))
        lines = s.tool("log_read", {"since": 0, "channel": "gpu", "limit": 200})["lines"]
    finally:
        code = s.close()
    n = fireflies(os.path.join(ROOT, on), os.path.join(ROOT, off))
    print("fireflies: %d (gate %d)" % (n, MAX_FIREFLIES))
    if n > MAX_FIREFLIES:
        failures.append("fireflies: %d over %d" % (n, MAX_FIREFLIES))
    sun = [l["msg"] for l in lines if "sun" in l["msg"]]
    print("\n".join(sun) or "no sun record in the log")
    if not any("lights without its sun" in m for m in sun):
        failures.append("the log does not say the environment's sun was taken out")
    if code != 0:
        failures.append("ohao serve exit %d" % code)
    print(json.dumps({"suite": "check_fireflies", "passed": 2 - min(2, len(failures)), "failed": len(failures),
                      "failures": failures}, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
