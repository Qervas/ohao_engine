#!/usr/bin/env python3
"""replay_test -- requirement 4 (Frank, 2026-11-08): gameplay replays exactly.

One headless `ohao serve` (no window, no GPU) plays the play scene: a walk, a turn, rounds fired at
an enemy, a reload, a grenade thrown and left to explode. Every command is recorded. Then:

  1. replay: restore the snapshot taken midway (a grenade in flight, a strafe and the aim held: state the
     components do not hold, step 3b) and re-issue the commands after it: the world reaches the same
     world.hash, at every checkpoint;
  2. fresh: a new `ohao serve` process loads the scene and re-issues every command: the same hashes from
     the start. Both loads pass the same `seed`: entity ids are drawn from std::random_device otherwise
     (ohao/ecs/world.hpp), and world.hash includes the id generator's state.

world.hash covers every saved component (transforms, health, weapons, AI, animators), every system's
registered state (physics' simulation, combat's grenades and round, the audio schedule, vfx) and the input,
so physics, combat, the enemy AI and animation must all replay bit for bit. The test also checks that the
fight happened (rounds fired, a hit, the grenade's explosion, the player moved, the world changed),
so it cannot pass on a world that stood still.

  python tests/replay/replay_test.py      # exit 0 pass, 1 fail; last line the JSON summary
Standard library only.
"""
import json
import math
import os
import queue
import shutil
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""
OHAO = os.path.join(ROOT, "build", "Release", "ohao" + EXE)
TMP = os.path.join(ROOT, "build", "replay_tmp", str(os.getpid()))
SCENE = "assets/scenes/blockout_play.scene.json"
SEED = "0000000020261108"   # entity ids from a fixed seed (16 hex digits), else from std::random_device

failures = []
passed = 0


def check(cond, what):
    global passed
    if cond:
        passed += 1
        print("  ok    " + what)
    else:
        failures.append(what)
        print("  FAIL  " + what)


class Serve:
    """`ohao serve` (headless) over stdio, MCP: one JSON-RPC message per line each way."""

    def __init__(self):
        os.makedirs(TMP, exist_ok=True)
        self.err = open(os.path.join(TMP, "serve.stderr.txt"), "wb")
        self.proc = subprocess.Popen([OHAO, "serve", "--out", os.path.relpath(TMP, ROOT)], cwd=ROOT,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err)
        self.lines = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._pump, daemon=True).start()
        init = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                           "clientInfo": {"name": "replay_test", "version": "1"}})
        if init is None or "result" not in init:
            raise RuntimeError("ohao serve did not initialize")
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def send(self, msg):
        self.proc.stdin.write((json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method, params, timeout=120):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                raise RuntimeError("no reply to %s within %d s" % (method, timeout))
            if line is None:
                raise RuntimeError("ohao serve exited during %s" % method)
            reply = json.loads(line)
            if "method" not in reply and reply.get("id") == rid:
                return reply

    def tool(self, name, arguments=None):
        reply = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        result = reply.get("result", {})
        text = next((c["text"] for c in result.get("content", []) if c.get("type") == "text"), "null")
        if result.get("isError") or "error" in reply:
            raise RuntimeError("%s %s: %s" % (name, json.dumps(arguments), text or reply.get("error")))
        return json.loads(text)

    def close(self):
        try:
            self.proc.stdin.close()
            code = self.proc.wait(timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()
            code = "killed"
        self.err.close()
        return code


def hash_of(s):
    return s.tool("world_hash")["hash"]


class Recorder:
    """Issues the play's commands and records them, with world.hash at each checkpoint."""

    def __init__(self, s):
        self.s, self.commands, self.hashes = s, [], []

    def do(self, name, args):
        self.commands.append((name, args))
        return self.s.tool(name, args)

    def checkpoint(self):
        self.hashes.append(hash_of(self.s))
        self.commands.append(("#checkpoint", None))


def replay(s, commands):
    """Re-issues recorded commands; returns world.hash at each checkpoint."""
    hashes = []
    for name, args in commands:
        if name == "#checkpoint":
            hashes.append(hash_of(s))
        else:
            s.tool(name, args)
    return hashes


def step(r, frames, events=None):
    if events:
        r.do("input_inject", {"events": events})
    r.do("world_step", {"frames": frames})


def first_half(r):
    """A walk: a quarter turn by the mouse, forward, a turn back, forward, a jump."""
    step(r, 1, [{"mouse": [-900, 0]}])
    step(r, 35, [{"axis": "move_forward", "value": 1}])
    step(r, 10, [{"axis": "move_forward", "value": None}, {"mouse": [900, 0]}])
    step(r, 40, [{"axis": "move_forward", "value": 1}])
    step(r, 1, [{"action": "jump", "down": True}])
    step(r, 30, [{"action": "jump", "down": False}, {"axis": "move_forward", "value": None}])


def fight(r, s):
    """The fight: face the nearest living enemy, three bursts, a reload, a grenade thrown, then a strafe with
    the aim held while it flies. The facing is computed once, here, and recorded as a teleport, so a replay
    issues the same command. The player is given 100,000 hit points first (a recorded edit): the enemies
    fire back, and a dead player's round restarts to its start 3 s later, which would undo the walk."""
    info = s.tool("combat_info")
    me = next(c for c in info["combatants"] if c.get("player"))
    r.do("world_apply", {"commands": [{"op": "component.set", "id": me["entity"], "type": "Health",
                                       "value": {"max": 100000, "current": 100000}}]})
    foes = [c for c in info["combatants"] if not c.get("player") and not c.get("dead")]
    foe = min(foes, key=lambda c: math.dist(c["pos"], me["pos"]))
    p, q = me["pos"], foe["pos"]
    yaw = round(math.degrees(math.atan2(-(q[0] - p[0]), -(q[2] - p[2]))), 2)
    r.do("player_teleport", {"pos": p, "yawDeg": yaw})
    for _ in range(3):
        step(r, 13, [{"action": "fire", "down": True}])   # rounds on steps 1, 7 and 13 (10 Hz)
        step(r, 1, [{"action": "fire", "down": False}])
    r.checkpoint()
    step(r, 1, [{"action": "reload", "down": True}])
    step(r, 100, [{"action": "reload", "down": False}])
    r.checkpoint()
    step(r, 1, [{"action": "grenade", "down": True}])
    step(r, 30, [{"action": "grenade", "down": False}, {"axis": "move_right", "value": 1},
                 {"action": "aim", "down": True}])
    r.checkpoint()


def after_throw(r):
    """The grenade lands and explodes (150 steps after the throw); the strafe and the aim let go on the way."""
    step(r, 20, [])
    step(r, 110, [{"axis": "move_right", "value": None}, {"action": "aim", "down": False}])
    r.checkpoint()


def main():
    if not os.path.exists(OHAO):
        print("ohao is not built: " + OHAO)
        return 1
    shutil.rmtree(TMP, ignore_errors=True)
    print("replay_test (requirement 4: gameplay replays exactly)")
    s = Serve()
    try:
        s.tool("scene_load", {"path": SCENE, "replace": True, "seed": SEED})
        start_hash = hash_of(s)
        start_pos = s.tool("player_info")["pos"]
        r = Recorder(s)
        first_half(r)
        r.checkpoint()
        fight(r, s)
        s.tool("world_snapshot", {"name": "mid"})   # a grenade in flight, a strafe and the aim held
        mid = len(r.commands)
        after_throw(r)
        end_pos = s.tool("player_info")["pos"]
        kinds, since = [], 0
        while True:   # the slice's events in pages of 64
            page = s.tool("events_read", {"since": since, "max": 64, "names": ["slice.*"]})
            kinds += [e["event"][len("slice."):] for e in page["events"]]
            if not page["events"] or page["next"] == since:
                break
            since = page["next"]

        # The fight happened (else the hashes below prove nothing).
        check(kinds.count("shot") >= 9, "rounds fired: %d shots (want 9)" % kinds.count("shot"))
        check("hit" in kinds, "a round hit something (%d hits)" % kinds.count("hit"))
        check("explode" in kinds, "the grenade exploded")
        check(math.dist(start_pos, end_pos) > 1.0, "the player moved %.2f m" % math.dist(start_pos, end_pos))
        check(len(set([start_hash] + r.hashes)) == len(r.hashes) + 1,
              "world.hash changed at every checkpoint (%d distinct)" % len(set([start_hash] + r.hashes)))

        # 1. Replay from the midway snapshot (taken right after the fourth checkpoint).
        s.tool("world_restore", {"name": "mid"})
        check(hash_of(s) == r.hashes[3], "the restore puts back the midway world.hash")
        again = replay(s, r.commands[mid:])
        check(again == r.hashes[4:], "replayed from midway: the same world.hash at %d checkpoint(s)%s"
              % (len(again), "" if again == r.hashes[4:] else " (got %s, want %s)" % (again, r.hashes[4:])))

    except RuntimeError as e:
        failures.append(str(e))
        print("  FAIL  " + str(e))
    code = s.close()
    check(code == 0, "ohao serve exits 0 (%s)" % code)
    if failures:
        return finish()

    # 2. A new process: load, then every command again.
    s = Serve()
    try:
        s.tool("scene_load", {"path": SCENE, "replace": True, "seed": SEED})
        check(hash_of(s) == start_hash, "a new process's load has the first load's world.hash")
        fresh = replay(s, r.commands)
        check(fresh == r.hashes, "in a new process: the same world.hash at all %d checkpoints%s"
              % (len(fresh), "" if fresh == r.hashes else " (got %s, want %s)" % (fresh, r.hashes)))
    except RuntimeError as e:
        failures.append(str(e))
        print("  FAIL  " + str(e))
    code = s.close()
    check(code == 0, "the second ohao serve exits 0 (%s)" % code)
    return finish()


def finish():
    shutil.rmtree(TMP, ignore_errors=True)
    print("Results: %d/%d passed" % (passed, passed + len(failures)))
    print(json.dumps({"suite": "replay_test", "passed": passed, "failed": len(failures), "failures": failures}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
