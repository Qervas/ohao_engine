#!/usr/bin/env python3
"""check_tests -- the test of tools/check.py (sub-project 4, test 3). CPU only.

    python tests/check/check_tests.py      last line is a JSON summary; exit 0 = pass

  t3_* run tools/check.py against the selftest fixtures (tests/check/fixture), configured
  into their own build tree, build/check_fixture: a failing test makes the check exit 1
  and reports it with its own summary and the tail of its output; check.json, the last
  stdout line and junit.xml agree; labels select exactly their tests.
  t3_every_test_executable_is_registered: every test executable the main build produced
  (*_test, *_tests, *_bench in build/Release) runs as a CTest test, and every registered
  test carries exactly one known label.
Standard library only.
"""
import importlib.util
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SRC = ROOT / "tests" / "check" / "fixture"
FIXTURE_BUILD = ROOT / "build" / "check_fixture"
MAIN_BUILD = ROOT / "build"

spec = importlib.util.spec_from_file_location("ohao_check", ROOT / "tools" / "check.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

passed, failed, failures = 0, 0, []


def run(fn):
    global passed, failed
    print(f"  {fn.__name__:<62}", end="", flush=True)
    try:
        fn()
    except AssertionError as e:
        failed += 1
        failures.append(f"{fn.__name__}: {e}")
        print(f"FAIL\n    {e}")
        return
    passed += 1
    print("PASS")


def run_check(label):
    out = FIXTURE_BUILD / f"out_{label}"
    args = ("--labels", label)
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "check.py"), "--build-dir", str(FIXTURE_BUILD),
                        "--no-build", "--out", str(out), *args], capture_output=True, text=True, cwd=ROOT)
    return r, out


def configure_fixture():
    cmake = check.find_cmake()
    r = subprocess.run([str(cmake), "-B", str(FIXTURE_BUILD), "-S", str(FIXTURE_SRC)], capture_output=True,
                       text=True, cwd=ROOT)
    assert r.returncode == 0, "the fixture project configures: " + r.stdout[-500:] + r.stderr[-500:]


_selftest = {}


def selftest_run():
    if not _selftest:
        _selftest["r"], _selftest["out"] = run_check("selftest")
    return _selftest["r"], _selftest["out"]


def t3_a_failing_test_fails_the_check():
    r, out = selftest_run()
    assert r.returncode == 1, f"exit code 1 (got {r.returncode}); stderr: {r.stderr[-300:]}"
    verdict = json.loads((out / "check.json").read_text(encoding="utf-8"))
    assert verdict["ok"] is False, "check.json says not ok"
    assert verdict["counts"] == {"tests": 2, "passed": 1, "failed": 1}, f"counts {verdict['counts']}"


def t3_the_failure_carries_its_summary_and_output_tail():
    _, out = selftest_run()
    tests = {t["name"]: t for t in json.loads((out / "check.json").read_text(encoding="utf-8"))["tests"]}
    f = tests.get("fixture_fail")
    assert f is not None and f["ok"] is False, "fixture_fail is reported as failed"
    assert f["passed"] == 2 and f["failed"] == 1, "its own summary line is read (2 passed, 1 failed)"
    assert f["failures"] == ["t_fixture: the check that fails"], f"its failures are listed: {f.get('failures')}"
    assert len(f["tail"]) == check.TAIL_LINES, f"the tail is {check.TAIL_LINES} lines (got {len(f['tail'])})"
    assert "FIXTURE-MARKER: the check that fails" in f["tail"], "the tail holds the output's end"
    p = tests.get("fixture_pass")
    assert p is not None and p["ok"] is True and p["passed"] == 3 and "tail" not in p, \
        "a passing test has its counts and no tail"


def t3_stdout_last_line_is_check_json():
    r, out = selftest_run()
    lines = r.stdout.strip().splitlines()
    assert json.loads(lines[-1]) == json.loads((out / "check.json").read_text(encoding="utf-8")), \
        "the last stdout line is check.json"
    assert not any("noise line" in l for l in lines[:-1]), \
        "a failing test's output stays out of the progress lines (it is in the log and the tail)"
    assert "noise line 199" in (out / "ctest.log").read_text(encoding="utf-8"), "the full output is in ctest.log"


def t3_junit_parses_and_agrees():
    _, out = selftest_run()
    cases = {c.get("name"): c for c in ET.parse(out / "junit.xml").getroot().iter("testcase")}
    assert set(cases) == {"fixture_pass", "fixture_fail"}, f"junit.xml lists the two selftest cases: {sorted(cases)}"
    assert cases["fixture_fail"].get("status") == "fail" and cases["fixture_pass"].get("status") == "run", \
        "junit.xml marks the failure"


def t3_labels_select_exactly_their_tests():
    r, out = run_check("quick")
    verdict = json.loads((out / "check.json").read_text(encoding="utf-8"))
    assert r.returncode == 0 and verdict["ok"] is True, f"the quick fixture passes (exit {r.returncode})"
    assert [t["name"] for t in verdict["tests"]] == ["fixture_other_label"], \
        f"only the quick-labelled test ran: {[t['name'] for t in verdict['tests']]}"


def t3_an_unknown_label_is_an_environment_error():
    r, out = run_check("nonsense")
    verdict = json.loads((out / "check.json").read_text(encoding="utf-8"))
    assert r.returncode == 2 and "unknown label" in verdict.get("error", ""), f"exit 2 with the reason (got {r.returncode})"


def t3_every_test_executable_is_registered():
    ctest = check.find_ctest(check.find_cmake())
    r = subprocess.run([str(ctest), "--test-dir", str(MAIN_BUILD), "-C", "Release", "-N", "--show-only=json-v1"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, "ctest lists the main build's tests"
    registered = json.loads(r.stdout).get("tests", [])
    assert registered, "the main build has tests"
    commands = {Path(t["command"][0]).stem.lower() for t in registered if t.get("command")}
    exes = sorted(p.stem for p in (MAIN_BUILD / "Release").glob("*.exe")
                  if p.stem.endswith(("_test", "_tests", "_bench")))
    assert exes, "the main build produced test executables"
    missing = [e for e in exes if e.lower() not in commands]
    assert not missing, f"test executables no CTest test runs: {missing}"
    bad = []
    for t in registered:
        labels = next((p["value"] for p in t.get("properties", []) if p.get("name") == "LABELS"), [])
        if len(labels) != 1 or labels[0] not in check.KNOWN_LABELS:
            bad.append(f"{t['name']}={labels}")
    assert not bad, f"tests without exactly one known label: {bad}"


def main():
    print("check_tests (tools/check.py)")
    try:
        configure_fixture()
    except AssertionError as e:
        global failed
        failed += 1
        failures.append(f"configure: {e}")
    else:
        for fn in (t3_a_failing_test_fails_the_check, t3_the_failure_carries_its_summary_and_output_tail,
                   t3_stdout_last_line_is_check_json, t3_junit_parses_and_agrees,
                   t3_labels_select_exactly_their_tests, t3_an_unknown_label_is_an_environment_error):
            run(fn)
    run(t3_every_test_executable_is_registered)
    print(f"Results: {passed}/{passed + failed} passed")
    print(json.dumps({"suite": "check_tests", "passed": passed, "failed": failed, "failures": failures},
                     separators=(",", ":")))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
