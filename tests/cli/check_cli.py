"""CLI goldens for the ohao tool (reflection spec, tests 14 and 16).

Runs every case in tests/cli/cases.json twice, from the repo root, each run starting
from an empty build/cli_tmp. A case passes when both runs exit with the expected code,
print the same bytes and write the same files, and those match the checked-in
expectations. See the _comment in cases.json for the fields.

  python tests/cli/check_cli.py [--cpu-only] [--only NAME] [--update]

The last line printed is a JSON summary: {"suite": "cli_tests", "passed", "failed", "failures"}.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CASES = os.path.join(ROOT, "tests", "cli", "cases.json")
EXPECTED = os.path.join(ROOT, "tests", "cli", "expected")
TMP = os.path.join(ROOT, "build", "cli_tmp")
_CONFIG_DIRS = ("", "Release", "RelWithDebInfo", "Debug")


def resolve_exe(name):
    for cfg in _CONFIG_DIRS:
        for sfx in ("", ".exe"):
            cand = os.path.join(ROOT, "build", cfg, name + sfx)
            if os.path.isfile(cand):
                return cand
    raise SystemExit(f"check_cli: no build of '{name}' under build/ (build it first)")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def run_once(case, exe):
    # A fresh build/cli_tmp per case, except where a case reads the one before it.
    if not case["name"].endswith("_back"):
        shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP, exist_ok=True)
    env = dict(os.environ, **case.get("env", {}))
    proc = subprocess.run([exe] + case["argv"][1:], cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=600)
    files = {}
    for path in list(case.get("files", {})) + case.get("outputs", []):
        full = os.path.join(ROOT, path)
        files[path] = read(full) if os.path.isfile(full) else None
    return proc.returncode, proc.stdout, files, proc.stderr


def check(case, exe, update):
    problems = []
    first = run_once(case, exe)
    second = run_once(case, exe)
    code, out, files, err = first
    if code != case["exit"]:
        tail = err.decode("utf-8", "replace").strip().splitlines()[-3:]
        problems.append(f"exit {code}, expected {case['exit']}; stdout {out[:300]!r}; stderr tail {tail}")
    if second[0] != code:
        problems.append(f"exit code differs between runs: {code} vs {second[0]}")
    if second[1] != out and not case.get("timed"):
        problems.append("stdout differs between two runs")
    for needle in case.get("contains", []):
        for i, run in enumerate((first, second)):
            if needle.encode() not in run[1]:
                problems.append(f"run {i + 1}: stdout lacks {needle!r}: {run[1][:300]!r}")
    for path, data in files.items():
        if data is None:
            problems.append(f"{path} was not written")
        elif second[2][path] != data:
            problems.append(f"{path} differs between two runs")
    if case.get("stdout"):
        golden = os.path.join(EXPECTED, case["name"] + ".out")
        if update:
            with open(golden, "wb") as f:
                f.write(out)
        elif not os.path.isfile(golden):
            problems.append(f"no expected stdout {os.path.relpath(golden, ROOT)} (run with --update)")
        elif read(golden) != out:
            problems.append(f"stdout differs from {os.path.relpath(golden, ROOT)}:\n      got      {out[:400]!r}\n"
                            f"      expected {read(golden)[:400]!r}")
    for path, reference in case.get("files", {}).items():
        data = files.get(path)
        if data is None:
            continue
        ref = os.path.join(ROOT, reference)
        if update and reference.startswith("tests/cli/expected/"):
            with open(ref, "wb") as f:
                f.write(data)
        elif not os.path.isfile(ref):
            problems.append(f"no reference {reference}")
        elif read(ref) != data:
            problems.append(f"{path} differs from {reference}")
    return problems


def main(argv):
    cpu_only = "--cpu-only" in argv
    update = "--update" in argv
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    with open(CASES, encoding="utf-8") as f:
        cases = json.load(f)["cases"]
    os.makedirs(EXPECTED, exist_ok=True)
    passed, failures = 0, []
    exes = {}
    # Cases that read what an earlier case wrote run right after it, so --only of the later
    # one also runs the earlier one.
    for i, case in enumerate(cases):
        if only and case["name"] != only and not (i + 1 < len(cases) and cases[i + 1]["name"] == only
                                                    and cases[i + 1]["name"].endswith("_back")):
            continue
        if cpu_only and case.get("gpu"):
            print(f"[SKIP]  {case['name']} (gpu)")
            continue
        name = case["argv"][0]
        exe = exes.setdefault(name, resolve_exe(name))
        problems = check(case, exe, update)
        if problems:
            failures.append(case["name"])
            print(f"[FAIL]  {case['name']}")
            for p in problems:
                print(f"    {p}")
        else:
            passed += 1
            print(f"[PASS]  {case['name']} (exit {case['exit']})")
    print(f"Results: {passed}/{passed + len(failures)} passed")
    print(json.dumps({"suite": "cli_tests", "passed": passed, "failed": len(failures), "failures": failures}))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
