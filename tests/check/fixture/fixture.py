"""A fake suite for tools/check.py's own test: prints 200 lines of noise, a marker, and the
shared harness's JSON summary line; `fail` fails one check and exits 1. `noisy_once FILE`
and `noisy_always FILE` play a benchmark under outside load (summary `"noisy": true`, exit
2): the first is noisy only on its first run, the second every time; each appends a line to
FILE per run, so the test can count the attempts."""
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
if mode in ("noisy_once", "noisy_always"):
    counter = sys.argv[2]
    try:
        with open(counter, encoding="utf-8") as f:
            runs = len(f.readlines())
    except OSError:
        runs = 0
    with open(counter, "a", encoding="utf-8") as f:
        f.write("run\n")
    if mode == "noisy_always" or runs == 0:
        print(json.dumps({"suite": "fixture_" + mode, "passed": 1, "failed": 0, "failures": [], "noisy": True},
                         separators=(",", ":")))
        sys.exit(2)
print(json.dumps({"suite": "fixture_pass", "passed": 3, "failed": 0, "failures": []}, separators=(",", ":")))
