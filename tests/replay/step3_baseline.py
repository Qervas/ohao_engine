#!/usr/bin/env python3
"""Step 3's behaviour baseline. Temporary: docs/plans/2026-11-09-step3-engine-game-boundary.md, deleted when step 3
closes. replay_test compares two runs of one build, so a refactor that changes the fight passes it. This plays
replay_test's script and writes, at each checkpoint, the canonical scene, world.hash, the combat log, the World's
event log (events.read: physics, animation, the audio commands; step 3c), the vfx records and the player. An older
recording's physics and audio logs (physics.events, audio.info's commands) compare with the event log by meaning:
seqs are left out (one sequence now), floats within their old rounding. Not a gate: run by hand.

    python tests/replay/step3_baseline.py OUT.json             record
    python tests/replay/step3_baseline.py --compare A.json B.json   what differs
"""
import json, math, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "replay"))
import replay_test as rt  # noqa: E402


def pages(s, tool, key, page=64):
    out, since = [], 0
    while True:
        r = s.tool(tool, {"since": since, "max": page})
        out += r.get(key, [])
        if not r.get(key) or r.get("next") == since:
            return out
        since = r["next"]


def events(s):
    out, since = [], 0
    while True:
        r = s.tool("events_read", {"since": since, "max": 64})
        out += r["events"]
        if not r["events"] or r["next"] == since:
            return out
        since = r["next"]


def second_half(r, s):
    """replay_test's fight as it was at a98d34e2, when the baseline was recorded (replay_test has changed since)."""
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
        rt.step(r, 13, [{"action": "fire", "down": True}])
        rt.step(r, 1, [{"action": "fire", "down": False}])
    r.checkpoint()
    rt.step(r, 1, [{"action": "reload", "down": True}])
    rt.step(r, 100, [{"action": "reload", "down": False}])
    r.checkpoint()
    rt.step(r, 1, [{"action": "grenade", "down": True}])
    rt.step(r, 160, [{"action": "grenade", "down": False}])
    r.checkpoint()


class Rec(rt.Recorder):
    def __init__(self, s):
        super().__init__(s)
        self.points = []

    def checkpoint(self):
        super().checkpoint()
        s = self.s
        self.points.append({
            "hash": self.hashes[-1],
            "scene": s.tool("scene_save", {})["scene"],
            "player": s.tool("player_info"),
            "events": events(s),
            "vfx": pages(s, "vfx_info", "records", 16),
        })


def record(out):
    s = rt.Serve()
    try:
        s.tool("scene_load", {"path": rt.SCENE, "replace": True, "seed": rt.SEED})
        r = Rec(s)
        r.checkpoint()
        rt.first_half(r)
        r.checkpoint()
        second_half(r, s)
    finally:
        s.close()
    json.dump({"commands": r.commands, "points": r.points}, open(out, "w"), indent=1, sort_keys=True)
    print("%s: %d checkpoints, %d commands" % (out, len(r.points), len(r.commands)))


ZERO = "0000000000000000"


def physics_of(point):
    """(step, kind, a, b, point, normal, speed, surface) per physics event, from either log."""
    if "physics" in point:
        return [(e["step"], e["kind"], e["a"], e["b"], e["point"], e["normal"], e["speed"], e.get("surface"))
                for e in point["physics"]]
    out = []
    for e in point["events"]:
        if not e["event"].startswith("physics."):
            continue
        kind = e["event"][len("physics."):]
        a, b = (("a", "b") if "contact" in kind else ("trigger", "body") if "trigger" in kind else
                ("joint", "other") if kind == "joint_break" else ("player", "ground"))
        out.append((e["step"], kind, e.get(a, ZERO), e.get(b, ZERO), e["point"], e.get("normal", [0, 0, 0]),
                    e.get("speed", 0), e.get("surface")))
    return out


def combat_of(point):
    """(step, kind, fields) per combat event, from combat.events or the event log's slice.*."""
    out = []
    if "combat" in point:
        for e in point["combat"]:
            k = e["kind"]
            if k == "shot":
                f = (e.get("source"), e["point"], e["dir"], e["coneDeg"])
            elif k == "hit":
                f = (e.get("source"), e.get("target"), e["point"], e["normal"], e["dist"], e["surface"])
            elif k == "damage":
                f = (e.get("source"), e.get("target"), e["amount"], e["health"])
            elif k == "death":
                f = (e.get("source"), e.get("target"))
            elif k in ("reload", "reloaded"):
                f = (e.get("source"), e["ammo"])
            elif k == "explode":
                f = (e.get("source"), e["point"])
            else:
                f = ()
            out.append((e["step"], k, f))
        return out
    for e in point["events"]:
        if not e["event"].startswith("slice."):
            continue
        k = e["event"][len("slice."):]
        if k == "shot":
            f = (e.get("shooter"), e["eye"], e["dir"], e["coneDeg"])
        elif k == "hit":
            f = (e.get("shooter"), e.get("body"), e["point"], e["normal"], e["dist"], e["surface"])
        elif k == "damage":
            f = (e.get("source"), e.get("target"), e["amount"], e["health"])
        elif k == "death":
            f = (e.get("killer"), e.get("target"))
        elif k in ("reload", "reloaded"):
            f = (e.get("holder"), e["ammo"])
        elif k == "explode":
            f = (e.get("thrower"), e["point"])
        else:
            f = ()
        out.append((e["step"], k, f))
    return out


def steps60(e):
    """An audio event's blend in steps of 1/60 s: blendSteps before step 3e, blendMs since."""
    return e["blendSteps"] if "blendSteps" in e else round(e["blendMs"] * 60 / 1000)


def audio_of(point):
    """(step, op, fields) per audio command, from either log; a play's gain only where the old log wrote it."""
    out = []
    if "audio" in point:
        for c in point["audio"]:
            op = c["op"]
            if op == "play":
                f = (c["sound"], c["voice"], c.get("variant", 0), c.get("pos"), c.get("occl"),
                     c.get("outcome", "played"), c.get("victim", 0), c.get("gain"))
            elif op == "stop":
                f = (c["voice"],)
            elif op == "bus":
                f = (c["bus"], c["db"], c["blend"])
            elif op == "snapshot":
                f = (c["snapshot"], c["blend"])
            elif op == "zone":
                f = (c["zone"], c.get("bed", ""), c["blend"])
            elif op == "occlusion":
                f = (c["voice"], c["occl"], c["blend"])
            else:
                f = (c["blend"],)
            out.append((c["step"], op, f, bool(c.get("resync"))))
        return out
    for e in point["events"]:
        if not e["event"].startswith("audio."):
            continue
        op = e["event"][len("audio."):]
        if op == "play":
            f = (e["sound"], e["voice"], e.get("variant", 0), e.get("pos"),
                 e.get("occlusionDb") if e.get("walls") else None, e.get("outcome", "played"), e.get("victim", 0),
                 e["gain"])
        elif op == "stop":
            f = (e["voice"],)
        elif op == "bus":
            f = (e["bus"], e["db"], steps60(e))
        elif op == "snapshot":
            f = (e["snapshot"], steps60(e))
        elif op == "zone":
            f = (e["zone"], e.get("bed", ""), steps60(e))
        elif op == "occlusion":
            f = (e["voice"], e["occlusionDb"], steps60(e))
        else:
            f = (steps60(e),)
        out.append((e["step"], op, f, bool(e.get("resync"))))
    return out


def near(x, y, tol):
    if isinstance(x, (list, tuple)) and isinstance(y, (list, tuple)):
        return len(x) == len(y) and all(near(u, v, tol) for u, v in zip(x, y))
    if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool):
        return abs(x - y) <= tol
    return x == y


def same_logs(old, new, tol, gain_at=None):
    if len(old) != len(new):
        return "%d events, %d now" % (len(old), len(new))
    for k, (o, n) in enumerate(zip(old, new)):
        if gain_at is not None and o[1] == "play" and o[2][gain_at] is None:   # the old log left the volume out
            n = (n[0], n[1], n[2][:gain_at] + (None,) + n[2][gain_at + 1:], n[3])
        if not near(o, n, tol):
            return "event %d: %s, now %s" % (k, o, n)
    return None


def compare(a, b):
    A, B = json.load(open(a)), json.load(open(b))
    bad = 0
    for i, (p, q) in enumerate(zip(A["points"], B["points"])):
        for k in sorted(set(p) | set(q)):
            if k in ("physics", "audio", "combat", "events"):
                continue
            if p.get(k) != q.get(k):
                bad += 1
                print("checkpoint %d: %s differs" % (i, k))
        why = same_logs(combat_of(p), combat_of(q), 0.006)
        if why:
            bad += 1
            print("checkpoint %d: combat: %s" % (i, why))
        why = same_logs(physics_of(p), physics_of(q), 0.002)
        if why:
            bad += 1
            print("checkpoint %d: physics: %s" % (i, why))
        why = same_logs(audio_of(p), audio_of(q), 0.006, gain_at=7)
        if why:
            bad += 1
            print("checkpoint %d: audio: %s" % (i, why))
    if len(A["points"]) != len(B["points"]):
        bad += 1
        print("checkpoint counts differ")
    print("same" if not bad else "%d differences" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(compare(sys.argv[2], sys.argv[3]) if sys.argv[1] == "--compare" else record(sys.argv[1]))
