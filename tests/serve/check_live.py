#!/usr/bin/env python3
"""
check_live -- `ohao serve --window`, the live host, end to end (sub-project 4, step 9).

  test 16  a client drives `ohao serve --window` (a hidden window, validation on): load,
           apply, step with watch, inject, input.state, captures (deterministic, post off
           and on, inline JPEG and PNG previews, a region), log.read; a lockstep capture
           equals a headless capture of the same world state bit for bit (post off and on);
           another size is E_COMMAND; capture while minimized is
           E_STATE; a disconnect in the middle of a long world.step exits 0; validation
           reports nothing through teardown and no frame read back unasked.
  test 17  ohao_game's scene in the window host: helmet_game.scene.json saves byte for byte
           as loaded, and its fly camera moves moveSpeed * dt * 60 along its front after 60
           steps of injected move_forward = 1. (The presentation gate is the CTest test
           ohao_game_presentation.)
  test 11  (2b) path-traced frames in a window: lockstep rt_offline and rt_realtime captures
           are traced and presented, then the window is deferred again; each equals a
           headless capture of the same state to the BVH floor (max_abs 1 on at most 0.05%
           of channels); a long capture reports progress, is cancelled with no answer, and
           the host serves on; `--mode rt_offline --spp 24` presents 24 accumulating frames,
           stops, and starts again after a change; a realtime `--mode rt_realtime` window's
           capture is its next presented frame, and another mode is E_COMMAND.
  sp6 14   walking the level: the play scene's player in a hidden window walks, turns by the
           mouse and jumps on injected input; player.info says where; the same walk headless
           reaches the same place, world.hash and picture from the player's eyes, bit for bit.
  sp7 18   a fight (the done demo): in a hidden window an agent shoots EnemyA down (combat.info
           for where, player.teleport to face, fire bursts), kills the player with
           combat.damage, waits out the round's restart, clears the room and wins; the same
           fight headless reaches the same world.hash, combat.info, events and HUD picture.
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
import math
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
        self.notes = []   # notifications seen while waiting (progress, logs)

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
            if isinstance(msg, dict) and "method" in msg and "id" not in msg:
                self.notes.append(msg)
            if isinstance(msg, dict) and "method" not in msg and msg.get("id") == rid:
                return msg

    def notify(self, method, params):
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        self.proc.stdin.write((json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

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
    # One frame: its passes' sum is at most its span (medians over several are not additive).
    stats = result(s.call("render.stats", {"frames": 1})) or {}
    names = [p[0] for p in stats.get("passes", [])]
    check(stats.get("frames") == 1 and stats.get("gpuMs", 0) > 0 and "gbuffer.late" in names and "lighting" in names
          and "present" in names and sum(p[1] for p in stats.get("passes", [])) <= stats.get("gpuMs", 0) * 1.05 + 0.01,
          "render.stats on the window host: per-pass medians (%s), their sum within the frame's %.3f ms"
          % (", ".join(names), stats.get("gpuMs", 0)))
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


def diff(a, b):
    """(max_abs, fraction of channels that differ) of two same-size RGBA images, or None."""
    try:
        (sa, pa), (sb, pb) = pixels(a), pixels(b)
    except OSError:
        return None
    if sa != sb or len(pa) != len(pb):
        return None
    worst, count = 0, 0
    for x, y in zip(pa, pb):
        if x != y:
            count += 1
            worst = max(worst, abs(x - y))
    return worst, count / max(1, len(pa))


def wait_stats(s, want, seconds=20):
    """render.stats {frames: 120} until `want(stats)` or the deadline; the last stats."""
    stats = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        stats = result(s.call("render.stats", {"frames": 120})) or {}
        if want(stats):
            break
        time.sleep(0.1)
    return stats


def test_path_tracer_window():
    print("test 11 (2b): path-traced frames in a window")
    out = out_dir("rt")
    size = (640, 360)
    s = Server([OHAO, "serve", "--window", "%dx%d" % size, "--hidden", "--out", out, "--seed", SEED,
                "assets/scenes/cornell_box.scene.json"], "rt", env=validation_env())
    _, _, _, _, hashed = drive(s)
    offline = result(s.call("render.capture", {"path": out + "/offline.png", "mode": "rt_offline", "spp": 16})) or {}
    check(offline.get("deterministic") is True and offline.get("frames") == 16,
          "a lockstep rt_offline capture: 16 sample passes, deterministic (%s ms)" % offline.get("ms"))
    stats = result(s.call("render.stats", {"frames": 1})) or {}
    names = [p[0] for p in stats.get("passes", [])]
    check("pt.trace" in names and "pt.present" in names,
          "its frames were traced and presented (passes %s)" % ", ".join(names))
    realtime = result(s.call("render.capture", {"path": out + "/realtime.png", "mode": "rt_realtime", "spp": 8})) or {}
    check(realtime.get("frames") == 8, "a lockstep rt_realtime capture: 8 frames")
    deferred = result(s.call("render.capture", {"inline": True})) or {}
    stats = result(s.call("render.stats", {"frames": 1})) or {}
    names = [p[0] for p in stats.get("passes", [])]
    check(deferred.get("mimeType") == "image/jpeg" and "lighting" in names and "present" in names,
          "after a path-traced capture the window is deferred again (its live mode)")

    # The same state headless: the same image, to the path tracer's BVH floor (1 LSB on a few
    # pixels, AGENTS.md: the driver builds a BVH differently per process).
    h = Server([OHAO, "serve", "--out", out_dir("rt_headless"), "--seed", SEED, "assets/scenes/cornell_box.scene.json"],
               "rt_headless", env=validation_env())
    _, _, _, _, hhash = drive(h)
    hout = out_dir("rt_headless")
    ho = h.call("render.capture", {"path": hout + "/offline.png", "mode": "rt_offline", "spp": 16,
                                   "width": size[0], "height": size[1]})
    hr = h.call("render.capture", {"path": hout + "/realtime.png", "mode": "rt_realtime", "spp": 8,
                                   "width": size[0], "height": size[1]})
    check(h.close() == 0 and result(ho) is not None and result(hr) is not None and
          (result(hhash) or {}).get("hash") == (result(hashed) or {}).get("hash"),
          "the headless host reaches the same world and captures")
    # The floor: the driver builds a window's BVHs and a headless host's differently, so a ray exactly
    # on a shared triangle edge may take either triangle. The realtime tracer's pixel moves by 1 LSB.
    # The reference (rt/path_tracer.slang) tests a path against the hit triangle's geometric normal,
    # so on an edge between two faces the path may go on otherwise: a few pixels, any amount.
    for mode, max_abs in (("offline", 255), ("realtime", 1)):
        d = diff(out + "/%s.png" % mode, hout + "/%s.png" % mode)
        check(d is not None and d[0] <= max_abs and d[1] <= 0.0005,
              "rt_%s: the window's lockstep capture equals the headless capture (max_abs %s, %.6f of channels)"
              % (mode, d[0] if d else "-", d[1] if d else 1.0))

    # A long capture cancelled mid-way: progress arrived, no answer, the host serves on.
    rid = s.send("render.capture", {"inline": True, "mode": "rt_offline", "spp": 60000,
                                    "_meta": {"progressToken": "rt-progress"}})
    time.sleep(1.5)
    s.notify("notifications/cancelled", {"requestId": rid})
    reply = s.wait(rid, timeout=5)
    progress = [n for n in s.notes if n.get("method") == "notifications/progress" and
                n.get("params", {}).get("progressToken") == "rt-progress"]
    check(reply is None, "a cancelled capture gets no answer")
    check(bool(progress) and progress[-1]["params"].get("total") == 60000,
          "it reported progress meanwhile (%d notifications, last %s)"
          % (len(progress), progress[-1]["params"] if progress else None))
    after = result(s.call("render.capture", {"inline": True, "mode": "rt_offline", "spp": 4})) or {}
    check(after.get("frames") == 4 and result(s.call("engine.info")) is not None,
          "after the cancel the host serves: another capture renders")
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")

    # A window presenting rt_offline (lockstep): its accumulation converges to --spp samples
    # after a change, a frame each, then it stops presenting until the next change.
    p = Server([OHAO, "serve", "--window", "%dx%d" % size, "--hidden", "--out", out_dir("rt_live"),
                "--mode", "rt_offline", "--spp", "24", "assets/scenes/cornell_box.scene.json"], "rt_live",
               env=validation_env())
    first = wait_stats(p, lambda st: st.get("frames", 0) >= 24)
    time.sleep(1.0)
    again = result(p.call("render.stats", {"frames": 120})) or {}
    names = [x[0] for x in first.get("passes", [])]
    check(first.get("frames", 0) >= 24 and "pt.present" in names and again.get("frame") == first.get("frame"),
          "rt_offline presents progressively: %s frames presented, then still (frame %s, %s)"
          % (first.get("frames"), first.get("frame"), again.get("frame")))
    p.call("world.step", {"frames": 1})
    later = wait_stats(p, lambda st: st.get("frame", 0) >= (again.get("frame") or 0) + 24)
    check(later.get("frame", 0) >= (again.get("frame") or 0) + 24, "a change restarts the accumulation: 24 more")
    code = p.close()
    check(code == 0 and teardown_clean(p.stderr()), "exit 0; validation clean through teardown")

    # A realtime window presenting rt_realtime: a capture is its next presented frame.
    r = Server([OHAO, "serve", "--window", "%dx%d" % size, "--hidden", "--realtime", "--out", out_dir("rt_rt"),
                "--mode", "rt_realtime", "assets/scenes/cornell_box.scene.json"], "rt_rt", env=validation_env())
    shot = result(r.call("render.capture", {"inline": True, "mode": "rt_realtime"})) or {}
    check(shot.get("deterministic") is False and shot.get("mimeType") == "image/jpeg" and shot.get("frames") == 1,
          "realtime rt_realtime: a capture is the next presented frame (deterministic false)")
    check(error_code(r.call("render.capture", {"inline": True})) == "E_COMMAND",
          "realtime: a deferred capture of an rt_realtime window is E_COMMAND (capture in lockstep)")
    code = r.close()
    check(code == 0 and teardown_clean(r.stderr()), "exit 0; validation clean through teardown")


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


def walk(s):
    """The play scene's player walks the same way in any host (sub-project 6, step 5): a quarter
    turn left by the mouse, west along the south of the room, a turn back, north up the lane at
    x -1, a jump. Returns (player.info, world.hash) after it."""
    for events, frames in [([{"mouse": [-900, 0]}], 1),
                           ([{"axis": "move_forward", "value": 1}], 35),
                           ([{"axis": "move_forward", "value": None}, {"mouse": [900, 0]}], 10),
                           ([{"axis": "move_forward", "value": 1}], 60),
                           ([{"action": "jump", "down": True}], 1),
                           ([{"action": "jump", "down": False}, {"axis": "move_forward", "value": None}], 60)]:
        s.call("input.inject", {"events": events})
        s.call("world.step", {"frames": frames})
    return result(s.call("player.info")) or {}, (result(s.call("world.hash")) or {}).get("hash")


def test_walk():
    print("sp6 test 14: walking the level in a hidden window")
    out = out_dir("walk")
    scene = "assets/scenes/blockout_play.scene.json"
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--out", out, "--seed", SEED, scene], "walk",
               env=validation_env())
    spawn = result(s.call("player.info")) or {}
    check(spawn.get("pos") == [1.5, 0, 2.2] and spawn.get("ground") is True,
          "the play scene's player stands at the spawn (player.info)")
    eyes = result(s.call("render.capture", {"inline": True})) or {}
    check(eyes.get("mimeType") == "image/jpeg", "the window shows the player's eyes")
    info, hashed = walk(s)
    pos = info.get("pos", [9, 9, 9])
    check(-1.3 < pos[0] < -0.6 and pos[2] < -1.0 and abs(pos[1]) < 0.01 and info.get("yawDeg") == 0 and
          info.get("footsteps", 0) >= 8,
          "a walk on injected input: west, north, a jump; on the floor at (%.3f, %.3f), %s footsteps" %
          (pos[0], pos[2], info.get("footsteps")))
    shot = result(s.call("render.capture", {"path": out + "/walk_window.png", "post": []})) or {}
    check(shot.get("deterministic") is True, "a lockstep capture after the walk")

    h = Server([OHAO, "serve", "--out", out, "--seed", SEED, scene], "walk_headless")
    info2, hashed2 = walk(h)
    check(hashed is not None and hashed == hashed2 and info2.get("pos") == info.get("pos"),
          "the same walk headless: the same place and world.hash (%s)" % hashed)
    shot2 = result(h.call("render.capture", {"path": out + "/walk_headless.png", "post": [],
                                              "width": shot.get("width", 960), "height": shot.get("height", 540)})) or {}
    same = False
    if shot2.get("path") and shot.get("path"):
        same = pixels(out + "/walk_window.png") == pixels(out + "/walk_headless.png")
    check(same, "and the same picture from the player's eyes, bit for bit")
    h.close()
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")


def test_render_stats_latency():
    print("sp9 tests 19, 20: latency and pacing in render.stats (a realtime window)")
    scene = "assets/scenes/blockout_play.scene.json"
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--realtime", "--out", out_dir("latency"),
                "--seed", SEED, scene], "latency", env=validation_env())
    # Mouse movements injected one at a time while the window runs: each is applied by the next
    # step and measured from its injection to the present of the first frame showing it in full.
    for i in range(60):
        s.call("input.inject", {"events": [{"mouse": [4 if i % 2 else -4, 0]}]})
        time.sleep(0.02)
    time.sleep(0.1)   # the last movements reach a frame (two steps)
    stats = result(s.call("render.stats", {"frames": 120})) or {}
    lat, pac = stats.get("latency") or {}, stats.get("pacing") or {}
    stages = lat.get("stages") or {}
    check(lat.get("frames", 0) > 0 and lat.get("source") in ("present_wait", "present_call") and
          0 < lat.get("medianMs", 0) <= lat.get("p95Ms", 0) < 1000,
          "a realtime window reports latency after injected look: %s frames, median %s ms, p95 %s ms (%s)" %
          (lat.get("frames"), lat.get("medianMs"), lat.get("p95Ms"), lat.get("source")))
    check(len(stages) == 4 and all(v >= 0 for v in stages.values()),
          "its stages, poll to consume, view, submit and present, none negative: %s" % stages)
    steps = pac.get("steps") or {}
    frame_ms = pac.get("frameMs") or {}
    check(0 < frame_ms.get("median", 0) <= frame_ms.get("p95", 0) <= frame_ms.get("p99", 0) and
          sorted(steps.keys()) == ["0", "1", "2", "3+"] and sum(steps.values()) > 0,
          "and pacing: frame time %s, steps per frame %s" % (frame_ms, steps))
    s.call("time.set", {"mode": "lockstep"})
    idle = result(s.call("render.stats", {"frames": 120})) or {}
    check(idle.get("frames", 0) > 0 and "latency" not in idle and "pacing" not in idle,
          "a lockstep window reports neither (its GPU timings still)")
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")


def test_render_stats_audio():
    print("sp11 test 29: render.stats audio and audio.info's device half (a realtime window with --sound)")
    scene = "assets/scenes/blockout_play.scene.json"
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--realtime", "--sound", "--out", out_dir("audio"),
                "--seed", SEED, scene], "audio", env=validation_env())
    s.call("player.teleport", {"pos": [1.5, 0, 2.2], "yawDeg": 43.6})   # toward EnemyA
    s.call("input.inject", {"events": [{"action": "fire", "down": True}, {"step": 30, "action": "fire", "down": False}]})
    time.sleep(1.5)   # the burst, its impacts and the enemies' replies play
    stats = result(s.call("render.stats", {"frames": 60})) or {}
    audio = stats.get("audio") or {}
    check(set(audio.keys()) == {"mixMs", "voices", "late"} and 0 < audio.get("mixMs", 0) < 10,
          "render.stats carries the mix's cost: %s" % audio)
    info = result(s.call("audio.info", {"max": 0})) or {}
    check(info.get("device") == "default" and info.get("sampleRate", 0) >= 44100 and "mixMs" in info and "late" in info and
          "device" in (info.get("voices") or {}),
          "audio.info adds the device's half: rate %s, late %s, mixMs %s" %
          (info.get("sampleRate"), info.get("late"), info.get("mixMs")))
    muted = result(s.call("audio.mix", {"bus": "weapons", "mute": True})) or {}
    check(any(b.get("name") == "weapons" and b.get("mix") == {"mute": True} for b in muted.get("buses", [])),
          "audio.mix mutes the window's weapons bus")
    s.call("audio.mix", {"clear": True})
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown with the device playing")
    quiet = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--out", out_dir("audio_off"), "--seed", SEED, scene],
                   "audio_off", env=validation_env())
    quiet.call("render.capture", {"inline": True})
    off = result(quiet.call("render.stats")) or {}
    check(off.get("frames", 0) > 0 and "audio" not in off and (result(quiet.call("audio.info")) or {}).get("device") == "none",
          "a window without --sound: no audio in render.stats, device none")
    check(quiet.close() == 0, "exit 0")


def test_view_turns_next_frame():
    print("sp9 test 19: the look lead (a realtime window shows a movement on the next frame)")
    scene = "assets/scenes/blockout_play.scene.json"
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--realtime", "--out", out_dir("lead"),
                "--seed", SEED, scene], "lead", env=validation_env())
    for i in range(60):
        s.call("input.inject", {"events": [{"mouse": [6 if i % 2 else -6, 0]}]})
        time.sleep(0.02)
    time.sleep(0.1)
    lat = (result(s.call("render.stats", {"frames": 120})) or {}).get("latency") or {}
    stages = lat.get("stages") or {}
    check(lat.get("frames", 0) > 0 and stages.get("pollToViewMs", 99) < 4.0,
          "a movement is in the very next frame's view: poll to view %s ms (median), latency %s ms" %
          (stages.get("pollToViewMs"), lat.get("medianMs")))
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")


def test_lead_dropped_in_lockstep():
    print("sp9 test 19: no lead in lockstep (a capture shows the step's view, whatever is injected)")
    scene = "assets/scenes/blockout_play.scene.json"
    out = out_dir("lead_lockstep")
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--realtime", "--out", out, "--seed", SEED, scene],
               "lead_lockstep", env=validation_env())
    s.call("input.inject", {"events": [{"mouse": [40, 0]}]})
    time.sleep(0.2)
    s.call("time.set", {"mode": "lockstep"})
    a = result(s.call("render.capture", {"path": out + "/a.png", "post": []})) or {}
    s.call("input.inject", {"events": [{"mouse": [300, 0]}]})   # 30 degrees, due at the next step
    b = result(s.call("render.capture", {"path": out + "/b.png", "post": []})) or {}
    s.call("world.step", {"frames": 1})
    c = result(s.call("render.capture", {"path": out + "/c.png", "post": []})) or {}
    same = bool(a.get("path") and b.get("path")) and pixels(out + "/a.png") == pixels(out + "/b.png")
    turned = bool(c.get("path")) and pixels(out + "/a.png") != pixels(out + "/c.png")
    check(same, "an injected movement not yet stepped changes no pixel of a lockstep capture")
    check(turned, "the step that applies it turns the view")
    code = s.close()
    check(code == 0 and teardown_clean(s.stderr()), "exit 0; validation clean through teardown, no unasked readback")


PLAYER, ENEMY_A, ENEMY_B = "5c0b1a7e00000001", "5c0b1a7e00000010", "5c0b1a7e00000020"


def combatant(info, eid):
    return next((c for c in (info or {}).get("combatants", []) if c.get("entity") == eid), {})


def shoot_down(call, eid, tries=20):
    """Faces the enemy (player.teleport where the player stands) and fires bursts of three rounds
    until combat.info says it is dead. Returns the bursts it took, None if it still lives."""
    for n in range(tries):
        info = call("combat.info", {})
        me, foe = combatant(info, PLAYER), combatant(info, eid)
        if foe.get("dead"):
            return n
        if me.get("dead") or not me.get("pos") or not foe.get("pos"):
            return None
        p, q = me["pos"], foe["pos"]
        yaw = round(math.degrees(math.atan2(-(q[0] - p[0]), -(q[2] - p[2]))), 2)
        call("player.teleport", {"pos": p, "yawDeg": yaw})
        call("input.inject", {"events": [{"action": "fire", "down": True}]})
        call("world.step", {"frames": 13})   # rounds on steps 1, 7 and 13 (10 Hz)
        call("input.inject", {"events": [{"action": "fire", "down": False}]})
        call("world.step", {"frames": 1})
    return None


def fight(call):
    """The done demo's fight (sub-project 7, step 5), the same in any host: the round starts; EnemyA
    shot down; the player killed; the round restarts 3 s later; both enemies shot down: won. `call`
    is (method, params) -> result or None. Returns what each stage left."""
    out = {}
    call("world.step", {"frames": 1})
    out["killA"] = shoot_down(call, ENEMY_A)
    call("world.step", {"frames": 50})   # sub-project 10: its death clip plays out
    out["animA"] = call("animation.info", {"entity": ENEMY_A, "since": 0, "max": 64}) or {}
    out["hurt"] = combatant(call("combat.info", {}), PLAYER).get("health")
    out["killed"] = call("combat.damage", {"entity": PLAYER, "amount": 1000, "source": ENEMY_B}) or {}
    call("world.step", {"frames": 1})
    out["dead"] = (call("combat.info", {}) or {}).get("round", {})
    call("world.step", {"frames": 180})
    out["restarted"] = call("combat.info", {}) or {}
    out["killA2"] = shoot_down(call, ENEMY_A)
    out["killB"] = shoot_down(call, ENEMY_B)
    call("world.step", {"frames": 1})
    out["final"] = call("combat.info", {}) or {}
    events = call("combat.events", {"since": 0, "max": 64}) or {}
    out["kinds"] = [e.get("kind") for e in events.get("events", [])]
    out["hash"] = (call("world.hash", {}) or {}).get("hash")
    return out


def check_fight(f, where):
    check(f["killA"] is not None and isinstance(f["hurt"], (int, float)) and f["hurt"] <= 100,
          "%s: EnemyA shot down in %s bursts; the player at %s hp" % (where, f["killA"], f["hurt"]))
    body = (f["animA"].get("animators") or [{}])[0].get("layers", [{}])[0]
    falls = [e for e in f["animA"].get("events", []) if e.get("entity") == ENEMY_A and e.get("name") == "body_fall"]
    kicks = [e for e in f["animA"].get("events", []) if e.get("entity") == ENEMY_A]
    check(body.get("state") == "dead" and len(falls) == 1,
          "%s: animation.info: EnemyA's death clip played to its end (%s), body_fall once (%d of its events)" %
          (where, body.get("state"), len(kicks)))
    check(f["killed"].get("dead") is True and f["dead"].get("phase") == "dead" and f["dead"].get("restartIn", 0) > 2.9,
          "%s: combat.damage kills the player; the round is dead, restarting in %s s" % (where, f["dead"].get("restartIn")))
    r = f["restarted"]
    check(r.get("round", {}).get("restarts") == 1 and r.get("round", {}).get("phase") == "playing" and
          all(c.get("health") == 100 and not c.get("dead") for c in r.get("combatants", [])),
          "%s: 3 s later the round restarted: everyone back at 100" % where)
    won = f["final"].get("round", {})
    check(f["killA2"] is not None and f["killB"] is not None and won.get("phase") == "won" and "win" in f["kinds"],
          "%s: the room cleared (EnemyA in %s bursts, EnemyB in %s): won, a win event" % (where, f["killA2"], f["killB"]))


def audio_trace(call):
    """Every audio schedule command the host logged (OHAO_AUDIO_TRACE=1: one `audio` line per command,
    every field but seq), oldest first."""
    out, since = [], 0
    while True:
        page = call("log.read", {"since": since, "level": "info", "channel": "audio", "limit": 1000}) or {}
        lines = page.get("lines", [])
        out += [l["msg"][len("schedule: "):] for l in lines if l.get("msg", "").startswith("schedule: ")]
        if not lines or page.get("next", since) == since:
            return out, page.get("dropped", 0)
        since = page["next"]


def traced_env(validation=False):
    env = validation_env() if validation else dict(os.environ)
    env["OHAO_AUDIO_TRACE"] = "1"
    return env


def test_fight():
    print("sp7 test 18: a fight in a hidden window, and the same fight headless")
    out = out_dir("fight")
    scene = "assets/scenes/blockout_play.scene.json"
    s = Server([OHAO, "serve", "--window", "960x540", "--hidden", "--out", out, "--seed", SEED, scene], "fight",
               env=traced_env(validation=True))
    f = fight(lambda m, p: result(s.call(m, p)))
    check_fight(f, "window")
    shot = result(s.call("render.capture", {"path": out + "/fight_window.png", "post": [], "hud": True})) or {}
    check(shot.get("deterministic") is True, "a lockstep capture of the won round with its HUD")
    h = Server([OHAO, "serve", "--out", out, "--seed", SEED, scene], "fight_headless", env=traced_env())
    g = fight(lambda m, p: result(h.call(m, p)))
    check(f["hash"] is not None and f["hash"] == g["hash"] and f["final"] == g["final"] and f["kinds"] == g["kinds"] and
          f["animA"] == g["animA"],
          "the same fight headless: the same world.hash (%s), combat.info, events and animation.info" % f["hash"])
    # sub-project 11 test 28 (t28_window_equals_headless): the same voice schedule in both hosts.
    wa, wdropped = audio_trace(lambda m, p: result(s.call(m, p)))
    ha, hdropped = audio_trace(lambda m, p: result(h.call(m, p)))
    plays = sum(1 for l in wa if l.startswith("play"))
    with open(os.path.join(TMP, "fight_schedule_window.txt"), "w", encoding="utf-8") as fw:
        fw.write("\n".join(wa) + "\n")
    with open(os.path.join(TMP, "fight_schedule_headless.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(ha) + "\n")
    check(plays > 20 and not wdropped and not hdropped and wa == ha,
          "t28_window_equals_headless: the same audio schedule in the window and headless (%d commands, %d plays)" %
          (len(wa), plays))
    shot2 = result(h.call("render.capture", {"path": out + "/fight_headless.png", "post": [], "hud": True,
                                              "width": shot.get("width", 960), "height": shot.get("height", 540)})) or {}
    same = bool(shot2.get("path") and shot.get("path")) and pixels(out + "/fight_window.png") == pixels(out + "/fight_headless.png")
    check(same, "and the same picture with the HUD, bit for bit")
    # sub-project 12 test 32 (t32_window_equals_headless): a grenade thrown in both hosts and stepped to
    # its boom; the lockstep captures, the boom and its smoke drawn, are the same picture.
    effects = []
    for host, tag in ((s, "window"), (h, "headless")):
        result(host.call("input.inject", {"events": [{"action": "grenade", "down": True},
                                                     {"step": 1, "action": "grenade", "down": False}]}))
        result(host.call("world.step", {"frames": 152}))
        info = result(host.call("vfx.info", {"since": 0, "max": 16})) or {}
        effects.append(info.get("live", {}).get("effect", 0))
        result(host.call("render.capture", {"path": out + "/boom_%s.png" % tag, "post": ["taa", "bloom"],
                                            "width": shot.get("width", 960), "height": shot.get("height", 540)}))
    check(effects[0] > 0 and effects[0] == effects[1] and
          pixels(out + "/boom_window.png") == pixels(out + "/boom_headless.png"),
          "t32_window_equals_headless: the boom and its smoke (%s live effects) drawn the same in the window "
          "and headless, bit for bit" % effects[0])
    h.close()
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
    print("check_live (sub-project 4, tests 16 and 17; 2b, test 11)")
    test_live()
    test_path_tracer_window()
    test_game_scene()
    test_walk()
    test_fight()
    test_render_stats_latency()
    test_render_stats_audio()
    test_view_turns_next_frame()
    test_lead_dropped_in_lockstep()
    if os.name == "nt":
        test_window_pipe()
    print("Results: %d/%d passed" % (passed, passed + failed))
    print(json.dumps({"suite": "check_live", "passed": passed, "failed": failed, "failures": failures}))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
