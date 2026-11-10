"""One `ohao serve` process for a whole check: one Vulkan device, pipelines compiled once, scenes loaded and
captured in turn. A check that launched `ohao run` per render paid the device and the pipelines every time.

    with Serve("build/material") as s:
        s.load("build/material/furnace.scene.json")
        img = s.capture("build/material/furnace", mode="rt_offline", spp=64, width=128, height=72)   # HxWx3 float
"""
import json
import os
import queue
import subprocess
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "renderer"))
from pfm import read_pfm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + (".exe" if os.name == "nt" else ""))


class Serve:
    def __init__(self, out, args=(), env=None):
        """`out`: the directory (relative to the repo) captures may write; `args`: more `ohao serve` options."""
        os.makedirs(os.path.join(ROOT, out), exist_ok=True)
        self.out = out
        self.err = open(os.path.join(ROOT, out, "serve.stderr.txt"), "wb")
        self.proc = subprocess.Popen([OHAO, "serve", "--out", out, *args], cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.err, env=env)
        self.lines = queue.Queue()
        self.next = 0
        threading.Thread(target=self._pump, daemon=True).start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def call(self, method, params=None):
        """A native method's result; raises on an error reply or a dead server."""
        self.next += 1
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method,
                                           "params": params or {}}) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            line = self.lines.get(timeout=300)
            if line is None:
                raise RuntimeError("ohao serve ended (see %s/serve.stderr.txt)" % self.out)
            reply = json.loads(line)
            if "method" not in reply and reply.get("id") == self.next:
                if "error" in reply:
                    raise RuntimeError("%s: %s" % (method, reply["error"]))
                return reply["result"]

    def load(self, scene):
        """Replaces the world with the scene file at `scene` (relative to the repo)."""
        return self.call("scene.load", {"path": scene, "replace": True})

    def capture(self, stem, **params):
        """Renders the world (render.capture's params) into `stem`.png and `stem`.pfm and returns the HDR image."""
        self.call("render.capture", {"path": stem + ".png", "hdr": stem + ".pfm", **params})
        return read_pfm(os.path.join(ROOT, stem + ".pfm"))

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=120)
        self.err.close()
        return self.proc.returncode
