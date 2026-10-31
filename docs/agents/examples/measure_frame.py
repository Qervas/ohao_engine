"""Measure a frame through ohao serve: settings, a capture with timings, render.stats.

A stdlib-only client of the native protocol (one JSON-RPC request per line). It loads
Cornell headless, turns SSS off, captures one 640x360 deferred frame with its per-pass
timings, and checks render.stats for that frame agrees. Run from the repo root:
    python docs/agents/examples/measure_frame.py
"""
import json
import os
import subprocess

exe = os.path.abspath("build/Release/ohao.exe")
p = subprocess.Popen([exe, "serve", "--out", "build/doc_tmp/measure", "assets/scenes/cornell_box.scene.json"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
next_id = 0


def call(method, params=None):
    global next_id
    next_id += 1
    p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params or {}}) + "\n").encode())
    p.stdin.flush()
    reply = json.loads(p.stdout.readline())
    if "error" in reply:
        raise SystemExit(method + ": " + reply["error"]["message"])
    return reply["result"]


settings = call("render.settings", {"set": {"sss": False}})
shot = call("render.capture", {"inline": True, "maxSize": 64, "width": 640, "height": 360, "timings": True})
stats = call("render.stats", {"frames": 1})
gpu = call("engine.info").get("gpu", {})
p.stdin.close()
names = [t[0] for t in shot["timings"]]
print("exit", p.wait(), "| sss", settings["sss"], "| gpu named:", bool(gpu.get("name")))
print("passes:", names[0], "...", names[-1], "| sss ran:", "sss" in names)
print("capture gpuMs == render.stats gpuMs:", shot["gpuMs"] == stats["gpuMs"], "| passes sum within it:",
      sum(t[1] for t in shot["timings"]) <= shot["gpuMs"] * 1.05 + 0.01)
