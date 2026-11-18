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
  5. quoted and angle-bracket includes alike;
  6. each module is deletable: outside it, its headers are included only by the modules that declare it and by the
     libraries above the modules that link it (their declaration), ohao_<m> is linked by no module that does not
     declare it, and its module function (<m>Module) and its installs (the install* functions its sources declare)
     are named by nothing but the module list (ohao_engine, the module function only). The core, ecs, is not
     checked: every module declares it. A module's removal is then its directory, its line in the module list and
     the declarations of what uses it.
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
# A function declared or defined at a line's start (a namespace's scope): `void installPhysics(`, `VfxSystem& installVfx(`:
# the line before the name, and the name and its parenthesis.
DECL_HEAD = re.compile(rb'(?:[A-Za-z_][\w:<>]*[&*]?[ \t]+)+')
INSTALL_NAME = re.compile(rb'install[A-Z]\w*[ \t]*\(')
CALL_TAIL = re.compile(rb'[ \t]*\(')


def calls(text):
    """(offset, name) of each call of an install* function or a *Module function in `text`: memmem for the two words,
    then a look at each (few) occurrence."""
    at = text.find(b"install")
    while at >= 0:
        before = text[at - 1:at]
        m = INSTALL_NAME.match(text, at)
        if m and not (before.isalnum() or before == b"_"):
            yield at, text[at:m.end()].rstrip(b"( \t").decode()
        at = text.find(b"install", at + 1)
    at = text.find(b"Module")
    while at >= 0:
        end = at + len(b"Module")
        if CALL_TAIL.match(text, end):
            start = at
            while start > 0 and (text[start - 1:start].isalnum() or text[start - 1:start] == b"_"):
                start -= 1
            if start < at and text[start:start + 1].islower():
                yield start, text[start:end].decode()
        at = text.find(b"Module", at + 1)


def install_decls(text):
    """The install* functions `text` declares or defines at a line's start."""
    out = set()
    at = text.find(b"install")
    while at >= 0:
        line = text.rfind(NEWLINE, 0, at) + 1
        name = INSTALL_NAME.match(text, at)
        if name and DECL_HEAD.fullmatch(text, line, at):
            out.add(text[at:name.end()].rstrip(b"( \t").decode())
        at = text.find(b"install", at + 1)
    return out
MODULE_LIST = "ohao_engine"
CORE = "ecs"


def camel(name):
    """"world_render" -> "worldRender": a module's function is <camel>Module."""
    head, *rest = name.split("_")
    return head + "".join(w[:1].upper() + w[1:] for w in rest)


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

    texts = {}
    for lib in libs.values():
        for rel in lib["sources"]:
            if rel not in texts:
                try:
                    texts[rel] = (root / rel).read_bytes()
                except OSError:
                    texts[rel] = None
    top = max((level["ohao_" + m] for m in modules if "ohao_" + m in level), default=-1)

    def above(lib):
        return lib not in module_of and level[lib] > top

    # 6. Links of a module to another it does not declare (ohao_add_module links its DEPENDS).
    for name, lib in sorted(libs.items()):
        mine = module_of.get(name)
        if not mine:
            continue
        for d in lib["links"]:
            theirs = module_of.get(d)
            if theirs and theirs != CORE and theirs not in modules.get(mine, []):
                problems.append(f"{name} links {d}: {mine} does not declare {theirs} (rule 6: a module is linked "
                                f"only by the modules that declare it)")

    # 6. Each module's installs and module function: named only by the module list.
    owned_names = {}
    for name, lib in libs.items():
        mine = module_of.get(name)
        if not mine or mine == CORE:
            continue
        found = {camel(mine) + "Module"}
        for rel in lib["sources"]:
            if texts.get(rel):
                found |= install_decls(texts[rel])
        for n in found:
            owned_names[n] = name

    # 1-3, 5, 6. Includes, quoted and angle; and the names.
    for name, lib in sorted(libs.items()):
        mine = module_of.get(name)
        declared = set(modules.get(mine, [])) if mine else set()
        is_game = name in game_targets
        linked = set(lib["links"])
        for rel in lib["sources"]:
            text = texts.get(rel)
            if text is None:
                continue
            for at, fn in calls(text):
                owner_lib = owned_names.get(fn)
                if not owner_lib or owner_lib == name or (name == MODULE_LIST and fn.endswith("Module")):
                    continue
                line = text.count(NEWLINE, 0, at) + 1
                what = "module function" if fn.endswith("Module") else "install"
                whose = "the module list (" + MODULE_LIST + ")" if fn.endswith("Module") else "the module itself"
                problems.append(f"{rel}:{line} names {fn}, the {module_of[owner_lib]} module's {what}: named only by "
                                f"{whose} (rule 6)")
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
                    continue
                if theirs and theirs != CORE and above(name) and dst not in linked:
                    problems.append(f"{at()} includes {path}: {name} does not declare {theirs} (rule 6: a library "
                                    f"above the modules links each module it includes, target_link_libraries({name} "
                                    f"... {dst}))")
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
