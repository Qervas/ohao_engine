#!/usr/bin/env python3
"""
e2e_mcp -- test 19 (sub-project 4, step 11): an MCP client drives the live engine end to end.

A stdlib-only MCP client (newline-delimited JSON-RPC on the server's stdio, every call a
`tools/call`) connects to `ohao serve --window` (a hidden window, validation on) and runs
the acceptance session of the spec's test 19:

  initialize -> load helmet_game -> summary -> apply an edit -> snapshot -> record ->
  inject a 61-step flight -> step with watch -> capture inline and to a file -> frame
  stats (sub-project 2b, test 14) -> log read ->
  stop recording -> restore -> replay (every checkpoint matches) -> capture again

and requires: the flight moves the camera as injected, the replay reaches the same world
hash, the second capture is bit-identical to the first (the PNG files byte for byte, the
inline JPEGs too), and the server exits 0 with validation clean through teardown and no
frame read back unasked.

Usage (from the repo root):  python tests/serve/e2e_mcp.py
The last stdout line is the shared JSON summary. Scratch output: build/e2e_tmp/<pid>/.
Needs a GPU; no Python packages.
"""
import base64
import json
import os
import queue
import shutil
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + EXE)
TMP = os.path.join(ROOT, "build", "e2e_tmp", str(os.getpid()))
OUT = os.path.relpath(TMP, ROOT).replace(os.sep, "/")
SCENE = "assets/scenes/helmet_game.scene.json"
SEED = "00000000000000e2"

passed = 0
failed = 0
failures = []


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
        print("  ok    " + what)
    else:
        failed += 1
        failures.append(what)
        print("  FAIL  " + what)


class McpClient:
    """MCP over a child's stdio: one JSON-RPC message per line each way."""

    def __init__(self, argv, env):
        os.makedirs(TMP, exist_ok=True)
        self.err_path = os.path.join(TMP, "server.stderr.txt")
        self.err = open(self.err_path, "wb")
        self.proc = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, env=env)
        self.lines = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def _send(self, msg):
        self.proc.stdin.write((json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method, params=None, timeout=300):
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                return None
            if line is None:
                return None
            reply = json.loads(line)
            if "method" not in reply and reply.get("id") == rid:
                return reply

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)

    def tool(self, name, arguments=None):
        """(result object, image block or None, isError) of one tools/call."""
        reply = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        if reply is None or "result" not in reply:
            return None, None, True
        content = reply["result"].get("content", [])
        text = next((c["text"] for c in content if c.get("type") == "text"), "null")
        image = next((c for c in content if c.get("type") == "image"), None)
        return json.loads(text), image, bool(reply["result"].get("isError"))

    def close(self, timeout=120):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = "timeout"
        self.err.close()
        with open(self.err_path, "rb") as f:
            return code, f.read().decode("utf-8", "replace")


def read_bytes(rel):
    try:
        with open(os.path.join(ROOT, rel), "rb") as f:
            return f.read()
    except OSError:
        return None


def camera_position(result):
    entities = (result or {}).get("watch", {}).get("entities", [])
    return entities[0].get("Transform", {}).get("position") if entities else None


def main():
    if not os.path.exists(OHAO):
        print("ohao is not built: " + OHAO)
        return 1
    shutil.rmtree(TMP, ignore_errors=True)
    print("e2e_mcp (sub-project 4, test 19)")
    env = dict(os.environ)
    env["OHAO_VALIDATION"] = "1"
    c = McpClient([OHAO, "serve", "--window", "960x540", "--hidden", "--out", OUT], env)

    init = c.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "e2e_mcp", "version": "1"}})
    ok = init is not None and init.get("result", {}).get("protocolVersion") == "2025-11-25"
    check(ok and init["result"]["serverInfo"]["name"] == "ohao" and "tools" in init["result"]["capabilities"],
          "initialize: MCP 2025-11-25, tools and logging")
    c.notify("notifications/initialized")
    tools = c.request("tools/list")
    names = [t["name"] for t in (tools or {}).get("result", {}).get("tools", [])]
    check(len(names) == 23 and "render_capture" in names and "render_stats" in names and "input_replay" in names,
          "tools/list: 23 tools")

    info, _, err = c.tool("engine_info")
    check(not err and info.get("host") == "window" and info.get("time", {}).get("mode") == "lockstep",
          "engine_info: a window host in lockstep")
    loaded, _, err = c.tool("scene_load", {"path": SCENE, "seed": SEED})
    check(not err and loaded.get("entities", 0) > 0, "scene_load helmet_game (%s entities)" % (loaded or {}).get("entities"))
    summary, _, err = c.tool("world_summary", {"depth": 1})
    check(not err and summary.get("entities") == loaded.get("entities") and summary.get("outline"),
          "world_summary: the outline of the loaded scene")

    found, _, err = c.tool("world_query", {"names": ["SpinningCube"], "fields": ["/MaterialOverride/baseColor"]})
    cube = (found or {}).get("entities", [{}])[0].get("id")
    edited, _, err = c.tool("world_apply", {"commands": [
        {"op": "field.set", "id": cube, "path": "/MaterialOverride/baseColor", "value": [0.1, 0.65, 0.2]}]})
    check(cube is not None and not err and edited.get("applied") == 1 and edited.get("inverse"),
          "world_apply: the cube turns green, with its inverse")

    snap, _, err = c.tool("world_snapshot", {"name": "start"})
    check(not err and snap.get("name") == "start", "world_snapshot before recording")
    recording = OUT + "/flight.input.jsonl"
    rec, _, err = c.tool("input_record", {"path": recording})
    check(not err, "input_record starts")

    before, _, _ = c.tool("world_query", {"names": ["MainCamera"],
                                          "fields": ["/Transform/position", "/FlyController/moveSpeed"],
                                          "defaults": True})
    camera = (before or {}).get("entities", [{}])[0]
    p0 = camera.get("Transform", {}).get("position")
    speed = camera.get("FlyController", {}).get("moveSpeed")
    queued, _, err = c.tool("input_inject", {"events": [
        {"step": 0, "axis": "move_forward", "value": 1},
        {"step": 30, "axis": "move_up", "value": 1},
        {"step": 60, "axis": "move_forward", "value": None},
        {"step": 60, "axis": "move_up", "value": None}]})
    check(not err and queued.get("queued") == 4, "input_inject: a 61-step flight (forward, then up too)")
    stepped, _, err = c.tool("world_step", {"frames": 61, "watch": {"names": ["MainCamera"],
                                                                     "fields": ["/Transform/position"]}})
    p1 = camera_position(stepped)
    # At dt 1/60 the camera flies forward (-Z) for steps 0-59 and up for steps 30-59: the
    # releases at step 60 apply before that step simulates.
    moved = p0 is not None and p1 is not None and speed is not None and abs(p1[0] - p0[0]) < 1e-4 and \
        abs((p1[1] - p0[1]) - speed * 30 / 60) < 1e-3 and abs((p1[2] - p0[2]) + speed * 60 / 60) < 1e-3
    check(not err and moved, "world_step with watch: at %s m/s the camera flew %s -> %s" % (speed, p0, p1))

    first_png = OUT + "/first.png"
    shot1, image1, err = c.tool("render_capture", {"inline": True, "path": first_png})
    jpeg1 = base64.b64decode(image1["data"]) if image1 else b""
    check(not err and shot1.get("deterministic") is True and image1 and image1.get("mimeType") == "image/jpeg" and
          jpeg1[:2] == b"\xff\xd8" and (read_bytes(first_png) or b"")[:4] == b"\x89PNG",
          "render_capture: an inline JPEG (%d bytes) and a PNG file" % len(jpeg1))
    stats, _, err = c.tool("render_stats", {"frames": 10})
    passes = [p[0] for p in (stats or {}).get("passes", [])]
    check(not err and stats.get("gpuMs", 0) > 0 and "gbuffer" in passes and "lighting" in passes,
          "render_stats: %.3f ms of GPU a frame over %s frames, %d passes" %
          ((stats or {}).get("gpuMs", 0), (stats or {}).get("frames"), len(passes)))

    logs, _, err = c.tool("log_read", {"level": "info", "limit": 20})
    check(not err and isinstance(logs.get("lines"), list) and logs["lines"] and "next" in logs,
          "log_read: %d lines" % len((logs or {}).get("lines", [])))
    stopped, _, err = c.tool("input_record", {"stop": True})
    check(not err and stopped.get("steps") == 61 and stopped.get("events", 0) >= 4,
          "input_record stops: %s steps, %s events" % ((stopped or {}).get("steps"), (stopped or {}).get("events")))
    after, _, _ = c.tool("world_hash")

    restored, _, err = c.tool("world_restore", {"name": "start"})
    check(not err, "world_restore to the snapshot")
    replayed, _, err = c.tool("input_replay", {"path": recording})
    again, _, _ = c.tool("world_hash")
    check(not err and replayed.get("steps") == 61 and replayed.get("checkpoints", 0) >= 1,
          "input_replay: %s steps, %s checkpoints, every one matching" %
          ((replayed or {}).get("steps"), (replayed or {}).get("checkpoints")))
    check(after and again and after.get("hash") == again.get("hash") and after.get("frame") == again.get("frame"),
          "the replay reaches the recorded world hash %s" % (after or {}).get("hash"))

    second_png = OUT + "/second.png"
    shot2, image2, err = c.tool("render_capture", {"inline": True, "path": second_png})
    png1, png2 = read_bytes(first_png), read_bytes(second_png)
    check(not err and png1 and png1 == png2, "the capture after the replay equals the first, PNG byte for byte")
    check(image2 is not None and image1 is not None and image2["data"] == image1["data"],
          "and the inline JPEG too")

    code, err_text = c.close()
    live = [l for l in err_text.splitlines() if "live host:" in l]
    clean = bool(live) and "0 unrequested" in live[-1] and "validation 0 including teardown" in live[-1]
    reports = [l for l in err_text.splitlines() if l.startswith(("W validation:", "E validation:"))]
    check(code == 0 and clean and not reports,
          "closing stdin ends the session: exit 0, validation clean through teardown, no unasked readback")

    if failed == 0:
        shutil.rmtree(TMP, ignore_errors=True)
    print("Results: %d/%d passed" % (passed, passed + failed))
    print(json.dumps({"suite": "e2e_mcp", "passed": passed, "failed": failed, "failures": failures}))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
