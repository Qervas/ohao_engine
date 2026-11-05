#!/usr/bin/env python3
"""check_tests -- the test of tools/check.py (sub-project 4, test 3). CPU only.

    python tests/check/check_tests.py      last line is a JSON summary; exit 0 = pass

  t3_* run tools/check.py against the selftest fixtures (tests/check/fixture), configured
  into their own build tree, build/check_fixture: a failing test makes the check exit 1
  and reports it with its own summary and the tail of its output; check.json, the last
  stdout line and junit.xml agree; labels select exactly their tests; a bench that reports
  itself noisy is rerun at most twice, every attempt reported.
  t3_every_test_executable_is_registered: every test executable the main build produced
  (*_test, *_tests, *_bench in build/Release) runs as a CTest test, and every registered
  test carries exactly one known label.
  changed_*: tools/check_changed.py, the rules behind check.py --changed: every rule names a
  registered test or label; a saved component's file selects the schema's suites; a renderer file
  the GPU label; a spec selects nothing; a path no rule covers selects the whole default set.
  sp2b_t02_* (sub-project 2b, test 2): render_bench --evaluate on fixture results
  (tests/check/render_bench): a pending budget is reported with its measurement and not
  gated; a missed gated budget fails by scene and pass name; a noisy run (busy CPU or a slow
  GPU calibration) exits 2 and counts nothing as passed.
Standard library only.
"""
import importlib.util
import json
import os
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


def t3_a_noisy_bench_is_rerun_at_most_twice():
    """Spec "Measurement rules": the check reruns a noisy bench at most twice and reports
    every attempt (sub-project 4, step 6)."""
    for name in ("noisy_once.runs", "noisy_always.runs"):
        (FIXTURE_BUILD / name).unlink(missing_ok=True)
    r, out = run_check("bench")
    verdict = json.loads((out / "check.json").read_text(encoding="utf-8"))
    tests = {t["name"]: t for t in verdict["tests"]}
    once, always = tests.get("fixture_noisy_once"), tests.get("fixture_noisy_always")
    assert once is not None and always is not None, f"both noisy fixtures are reported: {sorted(tests)}"
    assert once["ok"] is True and "noisy" not in once, "noisy once, then a clean pass: the test passes"
    assert [(a["ok"], a["noisy"]) for a in once["attempts"]] == [(False, True), (True, False)], \
        f"its two attempts are reported: {once.get('attempts')}"
    runs = len((FIXTURE_BUILD / "noisy_always.runs").read_text(encoding="utf-8").splitlines())
    assert runs == 3, f"an always-noisy bench runs three times in all, the first plus two reruns (ran {runs})"
    assert always["ok"] is False and always.get("noisy") is True, "still noisy: not passed, and marked noisy"
    assert [(a["ok"], a["noisy"]) for a in always["attempts"]] == [(False, True)] * 3, \
        f"its three attempts are reported: {always.get('attempts')}"
    assert r.returncode == 1 and verdict["counts"] == {"tests": 2, "passed": 1, "failed": 1}, \
        f"exit 1, one of two passed (exit {r.returncode}, counts {verdict['counts']})"


def t3_an_unknown_label_is_an_environment_error():
    r, out = run_check("nonsense")
    verdict = json.loads((out / "check.json").read_text(encoding="utf-8"))
    assert r.returncode == 2 and "unknown label" in verdict.get("error", ""), f"exit 2 with the reason (got {r.returncode})"


# ---- render_bench's verdict (sub-project 2b, test 2) -----------------------------------------
# render_bench --evaluate applies budgets to recorded results without a GPU; variants of the
# fixture (tests/check/render_bench) are written under build/check_fixture/render_bench.

RB_FIXTURE = ROOT / "tests" / "check" / "render_bench"
RB_SCRATCH = FIXTURE_BUILD / "render_bench"
RENDER_BENCH = MAIN_BUILD / "Release" / ("render_bench.exe" if os.name == "nt" else "render_bench")


def evaluate_render_bench(name, results_edit=None, budgets_edit=None):
    """Runs render_bench --evaluate on the fixture, edited; returns (exit code, summary, out json)."""
    assert RENDER_BENCH.exists(), f"render_bench is built ({RENDER_BENCH})"
    RB_SCRATCH.mkdir(parents=True, exist_ok=True)
    results = json.loads((RB_FIXTURE / "results.json").read_text(encoding="utf-8"))
    budgets = json.loads((RB_FIXTURE / "budgets.json").read_text(encoding="utf-8"))
    if results_edit:
        results_edit(results)
    if budgets_edit:
        budgets_edit(budgets)
    rpath, bpath, opath = (RB_SCRATCH / f"{name}.{k}.json" for k in ("results", "budgets", "out"))
    rpath.write_text(json.dumps(results), encoding="utf-8")
    bpath.write_text(json.dumps(budgets), encoding="utf-8")
    r = subprocess.run([str(RENDER_BENCH), "--evaluate", str(rpath), "--check", str(bpath), "--out", str(opath)],
                       capture_output=True, text=True, cwd=ROOT)
    lines = r.stdout.strip().splitlines()
    summary = json.loads(lines[-1]) if lines else {}
    out = json.loads(opath.read_text(encoding="utf-8")) if opath.exists() else {}
    return r.returncode, summary, out


def sp2b_t02_a_pending_budget_is_reported_not_gated():
    code, summary, out = evaluate_render_bench("pending")
    status = {b["name"] + "@" + b["scene"]: b for b in out.get("budgets", [])}
    assert code == 0 and summary.get("passed") == 2 and summary.get("failed") == 0, \
        f"gbuffer and post are gated and met; nothing else counts (exit {code}, {summary})"
    gi = status.get("rt.gi@cornell_720p", {})
    assert gi.get("status") == "pending" and abs(gi.get("measured", 0) - 3.956) < 1e-3, \
        f"rt.gi, over its budget but gated from step 5, is pending with its measurement: {gi}"
    assert status.get("frame@bench100k_1080p", {}).get("status") == "unmeasured", \
        "a pending budget whose scene is not measured yet is reported as unmeasured, not failed"
    scene = out["scenes"][0]
    assert scene["frame"].get("budget") == 1.5 and \
        next(p for p in scene["passes"] if p["name"] == "rt.gi").get("budget") == 0.3, \
        "the scenes carry their frame and single-pass budgets (the spec's results shape)"


def sp2b_t02_a_missed_gated_budget_names_its_pass():
    code, summary, out = evaluate_render_bench("gated", budgets_edit=lambda b: b.update(currentStep=5))
    fails = summary.get("failures", [])
    assert code == 1 and summary.get("passed") == 2 and summary.get("failed") == 2, f"exit 1, two missed ({code}, {summary})"
    assert any(f.startswith("cornell_720p rt.gi:") and "3.956" in f and "0.300" in f for f in fails), \
        f"the miss names the scene, the pass, the median and the budget: {fails}"
    assert any("bench100k_1080p frame" in f and "not measured" in f for f in fails), \
        f"a gated budget that was not measured is a failure, by name: {fails}"
    assert out.get("verdict") == "over", "the results file says over"


def sp2b_t02_a_noisy_run_is_never_a_pass():
    def busy(r):
        r["externalLoad"] = {"totalCorePct": 80.0, "busy": [{"name": "UnrealEditor.exe", "corePct": 75.0}]}
    code, summary, out = evaluate_render_bench("noisy_load", results_edit=busy)
    assert code == 2 and summary.get("noisy") is True and summary.get("passed") == 0, \
        f"other processes over half a core: exit 2, noisy, nothing counted as passed ({code}, {summary})"
    assert all(b["status"] in ("noisy", "pending", "unmeasured") for b in out.get("budgets", [])), \
        "the gated budgets are marked noisy, not pass"

    def slow_gpu(r):
        r["calibration"]["gpu"]["afterMs"] = 2.5
    code, summary, _ = evaluate_render_bench("noisy_gpu", results_edit=slow_gpu)
    assert code == 2 and summary.get("noisy") is True and \
        any("GPU calibration" in f for f in summary.get("failures", [])), \
        f"a GPU calibration over its reference by more than 25% is noisy, and says so ({code}, {summary})"
    code, summary, _ = evaluate_render_bench("noisy_gated", results_edit=busy,
                                             budgets_edit=lambda b: b.update(currentStep=5))
    assert code == 2 and summary.get("passed") == 0, \
        f"noisy wins over a missed budget: exit 2, still nothing passed ({code}, {summary})"


def t3_every_test_executable_is_registered():
    ctest = check.find_ctest(check.find_cmake())
    r = subprocess.run([str(ctest), "--test-dir", str(MAIN_BUILD), "-C", "Release", "-N", "--show-only=json-v1"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, "ctest lists the main build's tests"
    registered = json.loads(r.stdout).get("tests", [])
    assert registered, "the main build has tests"
    commands = {Path(t["command"][0]).stem.lower() for t in registered if t.get("command")}
    # The built executables: Windows names them .exe; elsewhere they are the executable files without a suffix.
    built = ((MAIN_BUILD / "Release").glob("*.exe") if os.name == "nt" else
             (p for p in (MAIN_BUILD / "Release").iterdir() if p.is_file() and not p.suffix and os.access(p, os.X_OK)))
    exes = sorted(p.stem for p in built if p.stem.endswith(("_test", "_tests", "_bench")))
    assert exes, "the main build produced test executables"
    missing = [e for e in exes if e.lower() not in commands]
    assert not missing, f"test executables no CTest test runs: {missing}"
    bad = []
    for t in registered:
        labels = next((p["value"] for p in t.get("properties", []) if p.get("name") == "LABELS"), [])
        if len(labels) != 1 or labels[0] not in check.KNOWN_LABELS:
            bad.append(f"{t['name']}={labels}")
    assert not bad, f"tests without exactly one known label: {bad}"


def main_labels():
    ctest = check.find_ctest(check.find_cmake())
    return check.test_labels(ctest, MAIN_BUILD, "Release")


def changed_rules():
    sys.path.insert(0, str(ROOT / "tools"))
    import check_changed
    return check_changed


def changed_every_rule_names_a_test():
    cc, labels = changed_rules(), main_labels()
    known = {l for ls in labels.values() for l in ls}
    bad = sorted({t for _, ts in cc.RULES for t in ts
                  if (t[len("label:"):] not in known if t.startswith("label:") else t not in labels)})
    assert not bad, f"rule targets that are no registered test or label: {bad}"


def changed_selections_follow_reach():
    cc, labels = changed_rules(), main_labels()
    gpu = {n for n, ls in labels.items() if "gpu" in ls}
    default = {n for n, ls in labels.items() if any(l in check.DEFAULT_LABELS for l in ls)}
    names, _ = cc.select(["ohao/ecs/components.hpp"], labels, check.DEFAULT_LABELS)
    assert {"scene_tests", "reflect_tests", "check_serve", "check_cli"} <= set(names), \
        f"a saved component reaches the schema's suites: {names}"
    names, _ = cc.select(["ohao/render/rt/path_tracer.cpp"], labels, check.DEFAULT_LABELS)
    assert gpu <= set(names), f"a renderer file reaches every GPU suite: missing {sorted(gpu - set(names))}"
    names, _ = cc.select(["docs/specs/x.md", "site/a.html"], labels, check.DEFAULT_LABELS)
    assert names == [], f"a spec and the frozen site reach nothing: {names}"
    names, why = cc.select(["somewhere/new.cpp"], labels, check.DEFAULT_LABELS)
    assert why["somewhere/new.cpp"] == "unmapped" and default <= set(names), \
        "a path no rule covers runs the default set"


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
                   t3_labels_select_exactly_their_tests, t3_a_noisy_bench_is_rerun_at_most_twice,
                   t3_an_unknown_label_is_an_environment_error):
            run(fn)
    run(t3_every_test_executable_is_registered)
    run(changed_every_rule_names_a_test)
    run(changed_selections_follow_reach)
    for fn in (sp2b_t02_a_pending_budget_is_reported_not_gated, sp2b_t02_a_missed_gated_budget_names_its_pass,
               sp2b_t02_a_noisy_run_is_never_a_pass):
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed")
    print(json.dumps({"suite": "check_tests", "passed": passed, "failed": failed, "failures": failures},
                     separators=(",", ":")))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
