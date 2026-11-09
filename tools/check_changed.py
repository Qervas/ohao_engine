"""Which tests a change reaches: the rules behind `python tools/check.py --changed [REF]`.

A changed path is matched against RULES top to bottom; every rule that matches adds its targets
(test names, or `label:<label>` for every test with that label). A path no rule matches selects
the whole default set (quick, gpu): an unmapped change is never tested by nothing. Paths that
cannot change a test's result select nothing.
"""
from __future__ import annotations

import fnmatch
import subprocess
from pathlib import Path


# (glob, targets). fnmatch globs on '/'-separated paths; '*' crosses directories.
RULES: list[tuple[str, list[str]]] = [
    # Nothing a test reads.
    ("site/*", []), ("docs/*", []), ("*.md", []), ("LICENSE", []), (".gitignore", []), ("tests/bench/*", []),
    ("tools/bench/*", []),
    # The renderer: every GPU gate.
    ("ohao/render/*", ["label:gpu", "render_golden"]), ("ohao/gpu/*", ["label:gpu", "render_golden"]),
    ("shaders/*", ["label:gpu", "render_golden"]),
    ("tests/renderer/*", ["label:gpu"]), ("tests/validation/*", ["check_validation"]),
    ("tests/golden/*", ["render_golden"]),
    # Gameplay: anything a world steps through, and the scenes it plays.
    ("ohao/ecs/*", ["replay_test"]), ("ohao/physics/*", ["replay_test"]), ("ohao/level/*", ["replay_test"]),
    ("ohao/anim/*", ["replay_test"]), ("ohao/audio/*", ["replay_test"]), ("ohao/vfx/*", ["replay_test"]),
    ("assets/*", ["replay_test", "label:gpu", "render_golden"]), ("tests/replay/*", ["replay_test"]),
    # The agent interface and the hosts: replays go through ohao serve; captures through the render host.
    ("ohao/api/*", ["replay_test"]), ("ohao/rpc/*", ["replay_test"]), ("ohao/engine/*", ["replay_test"]),
    ("ohao/runtime/*", ["label:gpu", "render_golden"]), ("tools/ohao/*", ["replay_test", "label:gpu", "render_golden"]),
    ("tests/common/*", ["label:gpu", "replay_test"]),
]


def changed_paths(root: Path, ref: str) -> list[str]:
    """Tracked files that differ from `ref` (staged or not), and untracked ones not ignored."""
    def git(*args: str) -> list[str]:
        r = subprocess.run(["git", *args], capture_output=True, text=True, cwd=root)
        if r.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
        return [l for l in r.stdout.splitlines() if l]
    return sorted(set(git("diff", "--name-only", ref)) | set(git("ls-files", "--others", "--exclude-standard")))


def select(paths: list[str], labels_of: dict[str, list[str]], default_labels: list[str], root: Path | None = None):
    """(test names, why: {path: targets or 'unmapped'}). Unknown names in the rules are dropped. With
    `root`, a deleted file under tests/ selects nothing (a removed test cannot fail)."""
    names: set[str] = set()
    why: dict[str, object] = {}
    unmapped = False
    for p in paths:
        if root is not None and p.startswith("tests/") and not (root / p).exists():
            why[p] = []
            continue
        hit = [targets for glob, targets in RULES if fnmatch.fnmatch(p, glob)]
        if not hit:
            unmapped = True
            why[p] = "unmapped"
            continue
        targets = sorted({t for ts in hit for t in ts})
        why[p] = targets
        for t in targets:
            if t.startswith("label:"):
                lab = t[len("label:"):]
                names |= {n for n, ls in labels_of.items() if lab in ls}
            else:
                names.add(t)
    if unmapped:
        names |= {n for n, ls in labels_of.items() if any(l in default_labels for l in ls)}
    return sorted(n for n in names if n in labels_of), why
