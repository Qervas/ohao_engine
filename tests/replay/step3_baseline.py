#!/usr/bin/env python3
"""Step 3's behaviour baseline. Temporary: docs/plans/2026-11-09-step3-engine-game-boundary.md, deleted when step 3
closes. replay_test compares two runs of one build, so a refactor that changes the fight passes it. This plays
replay_test's script and writes, at each checkpoint, the canonical scene, world.hash, and every log (combat,
physics, the audio commands, the vfx records) and the player. Not a gate: run by hand.

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
            "combat": pages(s, "combat_events", "events"),
            "physics": pages(s, "physics_events", "events", 32),
            "audio": pages(s, "audio_info", "commands", 16),
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


def compare(a, b):
    A, B = json.load(open(a)), json.load(open(b))
    bad = 0
    for i, (p, q) in enumerate(zip(A["points"], B["points"])):
        for k in sorted(set(p) | set(q)):
            if p.get(k) != q.get(k):
                bad += 1
                print("checkpoint %d: %s differs" % (i, k))
    if len(A["points"]) != len(B["points"]):
        bad += 1
        print("checkpoint counts differ")
    print("same" if not bad else "%d differences" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(compare(sys.argv[2], sys.argv[3]) if sys.argv[1] == "--compare" else record(sys.argv[1]))
