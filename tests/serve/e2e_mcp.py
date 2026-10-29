#!/usr/bin/env python3
"""
e2e_mcp -- test 19 (sub-project 4, step 11): an MCP client drives the live engine end to end.

A stdlib-only MCP client (newline-delimited JSON-RPC on the server's stdio, every call a
`tools/call`) connects to `ohao serve --window` (a hidden window, validation on) and runs
the acceptance session of the spec's test 19:

  initialize -> load helmet_game -> summary -> apply an edit -> snapshot -> record ->
  inject a 61-step flight -> step with watch -> capture inline and to a file -> frame
  stats (sub-project 2b, test 14) -> log read ->
  stop recording -> restore -> replay (every checkpoint matches) -> capture again ->
  load the blockout level -> level.info -> a ray down at the player spawn -> physics.info ->
  (sub-project 6) lift and drop the crates -> physics.events -> load the play scene ->
  player.info -> look -> snapshot -> walk, turn and jump on injected input -> player.info,
  footsteps -> look -> player.teleport -> physics.contacts -> restore -> the same walk ->
  (sub-project 7) load the play scene again -> combat.info -> shoot EnemyA down ->
  combat.events -> audio.info, audio.mix (mute, solo, clear) -> look with the HUD -> combat.damage kills the player -> the restart ->
  clear the room -> won

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


def walk(c):
    """The play scene's walk (sub-project 6, step 5): a quarter turn left by the mouse, west along the
    south of the room, a turn back, north up the lane at x -1, a jump. Returns the last step's reply."""
    steps = [([{"mouse": [-900, 0]}], 1),
             ([{"axis": "move_forward", "value": 1}], 35),
             ([{"axis": "move_forward", "value": None}, {"mouse": [900, 0]}], 10),
             ([{"axis": "move_forward", "value": 1}], 60),
             ([{"action": "jump", "down": True}], 1),
             ([{"action": "jump", "down": False}, {"axis": "move_forward", "value": None}], 60)]
    last = None
    for events, frames in steps:
        _, _, err = c.tool("input_inject", {"events": events})
        last, _, err2 = c.tool("world_step", {"frames": frames})
        if err or err2:
            return None
    return last


def walk_the_level(c):
    """An agent walks the blockout as a player would (sub-project 6, step 5): it looks through the
    player's eyes, walks with injected input, reads where it is and what it stood on, teleports,
    and replays the walk from a snapshot to the same world hash."""
    loaded, _, err = c.tool("scene_load", {"path": "assets/scenes/blockout_play.scene.json", "replace": True})
    check(not err and loaded.get("entities", 0) > 0, "scene_load the play scene (replace)")
    spawn, _, err = c.tool("player_info")
    check(not err and spawn.get("pos") == [1.5, 0, 2.2] and spawn.get("ground") is True and spawn.get("on") == "Floor",
          "player_info: at the spawn (1.5, 0, 2.2), standing on the Floor")
    _, eyes0, err = c.tool("render_capture", {"inline": True, "path": OUT + "/walk_0.png"})
    check(not err and eyes0 is not None, "render_capture: the view from the player's eyes at the spawn")
    snap, _, err = c.tool("world_snapshot", {"name": "walk"})
    check(not err and snap is not None, "world_snapshot before the walk")
    events0 = ((c.tool("physics_events", {"since": 0, "max": 64})[0]) or {}).get("next", 0)

    stepped = walk(c)
    there, _, err = c.tool("player_info")
    pos = (there or {}).get("pos", [9, 9, 9])
    print("        walked to %s, yaw %s, %s footsteps" % (pos, (there or {}).get("yawDeg"), (there or {}).get("footsteps")))
    check(stepped is not None and not err and -1.3 < pos[0] < -0.6 and pos[2] < -1.0 and abs(pos[1]) < 0.01 and
          there.get("yawDeg") == 0 and there.get("ground") is True,
          "walked west then north up the lane x -1 and jumped: on the floor at (%.2f, %.2f), facing -Z" % (pos[0], pos[2]))
    events, _, err = c.tool("physics_events", {"since": events0, "max": 64})
    kinds = [e.get("kind") for e in (events or {}).get("events", [])]
    check(not err and kinds.count("footstep") >= 8 and kinds.count("land") == 1,
          "physics_events: the walk's footsteps (%d) and the jump's landing" % kinds.count("footstep"))
    _, eyes1, err = c.tool("render_capture", {"inline": True, "path": OUT + "/walk_1.png"})
    check(not err and eyes1 is not None and eyes1.get("data") != eyes0.get("data"),
          "render_capture: the view has changed with the walk")
    hash1 = (c.tool("world_hash")[0] or {}).get("hash")

    moved, _, err = c.tool("player_teleport", {"pos": [-2, 0.3, -1.35], "yawDeg": 90})   # clear of EnemyA
    check(not err and moved.get("on") == "Step" and moved.get("surface") == "metal", "player_teleport onto the Step: metal")
    contacts, _, err = c.tool("physics_contacts")
    check(not err and contacts is not None and "count" in contacts, "physics_contacts answers (%s pairs)" % contacts.get("count"))

    restored, _, err = c.tool("world_restore", {"name": "walk"})
    check(not err and restored is not None, "world_restore to the spawn")
    walk(c)
    hash2 = (c.tool("world_hash")[0] or {}).get("hash")
    _, eyes2, _ = c.tool("render_capture", {"inline": True, "path": OUT + "/walk_2.png"})
    check(hash1 is not None and hash1 == hash2, "the same walk from the snapshot reaches the same world hash (%s)" % hash2)
    check(eyes2 is not None and read_bytes(OUT + "/walk_1.png") == read_bytes(OUT + "/walk_2.png"),
          "and the same view, byte for byte")


PLAYER, ENEMY_A, ENEMY_B = "5c0b1a7e00000001", "5c0b1a7e00000010", "5c0b1a7e00000020"


def combatant(info, eid):
    return next((c for c in (info or {}).get("combatants", []) if c.get("entity") == eid), {})


def shoot_down(c, eid, tries=20):
    """Faces the enemy (player_teleport where the player stands) and fires bursts of three rounds
    until combat_info says it is dead. Returns the bursts it took, None if it still lives."""
    for n in range(tries):
        info = c.tool("combat_info")[0]
        me, foe = combatant(info, PLAYER), combatant(info, eid)
        if foe.get("dead"):
            return n
        if me.get("dead") or not me.get("pos") or not foe.get("pos"):
            return None
        p, q = me["pos"], foe["pos"]
        yaw = round(math.degrees(math.atan2(-(q[0] - p[0]), -(q[2] - p[2]))), 2)
        c.tool("player_teleport", {"pos": p, "yawDeg": yaw})
        c.tool("input_inject", {"events": [{"action": "fire", "down": True}]})
        c.tool("world_step", {"frames": 13})   # rounds on steps 1, 7 and 13 (10 Hz)
        c.tool("input_inject", {"events": [{"action": "fire", "down": False}]})
        c.tool("world_step", {"frames": 1})
    return None


def listen_to_the_fight(c):
    """Sub-project 11, step 4: mid-fight, an agent reads the voice schedule (audio_info), mutes a bus, solos
    one and clears the overrides (audio_mix); world_hash never moves."""
    # A cursor past the log comes back as its end: where the log is now.
    end = (c.tool("audio_info", {"since": 1000000000, "max": 0})[0] or {}).get("next", 0)
    info, _, err = c.tool("audio_info", {"since": max(0, end - 64), "max": 64})
    plays = [x.get("sound") for x in (info or {}).get("commands", []) if x.get("op") == "play"]
    check(not err and info.get("device") in ("default", "none") and info.get("zone") == "room" and
          "fire" in plays and "enemy_fire" in plays and len(info.get("buses", [])) == 10,
          "audio_info mid-fight: the player's and the enemies' rounds, the room's zone, ten buses (device %s; %s)" %
          ((info or {}).get("device"), sorted(set(plays))))
    before = (c.tool("world_hash")[0] or {}).get("hash")
    muted, _, err = c.tool("audio_mix", {"bus": "weapons", "mute": True})
    weapons = next((b for b in (muted or {}).get("buses", []) if b.get("name") == "weapons"), {})
    check(not err and weapons.get("mix") == {"mute": True} and "commands" not in muted, "audio_mix mutes the weapons bus")
    soloed, _, err = c.tool("audio_mix", {"bus": "foley", "solo": True})
    foley = next((b for b in (soloed or {}).get("buses", []) if b.get("name") == "foley"), {})
    check(not err and foley.get("mix") == {"solo": True}, "audio_mix solos the foley bus")
    cleared, _, err = c.tool("audio_mix", {"clear": True})
    check(not err and all("mix" not in b for b in (cleared or {}).get("buses", [])), "audio_mix clear: no override left")
    check((c.tool("world_hash")[0] or {}).get("hash") == before, "world_hash unchanged by the overrides")


def fight_the_round(c):
    """An agent plays the play scene's round (sub-project 7, step 5): shoots EnemyA down, is killed,
    waits out the restart, clears the room and wins, reading combat_info and combat_events and
    looking through the player's eyes with the HUD."""
    loaded, _, err = c.tool("scene_load", {"path": "assets/scenes/blockout_play.scene.json", "replace": True})
    check(not err and loaded.get("entities", 0) > 0, "scene_load the play scene for a fight (replace)")
    info, _, err = c.tool("combat_info")
    check(not err and len(info.get("combatants", [])) == 3 and combatant(info, PLAYER).get("player") is True and
          combatant(info, ENEMY_A).get("enemy", {}).get("state") == "idle",
          "combat_info: the player and two idle enemies")
    events0 = (c.tool("combat_events", {"max": 64})[0] or {}).get("next", 0)   # the session's log so far
    c.tool("world_step", {"frames": 1})
    bursts = shoot_down(c, ENEMY_A)
    events, _, err = c.tool("combat_events", {"since": events0, "max": 64})
    kinds = [e.get("kind") for e in (events or {}).get("events", [])]
    check(bursts is not None and not err and "shot" in kinds and "hit" in kinds and "death" in kinds,
          "EnemyA shot down in %s bursts: combat_events has its shots, hits and death" % bursts)
    listen_to_the_fight(c)
    # Sub-project 10: its body plays the death clip (its Transform stays upright) and lands.
    c.tool("world_step", {"frames": 50})
    anim, _, err = c.tool("animation_info", {"entity": ENEMY_A, "since": 0, "max": 64})
    body = ((anim or {}).get("animators") or [{}])[0].get("layers", [{}])[0]
    falls = [e for e in (anim or {}).get("events", []) if e.get("entity") == ENEMY_A and e.get("name") == "body_fall"]
    check(not err and body.get("state") == "dead" and len(falls) == 1,
          "animation_info: EnemyA's death clip played to its end (%s), body_fall once" % body.get("state"))
    _, hud, err = c.tool("render_capture", {"inline": True, "hud": True, "path": OUT + "/fight_hud.png"})
    check(not err and hud is not None, "render_capture with the HUD after the kill")
    killed, _, err = c.tool("combat_damage", {"entity": PLAYER, "amount": 1000, "source": ENEMY_B})
    check(not err and killed.get("dead") is True and killed.get("health") == 0, "combat_damage kills the player")
    c.tool("world_step", {"frames": 181})
    after, _, _ = c.tool("combat_info")
    check(after.get("round", {}).get("restarts") == 1 and
          all(x.get("health") == 100 for x in after.get("combatants", [])),
          "3 s later the round restarted: everyone at 100")
    a2, b = shoot_down(c, ENEMY_A), shoot_down(c, ENEMY_B)
    c.tool("world_step", {"frames": 1})
    won, _, _ = c.tool("combat_info")
    check(a2 is not None and b is not None and won.get("round", {}).get("phase") == "won",
          "the room cleared (EnemyA in %s bursts, EnemyB in %s): the round is won" % (a2, b))
    # An agent forces a state: an edit of the saved Animator through world_apply.
    forced = {"op": "component.set", "id": ENEMY_B, "type": "Animator",
              "value": {"graph": {"path": "assets/anim/enemy.animgraph.json"},
                        "layers": [{"state": "death", "steps": 10}, {"state": "pitch"}, {"state": "still"},
                                   {"state": "still"}, {"state": "still"}]}}
    applied, _, err = c.tool("world_apply", {"commands": [forced]})
    anim, _, _ = c.tool("animation_info", {"entity": ENEMY_B})
    layer = ((anim or {}).get("animators") or [{}])[0].get("layers", [{}])[0]
    check(not err and applied.get("applied") == 1 and layer.get("state") == "death",
          "world_apply component.set on EnemyB's Animator: animation_info reads the forced state")
    # Refused params are a JSON-RPC error (-32602), not a tool result.
    refused = c.request("tools/call", {"name": "combat_damage", "arguments": {"entity": ENEMY_A, "amount": 20000}})
    check(refused is not None and refused.get("error", {}).get("code") == -32602 and "E_RANGE" in json.dumps(refused),
          "combat_damage with an amount past 10,000: -32602, E_RANGE")


def throw_the_grenade(c):
    """sub-project 12 (ruling 23): the play scene's grenade thrown and left to boom 151 steps on. The
    boom writes its records (an effect, a transient light, the scorch decal), the frame shows them, and a
    restore and the same throw reach the same world hash and picture."""
    loaded, _, err = c.tool("scene_load", {"path": "assets/scenes/blockout_play.scene.json", "replace": True})
    check(not err and loaded.get("entities", 0) > 0, "scene_load the play scene for a grenade (replace)")
    snap, _, err = c.tool("world_snapshot", {"name": "vfx"})
    check(not err and snap is not None, "world_snapshot before the throw")

    def throw(tag):
        _, _, e1 = c.tool("input_inject", {"events": [{"action": "grenade", "down": True},
                                                       {"step": 1, "action": "grenade", "down": False}]})
        stepped, _, e2 = c.tool("world_step", {"frames": 151})
        _, img, e3 = c.tool("render_capture", {"inline": True, "path": OUT + "/boom_%s.png" % tag})
        return stepped, img, bool(e1 or e2 or e3)

    stepped, img1, err = throw("1")
    check(not err and stepped is not None and img1 is not None, "the grenade thrown, 151 steps to the boom, a frame")
    info, _, err = c.tool("vfx_info", {"since": 0, "max": 16})
    kinds = {k for r in (info or {}).get("records", []) for k in ("effect", "light", "decal") if k in r}
    check(not err and kinds == {"effect", "light", "decal"},
          "the boom wrote an effect, a light and a decal (%s)" % sorted(kinds))
    hash1 = (c.tool("world_hash")[0] or {}).get("hash")
    restored, _, err = c.tool("world_restore", {"name": "vfx"})
    check(not err and restored is not None, "world_restore to the throw's start")
    _, img2, err = throw("2")
    hash2 = (c.tool("world_hash")[0] or {}).get("hash")
    check(not err and hash1 is not None and hash1 == hash2, "the same throw after a restore reaches the same world hash")
    check(read_bytes(OUT + "/boom_1.png") == read_bytes(OUT + "/boom_2.png"),
          "the boom's picture after the restore equals the first, byte for byte")


def main():
    if not os.path.exists(OHAO):
        print("ohao is not built: " + OHAO)
        return 1
    shutil.rmtree(TMP, ignore_errors=True)
    print("e2e_mcp (sub-project 4, test 19)")
    env = dict(os.environ)
    env["OHAO_VALIDATION"] = "1"
    c = McpClient([OHAO, "serve", "--window", "960x540", "--hidden", "--sound", "--out", OUT], env)

    init = c.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "e2e_mcp", "version": "1"}})
    ok = init is not None and init.get("result", {}).get("protocolVersion") == "2025-11-25"
    check(ok and init["result"]["serverInfo"]["name"] == "ohao" and "tools" in init["result"]["capabilities"],
          "initialize: MCP 2025-11-25, tools and logging")
    c.notify("notifications/initialized")
    tools = c.request("tools/list")
    names = [t["name"] for t in (tools or {}).get("result", {}).get("tools", [])]
    check(len(names) == 39 and "render_capture" in names and "render_stats" in names and "render_settings" in names
          and "input_replay" in names and "level_info" in names and "physics_raycast" in names
          and "physics_events" in names and "physics_contacts" in names and "player_info" in names
          and "player_teleport" in names and "combat_info" in names and "combat_events" in names
          and "combat_damage" in names and "animation_info" in names and "audio_info" in names
          and "audio_mix" in names and "vfx_info" in names, "tools/list: 39 tools")

    info, _, err = c.tool("engine_info")
    gpu = (info or {}).get("gpu", {})
    check(not err and info.get("host") == "window" and info.get("time", {}).get("mode") == "lockstep" and
          gpu.get("name") and "rayTracing" in gpu and "meshShaders" in gpu,
          "engine_info: a window host in lockstep, on %s" % gpu.get("name"))
    loaded, _, err = c.tool("scene_load", {"path": SCENE, "seed": SEED})
    check(not err and loaded.get("entities", 0) > 0, "scene_load helmet_game (%s entities)" % (loaded or {}).get("entities"))
    summary, _, err = c.tool("world_summary", {"depth": 1})
    check(not err and summary.get("entities") == loaded.get("entities") and summary.get("outline"),
          "world_summary: the outline of the loaded scene")

    found, _, err = c.tool("world_query", {"names": ["SpinningCube"], "fields": ["/Material/base_color"]})
    cube = (found or {}).get("entities", [{}])[0].get("id")
    edited, _, err = c.tool("world_apply", {"commands": [
        {"op": "field.set", "id": cube, "path": "/Material/base_color", "value": [0.1, 0.65, 0.2]}]})
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
    check(not err and stats.get("gpuMs", 0) > 0 and "gbuffer.late" in passes and "lighting" in passes,
          "render_stats: %.3f ms of GPU a frame over %s frames, %d passes" %
          ((stats or {}).get("gpuMs", 0), (stats or {}).get("frames"), len(passes)))

    # A settings change and what it costs (2b test 14): SSR off, the capture's own pass list
    # without it, render_stats over the frames since; then back, so the replay below compares
    # against the first capture.
    settings, _, err = c.tool("render_settings")
    check(not err and settings.get("ssr") is True and settings.get("rtGI", {}).get("rays") == 2,
          "render_settings: the budgeted defaults")
    changed, _, err = c.tool("render_settings", {"set": {"ssr": False}})
    check(not err and changed.get("ssr") is False and changed.get("sss") is True, "render_settings: ssr off, the rest kept")
    timed, _, err = c.tool("render_capture", {"inline": True, "timings": True})
    names_ssr = [p[0] for p in (timed or {}).get("timings", [])]
    check(not err and timed.get("gpuMs", 0) > 0 and "lighting" in names_ssr and "ssr" not in names_ssr,
          "a capture with timings: %.3f ms of GPU, %d passes, no ssr" % ((timed or {}).get("gpuMs", 0), len(names_ssr)))
    stats2, _, err = c.tool("render_stats", {"frames": 1})
    check(not err and "ssr" not in [p[0] for p in (stats2 or {}).get("passes", [])], "render_stats agrees: no ssr")
    back, _, err = c.tool("render_settings", {"set": {"ssr": True}})
    check(not err and back.get("ssr") is True, "render_settings: ssr back on")

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

    # A level (sub-project 5, step 5): load the blockout, read level.info, cast a ray down at
    # the player spawn.
    blockout, _, err = c.tool("scene_load", {"path": "assets/scenes/blockout_room.scene.json", "replace": True})
    check(not err and blockout.get("entities", 0) > 0, "scene_load the blockout (replace)")
    info, _, err = c.tool("level_info")
    spawns = (info or {}).get("spawns", [])
    player = next((sp for sp in spawns if sp.get("tag") == "player"), None)
    info_bytes = len(json.dumps(info, separators=(",", ":")).encode("utf-8")) if info else 0
    check(not err and info.get("nodes") == 18 and len(spawns) == 3 and player is not None and info_bytes <= 1024,
          "level_info: 18 nodes, 3 spawns, a player spawn, %d bytes (at most 1,024)" % info_bytes)
    if player:
        x, y, z = player["pos"]
        hit, _, err = c.tool("physics_raycast", {"from": [x, y + 0.5, z], "dir": [0, -1, 0], "max": 2})
        hit_bytes = len(json.dumps(hit, separators=(",", ":")).encode("utf-8")) if hit else 0
        check(not err and hit.get("hit") is True and hit.get("name") == "Floor" and abs(hit["point"][1] - y) <= 1e-3
              and hit_bytes <= 200,
              "physics_raycast down at the player spawn hits the Floor at %s (%d bytes, at most 200)"
              % ((hit or {}).get("point"), hit_bytes))
    bodies, _, err = c.tool("physics_info")
    check(not err and bodies.get("bodies", {}).get("static") == 12 and bodies.get("bodies", {}).get("dynamic") == 2,
          "physics_info: 12 static bodies (the outdoor Ground among them) and 2 dynamic")

    # Physics in the step (sub-project 6, step 1): lift the crates apart (CrateA 1 m, CrateB 2 m: CrateB
    # stands on CrateA), step, read the landing as an event.
    crates, _, err = c.tool("world_query", {"names": ["CrateA", "CrateB"], "fields": ["/Name/value"]})
    ids = {e["Name"]["value"]: e["id"] for e in (crates or {}).get("entities", [])}
    check(not err and set(ids) == {"CrateA", "CrateB"}, "world_query finds CrateA and CrateB")
    if len(ids) == 2:
        _, _, err = c.tool("world_apply", {"commands": [
            {"op": "field.set", "id": ids["CrateA"], "path": "/Transform/position", "value": [1.3, 1.3, -1.7]},
            {"op": "field.set", "id": ids["CrateB"], "path": "/Transform/position", "value": [1.3, 2.8, -1.7]}]})
        check(not err, "world_apply lifts the crates")
        before, _, _ = c.tool("physics_info")
        start = (before or {}).get("steps", 0)   # the scene's step count runs across the session
        _, _, err = c.tool("world_step", {"frames": 60})
        events, _, err2 = c.tool("physics_events", {"since": 0})
        landed = [e for e in (events or {}).get("events", [])
                  if e.get("kind") == "contact_begin" and {e.get("an"), e.get("bn")} == {"CrateA", "Floor"}
                  and e.get("step", 0) > start]
        after_lift = landed[0]["step"] - start if landed else None
        check(not err and not err2 and len(landed) == 1 and 26 <= after_lift <= 29,
              "physics_events: CrateA lands on the Floor %s steps after the lift" % after_lift)
        after, _, err = c.tool("world_query", {"names": ["CrateA"], "fields": ["/Transform/position"]})
        y = ((after or {}).get("entities") or [{}])[0].get("Transform", {}).get("position", [0, 9, 0])[1]
        check(not err and abs(y - 0.3) < 0.01, "and its Transform rests at y %.4f (0.3, 1 cm)" % y)

    walk_the_level(c)
    fight_the_round(c)
    throw_the_grenade(c)

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
