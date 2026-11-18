#!/usr/bin/env python3
"""check_layers.py -- the engine's layers and modules, held to the graph CMake declares.

    python tools/check_layers.py [build/check/modules.json]     # exit 0: none broken; 1: each problem on a line

cmake/ohao_layers.cmake writes modules.json at configure (each library's level, sources and links; each module's
DEPENDS, from ohao_add_module) and runs this, so configure fails on a problem; tools/check.py runs it on every run, so
an include added to an existing file without a reconfigure is refused too. The rules, each problem naming the file,
the line and the rule (docs/specs/2026-11-18-engine-modules-design.md, 5.3):
  1. levels: a library links and includes only libraries below its own; a game's sources include the engine through
     game/api.hpp only;
  2. a module's source includes another module's header only if that module is in its DEPENDS (direct);
  3. a header under a library's private/ is included only by that library's own sources;
  4. DEPENDS names only modules that exist, and the module graph has no cycle (printed in order);
  5. quoted and angle-bracket includes alike.
A header belongs to the library that lists it among its sources, by its path under ohao/ (or games/), as sources
include it. An include inside `#if 0` or a comment counts; one built from a macro is missed.
Standard library only.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NEWLINE = b"\n"
INCLUDE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]', re.M)


def include_key(rel):
    """A header's path as sources include it: under ohao/ ("ecs/world.hpp") or games/ ("slice/combat.hpp")."""
    for top in ("ohao/", "games/"):
        if rel.startswith(top):
            return rel[len(top):]
    return None


def cycle_of(modules):
    """A cycle in the module graph, in order (its first module again at the end), or None."""
    state = {}
    stack = []

    def visit(m):
        state[m] = 1
        stack.append(m)
        for d in modules.get(m, []):
            if d not in modules:
                continue
            if state.get(d) == 1:
                return stack[stack.index(d):] + [d]
            if d not in state:
                found = visit(d)
                if found:
                    return found
        stack.pop()
        state[m] = 2
        return None

    for m in sorted(modules):
        if m not in state:
            found = visit(m)
            if found:
                return found
    return None


def check(graph):
    """[problem] for the graph modules.json holds."""
    root = Path(graph.get("root", ROOT))
    libs = graph["libraries"]
    modules = graph.get("modules", {})
    games = graph.get("games", {})
    engine_dirs = set(graph.get("engineDirs", []))
    exceptions = set(graph.get("exceptions", []))
    level = {name: lib["level"] for name, lib in libs.items()}
    module_of = {"ohao_" + m: m for m in modules}
    game_targets = set(games.values())
    problems = []

    # 4. DEPENDS: existing modules, no cycle.
    for m, deps in sorted(modules.items()):
        for d in deps:
            if d not in modules:
                problems.append(f"module {m} DEPENDS {d}, which is not a module (rule 4: ohao_add_module(<name>) "
                                f"declares one)")
    cyc = cycle_of(modules)
    if cyc:
        problems.append("the modules depend on each other in a cycle: " + " -> ".join(cyc) + " (rule 4)")

    # Who owns each header.
    owner = {}
    for name, lib in libs.items():
        for rel in lib["sources"]:
            if rel.endswith((".hpp", ".h")):
                key = include_key(rel)
                if key:
                    owner[key] = name

    def edge_ok(src, dst):
        return level[dst] < level[src] or f"{src}->{dst}" in exceptions

    # 1. Links point down.
    for name, lib in sorted(libs.items()):
        for d in lib["links"]:
            if d in level and not edge_ok(name, d):
                problems.append(f"{name} (level {level[name]}) links {d} (level {level[d]}): a dependency points "
                                f"up a layer (rule 1)")

    # 1-3, 5. Includes, quoted and angle.
    for name, lib in sorted(libs.items()):
        mine = module_of.get(name)
        declared = set(modules.get(mine, [])) if mine else set()
        is_game = name in game_targets
        for rel in lib["sources"]:
            try:
                text = (root / rel).read_bytes()
            except OSError:
                continue
            for m in INCLUDE.finditer(text):
                path = m.group(1).decode("utf-8", "replace")

                def at():
                    return f"{rel}:{text.count(NEWLINE, 0, m.start()) + 1}"

                if is_game:
                    if path.split("/", 1)[0] in engine_dirs and path != "game/api.hpp":
                        problems.append(f"{at()} includes {path}: a game includes the engine through game/api.hpp "
                                        f"only (rule 1)")
                    continue
                dst = owner.get(path)
                if not dst or dst == name:
                    continue
                if "/private/" in "/" + path:
                    problems.append(f"{at()} includes {path}: private to {dst} (rule 3)")
                    continue
                # Between two modules, a sideways or downward include is refused by the declaration (a declared module
                # is always below: levels follow DEPENDS), so it is named as such; an upward one by its level.
                theirs = module_of.get(dst)
                if (mine and theirs and theirs not in declared and f"{name}->{dst}" not in exceptions
                        and level[dst] <= level[name]):
                    problems.append(f"{at()} includes {path}: {mine} does not declare {theirs} (rule 2: "
                                    f"ohao_add_module({mine} DEPENDS ...))")
                    continue
                if not edge_ok(name, dst):
                    problems.append(f"{at()} includes {path}: {name} (level {level[name]}) cannot include {dst} "
                                    f"(level {level[dst]}): a dependency points up a layer (rule 1)")
    return problems


def main(argv):
    path = Path(argv[1]) if len(argv) > 1 else ROOT / "build" / "check" / "modules.json"
    try:
        graph = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"check_layers: cannot read {path}: {e} (configure writes it: cmake/ohao_layers.cmake)")
        return 1
    problems = check(graph)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
