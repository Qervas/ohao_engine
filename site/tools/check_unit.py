#!/usr/bin/env python3
"""Validate ONE authored monograph unit before the site build sees it.

Usage:  python site/tools/check_unit.py <module>/<id>
        python site/tools/check_unit.py materials/pbr-model

Checks (all must pass):
  1. every {{cite path "substring"}} resolves to exactly ONE line
  2. every $$math$$ block sits in a section that also has explanatory prose
  3. no banned filler strings
  4. frontmatter declares standard: v2
  5. {{figure id}} references an existing site/assets/units/<id>.svg

Exit 0 = PASS, 1 = FAIL. Safe to run concurrently: read-only.
Does NOT rebuild the site — never run generate_tree.py from a parallel agent.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SITE / "tools"))

from monograph import blocks, cite  # noqa: E402
from monograph.paths import CONTENT, SITE as SITE_ROOT  # noqa: E402

BANNED = ["Sources of truth:", "API / symbols the unit exposes or binds"]
CITE_RE = re.compile(r'\{\{cite\s+(\S+)\s+"(.+?)"\}\}')
FIG_RE = re.compile(r'\{\{figure\s+(\S+)\s+"(.*?)"\}\}')


def check(unit: str) -> int:
    if "/" not in unit:
        print(f"FAIL  expected <module>/<id>, got {unit!r}")
        return 1
    mod, uid = unit.split("/", 1)
    path = CONTENT / mod / f"{uid}.md"
    if not path.is_file():
        print(f"FAIL  no such unit file: {path}")
        return 1

    raw = path.read_text(encoding="utf-8")
    fm, body = blocks.split_frontmatter(raw)
    problems: list[str] = []
    notes: list[str] = []

    # 1. citations
    n_cites = 0
    for m in CITE_RE.finditer(body):
        n_cites += 1
        try:
            line, text = cite.resolve_citation(m.group(1), m.group(2))
            notes.append(f"  cite OK  {m.group(1)}:{line}")
        except cite.CitationError as e:
            problems.append(f"  CITE  {e}")

    # 2. math explained
    for slug, has_prose in blocks.iter_math_sections(raw):
        if not has_prose:
            problems.append(f"  MATH  section '{slug}' has $$math$$ but no explanatory prose")

    # 3. banned filler
    for b in BANNED:
        if b in body:
            problems.append(f"  FILLER  banned string present: {b!r}")

    # 4. standard
    if fm.get("standard") != "v2":
        problems.append("  FRONTMATTER  missing 'standard: v2'")

    # 5. figures exist
    for m in FIG_RE.finditer(body):
        svg = SITE_ROOT / "assets" / "units" / f"{m.group(1)}.svg"
        if not svg.is_file():
            problems.append(f"  FIGURE  missing asset: {svg.relative_to(SITE_ROOT)}")
        elif not m.group(2).strip():
            problems.append(f"  FIGURE  '{m.group(1)}' has an empty caption")

    # body must parse (fail-loud grammar: unterminated $$ / ::: / ``` raises)
    try:
        blocks.render_body(raw)
    except ValueError as e:
        problems.append(f"  GRAMMAR  {e}")
    except cite.CitationError:
        pass  # already reported above

    words = len(re.sub(r"\{\{.*?\}\}", "", body).split())
    print(f"unit {unit}: {words} words, {n_cites} citations")
    for n in notes:
        print(n)
    if problems:
        print("FAIL")
        for p in problems:
            print(p)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(check(sys.argv[1]))
