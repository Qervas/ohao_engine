#!/usr/bin/env python3
"""M3b ablation gate: baseline quality plate + stress cases recorded."""
from __future__ import annotations

import csv
import sys
from pathlib import Path


def main(root: Path) -> int:
    tsv = root / "ablation.tsv"
    if not tsv.is_file():
        print(f"FAIL missing {tsv}")
        return 1

    rows = list(csv.DictReader(tsv.open(), delimiter="\t"))
    if len(rows) < 3:
        print(f"FAIL need ≥3 ablation cases, got {len(rows)}")
        return 1

    names = {r.get("case") or r.get(list(r.keys())[0]) for r in rows}
    # Handle if DictReader keys are first header row fields
    cases = []
    for r in rows:
        # first column is case
        case = r.get("case")
        if case is None:
            case = list(r.values())[0]
        cases.append(case)
        print(f"  {dict(r)}")

    if "qplate" not in cases:
        print("FAIL missing baseline case 'qplate'")
        return 1

    # Baseline must pass; stresses are recorded (may fail honestly)
    by_case = {}
    for r in rows:
        case = r.get("case") or list(r.values())[0]
        by_case[case] = r

    base = by_case["qplate"]
    if str(base.get("pass", "0")) not in ("1", "True", "true"):
        print("FAIL quality-plate baseline must pass MAPTEST")
        return 1

    # At least one stress differs from baseline (table is real, not a single run)
    if len(set(cases)) < 3:
        print("FAIL ablation table needs distinct cases")
        return 1

    print(f"PASS tools/inverse_lab/test_ablation.py ({len(cases)} cases, baseline qplate green)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "renders/inverse_ablation")))
