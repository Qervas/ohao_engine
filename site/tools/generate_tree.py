#!/usr/bin/env python3
"""
Generate hierarchical monograph pages covering ohao/ + shaders/.

Run from repo root:
  python3 site/tools/generate_tree.py
  python3 site/tools/generate_tree.py --author   # regenerate rich markdown first

Package layout (site/tools/monograph/):
  tree/<module>.py     — per-module unit metadata (small files)
  content → site/content/units/<module>/<id>.md  — What/How/Why prose
  render.py / diagrams.py / code_probe.py / nav.py / build.py
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))


def main() -> None:
    ap = argparse.ArgumentParser(description="Build OHAO monograph tree site")
    ap.add_argument(
        "--author",
        action="store_true",
        help="Regenerate site/content/units/**.md from knowledge + code probe",
    )
    args = ap.parse_args()
    if args.author:
        from monograph.author_rich import author_all

        author_all()
    from monograph.build import build

    build()


if __name__ == "__main__":
    main()
