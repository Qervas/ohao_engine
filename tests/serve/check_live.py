#!/usr/bin/env python3
"""
check_live -- `ohao serve --window`, the live host, end to end (sub-project 4, step 9).

  test 16  a client drives `ohao serve --window` (a hidden window, validation on): load,
           apply, step with watch, inject, input.state, captures (deterministic, post off
           and on, inline JPEG and PNG previews, a region), log.read; a lockstep capture
           equals a headless capture of the same world state bit for bit (post off and on);
           another size or a path-traced mode is E_COMMAND; capture while minimized is
           E_STATE; a disconnect in the middle of a long world.step exits 0; validation
           reports nothing through teardown and no frame read back unasked.
  test 17  ohao_game's scene in the window host: helmet_game.scene.json saves byte for byte
           as loaded, and its fly camera moves moveSpeed * dt * 60 along its front after 60
           steps of injected move_forward = 1. (The presentation gate is the CTest test
           ohao_game_presentation.)
  test 15  (the window host's part, Windows) `ohao serve --window --pipe NAME`: a step and
           an inline capture over the pipe; the client's disconnect ends the session with
           exit 0 and a clean teardown.

Usage (from the repo root):  python tests/serve/check_live.py
The last stdout line is the shared JSON summary. Scratch output: build/live_tmp/<pid>/.
Needs a GPU and Pillow.
"""
import base64
import ctypes
import io
import json
import os
import queue
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + EXE)
TMP = os.path.join(ROOT, "build", "live_tmp", str(os.getpid()))
SPHERE = "499421b6ef3e75a9"   # MetalSphere in assets/scenes/cornell_box.scene.json
SEED = "0000000000000001"

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


class Server:
    """A server process: lines in on stdin, replies out on stdout, stderr to a file."""

    def __init__(self, argv, name, env=None):
        os.makedirs(TMP, exist_ok=True)
        self.err_path = os.path.join(TMP, name + ".stderr.txt")
        self.err = open(self.err_path, "wb")
        self.proc = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err,
                                     env=env)
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.next_id = 0

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def send(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        return self.next_id

    def wait(self, rid, timeout=120):
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                return None
            if line is None:
                return None
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if isinstance(msg, dict) and "method" not in msg and msg.get("id") == rid:
                return msg

    def call(self, method, params=None):
        return self.wait(self.send(method, params))

    def close(self, timeout=60):
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
        return code

    def stderr(self):
        with open(self.err_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def result(reply):
    return reply.get("result") if isinstance(reply, dict) else None


def error_code(reply):
    if not isinstance(reply, dict) or "error" not in reply:
        return None
    data = reply["error"].get("data")
    return data.get("code") if isinstance(data, dict) else None


def out_dir(name):
    return os.path.relpath(os.path.join(TMP, name), ROOT).replace(os.sep, "/")


def pixels(path):
    from PIL import Image
    with Image.open(os.path.join(ROOT, path)) as im:
        return im.size, im.convert("RGBA").tobytes()


def validation_env():
    env = dict(os.environ)
    env["OHAO_VALIDATION"] = "1"
    return env


def teardown_clean(text):
    """The live host's last line says 0 unrequested readbacks and 0 validation messages."""
    lines = [l for l in text.splitlines() if "live host:" in l]
    return bool(lines) and "0 unrequested" in lines[-1] and "validation 0 including teardown" in lines[-1]


def minimize(pid, title):
    """Minimizes the process's window by its title (Windows). False if it was not found."""
    if os.name != "nt":
        return False
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            if buf.value == title:
                found.append(hwnd)
        return True

    user32.EnumWindows(proto(visit), 0)
    for hwnd in found:
        user32.ShowWindow(hwnd, 6)   # SW_MINIMIZE
    return bool(found)


# ---- the world state both hosts reach ----------------------------------------------------------

def drive(s):
    """Edits and steps the Cornell world the same way in any host; returns (watch, hash)."""
    applied = s.call("world.apply", {"commands": [{"op": "field.set", "id": SPHERE, "path": "/Transform/position",
                                                   "value": [0, -1, 2]}]})
    stepped = s.call("world.step", {"frames": 3, "watch": {"names": ["MetalSphere"], "fields": ["/Transform/position"]}})
    injected = s.call("input.inject", {"events": [{"step": 0, "action": "jump", "down": True}]})
    s.call("world.step", {"frames": 1})
    state = s.call("input.state")
    hashed = s.call("world.hash")
    return applied, stepped, injected, state, hashed


def test_live():
    print("test 16: the live host")
    out = out_dir("live")
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--out", out, "--seed", SEED,
                "assets/scenes/cornell_box.scene.json"], "live", env=validation_env())
    info = result(s.call("engine.info"))
    check(info is not None and info.get("host") == "window" and info.get("time", {}).get("mode") == "lockstep",
          "engine.info: a window host, lockstep by default")
    applied, stepped, injected, state, hashed = drive(s)
    watch = (result(stepped) or {}).get("watch", {}).get("entities", [])
    check(result(applied) is not None and result(stepped) is not None and result(stepped).get("frame") == 3 and
          watch and watch[0].get("Transform", {}).get("position") == [0, -1, 2],
          "world.apply and world.step with watch answer in the window host")
    jump = (result(state) or {}).get("actions", {}).get("jump", {})
    check(result(injected) is not None and jump.get("held") is True, "input.inject reaches the next step (input.state)")

    plain = s.call("render.capture", {"path": out + "/live_plain.png", "post": []})
    post = s.call("render.capture", {"path": out + "/live_post.png", "post": ["taa", "ssao", "bloom"], "exposure": 1})
    rp = result(plain) or {}
    check(rp.get("deterministic") is True and rp.get("frames") == 1 and rp.get("width") and rp.get("height"),
          "a lockstep capture is deterministic: 1 deferred frame from a reset without TAA (%sx%s, %s ms)" %
          (rp.get("width"), rp.get("height"), rp.get("ms")))
    check((result(post) or {}).get("frames") == 10, "with TAA on, 10 frames")
    width, height = rp.get("width", 960), rp.get("height", 540)

    preview = s.call("render.capture", {"inline": True})
    rv = result(preview) or {}
    jpeg = base64.b64decode(rv.get("image", "")) if rv.get("image") else b""
    long_side = max(rv.get("imageSize", [0, 0]))
    check(rv.get("mimeType") == "image/jpeg" and jpeg[:2] == b"\xff\xd8" and long_side == min(768, max(width, height)),
          "an inline preview: a JPEG, long side at most 768 (%s, %d bytes)" % (rv.get("imageSize"), len(jpeg)))
    zoom = result(s.call("render.capture", {"inline": True, "format": "png", "region": [100, 100, 200, 100]})) or {}
    png = base64.b64decode(zoom.get("image", "")) if zoom.get("image") else b""
    check(zoom.get("mimeType") == "image/png" and png[:4] == b"\x89PNG" and zoom.get("imageSize") == [200, 100],
          "a region zooms: a 200x100 PNG of the frame")
    check(error_code(s.call("render.capture", {"path": out + "/x.png", "width": 640, "height": 480})) == "E_COMMAND",
          "another size than the window's is E_COMMAND")
    check(error_code(s.call("render.capture", {"path": out + "/x.png", "mode": "rt_offline"})) == "E_COMMAND",
          "a path-traced capture in a window host is E_COMMAND")
    logs = result(s.call("log.read", {"level": "info", "limit": 50})) or {}
    check(isinstance(logs.get("lines"), list) and any("swapchain" in l.get("msg", "") for l in logs["lines"]),
          "log.read returns the engine's log (%d lines)" % len(logs.get("lines", [])))

    # The same state in a headless host: the same pixels.
    h = Server([OHAO, "serve", "--out", out_dir("headless"), "--seed", SEED, "assets/scenes/cornell_box.scene.json"],
               "headless", env=validation_env())
    _, _, _, _, hhash = drive(h)
    check(result(hhash) is not None and result(hhash).get("hash") == (result(hashed) or {}).get("hash"),
          "the headless host reaches the same world hash")
    hout = out_dir("headless")
    hp = h.call("render.capture", {"path": hout + "/plain.png", "width": width, "height": height, "post": []})
    hq = h.call("render.capture", {"path": hout + "/post.png", "width": width, "height": height,
                                   "post": ["taa", "ssao", "bloom"], "exposure": 1})
    check(h.close() == 0 and result(hp) is not None and result(hq) is not None, "the headless captures render")
    for tag, a, b in (("off", out + "/live_plain.png", hout + "/plain.png"),
                      ("on", out + "/live_post.png", hout + "/post.png")):
        try:
            same = pixels(a) == pixels(b)
        except OSError:
            same = False
        check(same, "post effects %s: the window's lockstep capture equals the headless capture bit for bit" % tag)

    # Minimized: no frame to capture.
    if minimize(s.proc.pid, "OHAO serve"):
        time.sleep(0.5)
        check(error_code(s.call("render.capture", {"inline": True})) == "E_STATE", "capture while minimized is E_STATE")
    else:
        check(False, "capture while minimized is E_STATE (the window could not be found to minimize)")

    # A disconnect in the middle of a long step: exit 0, validation clean through teardown.
    s.send("world.step", {"frames": 1000000})
    time.sleep(0.5)
    started = time.time()
    code = s.close(timeout=60)
    err = s.stderr()
    check(code == 0, "closing stdin during a long world.step exits 0 (%.1f s)" % (time.time() - started))
    check(teardown_clean(err), "validation reported nothing through teardown and no frame was read back unasked")
    reports = [l for l in err.splitlines() if l.startswith(("W validation:", "E validation:"))]
    check(not reports, "no validation message on stderr" + (": " + reports[0] if reports else ""))


def test_game_scene():
    print("test 17: ohao_game's scene in the window host")
    out = out_dir("game")
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--out", out,
                "assets/scenes/helmet_game.scene.json"], "game", env=validation_env())
    saved = result(s.call("scene.save", {"path": out + "/helmet_game.scene.json"}))
    try:
        with open(os.path.join(ROOT, "assets", "scenes", "helmet_game.scene.json"), "rb") as f:
            original = f.read()
        with open(os.path.join(ROOT, out, "helmet_game.scene.json"), "rb") as f:
            again = f.read()
    except OSError:
        original, again = b"", b"x"
    check(saved is not None and original == again, "helmet_game.scene.json saves byte for byte as loaded")

    before = (result(s.call("world.query", {"names": ["MainCamera"], "fields": ["/Transform/position"]})) or {})
    p0 = before.get("entities", [{}])[0].get("Transform", {}).get("position")
    s.call("input.inject", {"events": [{"step": 0, "axis": "move_forward", "value": 1},
                                       {"step": 60, "axis": "move_forward", "value": None}]})
    stepped = result(s.call("world.step", {"frames": 60, "watch": {"names": ["MainCamera"],
                                                                    "fields": ["/Transform/position"]}})) or {}
    p1 = stepped.get("watch", {}).get("entities", [{}])[0].get("Transform", {}).get("position")
    moved = p0 is not None and p1 is not None and abs(p1[0] - p0[0]) < 1e-5 and abs(p1[1] - p0[1]) < 1e-5 and \
        abs((p1[2] - p0[2]) + 3.0) < 1e-4
    check(moved, "60 steps of move_forward = 1 fly the camera 3 m (moveSpeed * dt * 60) along -Z: %s -> %s" % (p0, p1))
    shot = result(s.call("render.capture", {"inline": True, "post": ["taa", "ssao", "bloom"], "exposure": 1})) or {}
    check(shot.get("mimeType") == "image/jpeg", "the game scene captures")
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")


def test_window_pipe():
    print("test 15: the window host over the named pipe")
    out = out_dir("pipe")
    name = "check-live-%d" % os.getpid()
    path = "\\\\.\\pipe\\ohao-" + name
    s = Server([OHAO, "serve", "--window", "640x360", "--hidden", "--pipe", name, "--out", out,
                "assets/scenes/cornell_box.scene.json"], "pipe", env=validation_env())
    f = None
    for _ in range(1500):   # the window and the renderer come up first
        try:
            f = open(path, "r+b", buffering=0)
            break
        except OSError:
            time.sleep(0.02)
    check(f is not None, "a client connects to the window host's pipe")
    if f is None:
        s.close()
        return
    pending = b""

    def call(rid, method, params):
        nonlocal pending
        f.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params},
                           separators=(",", ":")).encode() + b"\n")
        while True:
            while b"\n" not in pending:
                chunk = f.read(1 << 20)
                if not chunk:
                    return None
                pending += chunk
            line, pending = pending.split(b"\n", 1)
            msg = json.loads(line)
            if "method" not in msg and msg.get("id") == rid:
                return msg

    stepped = result(call(1, "world.step", {"frames": 2})) or {}
    shot = result(call(2, "render.capture", {"inline": True})) or {}
    check(stepped.get("frame") == 2 and shot.get("mimeType") == "image/jpeg" and shot.get("deterministic") is True,
          "world.step and an inline capture over the pipe")
    f.close()
    try:
        code = s.proc.wait(timeout=60)   # stdin is still open: the pipe's end ends the session
    except subprocess.TimeoutExpired:
        code = "timeout"
    s.close()
    check(code == 0 and teardown_clean(s.stderr()),
          "the client's disconnect closes the window host: exit 0, validation clean, no unasked readback")


def main():
    if not os.path.exists(OHAO):
        print("ohao is not built: " + OHAO)
        return 1
    print("check_live (sub-project 4, tests 16 and 17)")
    test_live()
    test_game_scene()
    if os.name == "nt":
        test_window_pipe()
    print("Results: %d/%d passed" % (passed, passed + failed))
    print(json.dumps({"suite": "check_live", "passed": passed, "failed": failed, "failures": failures}))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
