"""A fake suite for tools/check.py's own test: prints 200 lines of noise, a marker, and the
shared harness's JSON summary line; `fail` fails one check and exits 1."""
import json
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else "pass"
for i in range(200):
    print(f"noise line {i}")
if mode == "fail":
    print("FIXTURE-MARKER: the check that fails")
    print(json.dumps({"suite": "fixture_fail", "passed": 2, "failed": 1,
                      "failures": ["t_fixture: the check that fails"]}, separators=(",", ":")))
    sys.exit(1)
print(json.dumps({"suite": "fixture_pass", "passed": 3, "failed": 0, "failures": []}, separators=(",", ":")))
