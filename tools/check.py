#!/usr/bin/env python3
"""check.py -- build OHAO and run its tests from one command, with one verdict.

    python tools/check.py [--labels quick,gpu] [--all] [--changed [REF]] [--jobs 6] [--config Release]
                          [--no-build] [--no-doctor] [--build-dir build] [--out DIR] [--ctest-jobs 4]

--changed [REF] runs only the tests the files changed since REF (default HEAD: the working tree,
staged or not, and untracked files) reach, by tools/check_changed.py's rules; a path no rule covers
runs the default labels. For iterating; close a build step with the full run.

Steps (sub-project 4 spec, "One entry point"):
  1. preflight: the CMake to use (never conda's) and the Python modules the harnesses need;
  2. configure, if the build directory has no CMakeCache.txt;
  3. build the `shaders` target first, then everything (at most 6 jobs: heavier parallel
     load has crashed the reference machine);
  4. `ohao doctor` (--cpu-only unless a gpu or golden label is chosen): when an
     error-level check fails the run stops there, with the doctor's verdict in check.json;
  5. ctest with the chosen labels: JUnit XML, output on failure, an error if no test matches;
  6. the kernel log read for NVIDIA GPU faults (Xid) during the tests: any fails the run
     (gpuFaults lists them; "unknown" where the log cannot be read);
  7. the verdict: <out>/check.json, also printed as the last line of stdout.

check.json: {ok, labels, build: {ok, seconds, tail?}, doctor, tests: [{name, labels, ok,
seconds, passed?, failed?, failures?, tail?}], counts: {tests, passed, failed}, gpuFaults}. passed /
failed / failures come from the suite's own JSON summary line; tail is the last 40 lines of a
failing test's output, so no failure is ever reported without what it printed.

Labels (cmake/ohao_tests.cmake): quick and gpu (the default set); --all adds golden and slow.

Exit code: 0 when the build and every selected test pass, 1 otherwise, 2 on a usage or
environment error (wrong cmake, missing module).
Test-only Python (standard library only); never on an engine path.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LABELS = ["quick", "gpu"]
ALL_LABELS = DEFAULT_LABELS + ["golden", "slow"]
KNOWN_LABELS = ALL_LABELS
MAX_JOBS = int(os.environ.get("OHAO_MAX_JOBS", "6"))
TAIL_LINES = 40
# Modules the harnesses import, by the label whose tests need them.
MODULES_BY_LABEL = {"quick": ["jsonschema", "numpy", "PIL"], "golden": ["numpy", "PIL"], "gpu": ["numpy", "PIL"]}
GPU_LABELS = {"gpu", "golden"}


class EnvError(Exception):
    """A problem with the environment, not with OHAO: exit code 2."""


def is_conda(path):
    p = str(path).lower().replace("\\", "/")
    return "conda" in p or "/envs/" in p


def find_cmake():
    """OHAO_CMAKE, else the scoop shim, else PATH; conda's cmake is refused (it shadows
    the real one on PATH on the reference machine and fails the configure)."""
    candidates = []
    if os.environ.get("OHAO_CMAKE"):
        candidates.append(Path(os.environ["OHAO_CMAKE"]))
    for name in ("cmake.exe", "cmake"):
        candidates.append(Path.home() / "scoop" / "shims" / name)
    found = shutil.which("cmake")
    if found:
        candidates.append(Path(found))
    for c in candidates:
        if c.is_file():
            if is_conda(c):
                raise EnvError(f"{c} is conda's cmake, which shadows the real one on PATH. "
                               "Set OHAO_CMAKE to the real cmake (for example the scoop shim).")
            return c
    raise EnvError("no cmake found: install CMake 3.21+ or set OHAO_CMAKE")


def find_ctest(cmake):
    for name in ("ctest.exe", "ctest"):
        c = cmake.parent / name
        if c.is_file():
            return c
    found = shutil.which("ctest")
    if found and not is_conda(found):
        return Path(found)
    raise EnvError(f"no ctest next to {cmake} or on PATH")


def cache_value(build_dir, key):
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    return None


def check_modules(python, labels):
    needed = sorted({m for lab in labels for m in MODULES_BY_LABEL.get(lab, [])})
    if not needed:
        return
    missing = []
    for m in needed:
        r = subprocess.run([python, "-c", f"import {m}"], capture_output=True)
        if r.returncode != 0:
            missing.append(m)
    if missing:
        pip = {"PIL": "Pillow"}
        raise EnvError(f"{python} lacks {', '.join(missing)}: pip install "
                       + " ".join(pip.get(m, m) for m in missing))


def tail(text, n=TAIL_LINES):
    lines = (text or "").rstrip("\n").splitlines()
    return lines[-n:]


def suite_line(output):
    """The suite's own JSON summary: the last line that parses as {"suite": ...}."""
    for line in reversed((output or "").splitlines()):
        line = line.strip()
        if line.startswith("{") and '"suite"' in line:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and "suite" in obj:
                return obj
    return None


PROGRESS = re.compile(r"^\s*\d+/\d+ Test\s+#\d+:|^\d+% tests passed")


def run_logged(cmd, log, echo):
    """Runs cmd, writing its output to `log`; returns (rc, text). With echo, CTest's
    one-line-per-test progress is copied to stdout (the failing tests' output stays in the
    log and their tail goes into check.json, so stdout stays short for an agent)."""
    chunks = []
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace")
        for line in proc.stdout:
            f.write(line)
            chunks.append(line)
            if echo and PROGRESS.match(line):
                sys.stdout.write(line)
                sys.stdout.flush()
        rc = proc.wait()
    return rc, "".join(chunks)


def build(cmake, build_dir, config, jobs, out):
    t0 = time.perf_counter()
    # Shaders first: a green build must never run stale SPIR-V.
    for target, log in ((["--target", "shaders"], "build_shaders.log"), ([], "build.log")):
        rc, text = run_logged([str(cmake), "--build", str(build_dir), "--config", config,
                               "--parallel", str(jobs)] + target, out / log, False)
        if rc != 0:
            return {"ok": False, "seconds": round(time.perf_counter() - t0, 1), "log": log, "tail": tail(text)}
    return {"ok": True, "seconds": round(time.perf_counter() - t0, 1)}


def ohao_exe(build_dir, config):
    for c in (build_dir / config / "ohao.exe", build_dir / config / "ohao", build_dir / "ohao.exe"):
        if c.is_file():
            return c
    return None


def run_doctor(build_dir, config, labels, out):
    """`ohao doctor` (--cpu-only unless a GPU label is selected) -> its verdict, or None
    when this build has no ohao executable (the check's own fixtures)."""
    exe = ohao_exe(build_dir, config)
    if exe is None:
        return None
    cmd = [str(exe), "doctor"] + ([] if GPU_LABELS & set(labels) else ["--cpu-only"])
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    (out / "doctor.log").write_text(r.stderr, encoding="utf-8")
    try:
        verdict = json.loads(r.stdout.strip().splitlines()[-1])["result"]
    except (IndexError, KeyError, ValueError):
        verdict = {"ok": False, "checks": [], "error": f"ohao doctor printed no verdict (exit {r.returncode})"}
    return verdict


def test_labels(ctest, build_dir, config):
    r = subprocess.run([str(ctest), "--test-dir", str(build_dir), "-C", config, "-N",
                        "--show-only=json-v1"], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        return {}
    labels = {}
    for t in json.loads(r.stdout).get("tests", []):
        labs = []
        for p in t.get("properties", []):
            if p.get("name") == "LABELS":
                labs = p.get("value", [])
        labels[t["name"]] = labs
    return labels


def summarize(junit, labels):
    tests = []
    if not junit.is_file():
        return tests
    for case in ET.parse(junit).getroot().iter("testcase"):
        name = case.get("name")
        status = case.get("status", "run")
        failed = status != "run" or case.find("failure") is not None or case.find("error") is not None
        output = "".join(case.find("system-out").itertext()) if case.find("system-out") is not None else ""
        entry = {"name": name, "labels": labels.get(name, []), "ok": not failed,
                 "seconds": round(float(case.get("time") or 0.0), 2)}
        s = suite_line(output)
        if s is not None:
            entry["passed"] = s.get("passed")
            entry["failed"] = s.get("failed")
            if s.get("failures"):
                entry["failures"] = s["failures"][:20]
        if failed:
            entry["status"] = status
            entry["tail"] = tail(output)
        tests.append(entry)
    return tests


# The summary line is a suite's last: keep the end of long outputs, not the start.
OUTPUT_FLAGS = ["--output-on-failure", "--test-output-size-passed", "65536",
                "--test-output-size-failed", "1048576", "--test-output-truncation", "head"]

def gpu_faults_since(t0):
    """NVIDIA's GPU faults (Xid lines) the kernel logged since t0 (epoch seconds), or None when the
    kernel log cannot be read (not Linux, no journalctl, no permission). A test can pass while the GPU
    faults under it (a hung channel the driver recovered, a firmware error before a crash), so a run
    with any fault fails whatever the tests said."""
    if not sys.platform.startswith("linux") or shutil.which("journalctl") is None:
        return None
    r = subprocess.run(["journalctl", "-k", "--no-pager", "-o", "short-iso", "--since", "@%d" % int(t0)],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        return None
    return [line for line in r.stdout.splitlines() if "NVRM: Xid" in line]


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build OHAO and run its tests; one JSON verdict.")
    ap.add_argument("--labels", default=",".join(DEFAULT_LABELS),
                    help="comma-separated CTest labels (default: quick,gpu)")
    ap.add_argument("--all", action="store_true", help="every label")
    ap.add_argument("--jobs", type=int, default=MAX_JOBS, help=f"build parallelism (at most {MAX_JOBS})")
    ap.add_argument("--ctest-jobs", type=int, default=4, help="tests run at once (GPU tests never overlap)")
    ap.add_argument("--config", default="Release")
    ap.add_argument("--no-build", action="store_true", help="test what is built")
    ap.add_argument("--no-doctor", action="store_true", help="skip `ohao doctor`")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--out", default=None, help="where check.json, junit.xml and logs go (default <build>/check)")
    ap.add_argument("--changed", nargs="?", const="HEAD", default=None, metavar="REF",
                    help="only the tests the files changed since REF (default HEAD) reach (tools/check_changed.py)")
    args = ap.parse_args(argv)

    labels = ALL_LABELS if args.all else [l for l in args.labels.split(",") if l]
    unknown = [l for l in labels if l not in KNOWN_LABELS]
    build_dir = (ROOT / args.build_dir).resolve()
    out = Path(args.out).resolve() if args.out else build_dir / "check"
    out.mkdir(parents=True, exist_ok=True)
    jobs = max(1, min(args.jobs, MAX_JOBS))

    result = {"ok": False, "labels": labels, "build": None, "doctor": None, "tests": [],
              "counts": {"tests": 0, "passed": 0, "failed": 0}}
    try:
        if unknown:
            raise EnvError(f"unknown label(s) {', '.join(unknown)}; known: {', '.join(KNOWN_LABELS)}")
        cmake = find_cmake()
        ctest = find_ctest(cmake)
        if not (build_dir / "CMakeCache.txt").is_file():
            if args.no_build:
                raise EnvError(f"{build_dir} is not configured (no CMakeCache.txt) and --no-build was given")
            rc, text = run_logged([str(cmake), "-B", str(build_dir), "-S", str(ROOT)], out / "configure.log", False)
            if rc != 0:
                result["build"] = {"ok": False, "seconds": 0.0, "tail": tail(text)}
                return finish(result, out)
        # The interpreter CTest runs the harnesses with (FindPython keeps it internal).
        python = (cache_value(build_dir, "Python3_EXECUTABLE") or cache_value(build_dir, "_Python3_EXECUTABLE")
                  or sys.executable)
        check_modules(python, labels)
    except EnvError as e:
        result["error"] = str(e)
        print(f"check: {e}", file=sys.stderr)
        return finish(result, out, code=2)

    if args.no_build:
        result["build"] = {"ok": True, "skipped": True}
    else:
        print(f"check: building ({args.config}, {jobs} jobs; shaders first) ...", flush=True)
        result["build"] = build(cmake, build_dir, args.config, jobs, out)
        print(f"check: build {'ok' if result['build']['ok'] else 'FAILED'} in {result['build']['seconds']} s", flush=True)
        if not result["build"]["ok"]:
            return finish(result, out)

    selected = None
    if args.changed is not None:
        sys.path.insert(0, str(ROOT / "tools"))
        import check_changed
        labels_of = test_labels(ctest, build_dir, args.config)
        try:
            paths = check_changed.changed_paths(ROOT, args.changed)
        except RuntimeError as e:
            result["error"] = str(e)
            return finish(result, out, code=2)
        selected, why = check_changed.select(paths, labels_of, DEFAULT_LABELS, ROOT)
        result["changed"] = {"ref": args.changed, "paths": why, "tests": selected}
        for p, t in why.items():
            print(f"check: changed {p} -> {'every default test (no rule)' if t == 'unmapped' else ', '.join(t) or 'nothing'}",
                  flush=True)
        print(f"check: {len(selected)} test(s): {' '.join(selected) or '(none)'}", flush=True)
        if not selected:
            result["ok"] = result["build"]["ok"]
            return finish(result, out)
        labels = sorted({l for n in selected for l in labels_of.get(n, [])})   # the doctor's GPU check follows them
        result["labels"] = labels

    if not args.no_doctor:
        result["doctor"] = run_doctor(build_dir, args.config, labels, out)
        if result["doctor"] is not None:
            bad = [c for c in result["doctor"]["checks"] if not c["ok"]]
            print(f"check: doctor {'ok' if result['doctor']['ok'] else 'FAILED'}"
                  + (f" ({', '.join(c['id'] + ' ' + c['level'] for c in bad)})" if bad else ""), flush=True)
            if not result["doctor"]["ok"]:
                return finish(result, out)   # the environment cannot run the tests: say so, run nothing

    junit = out / "junit.xml"
    if junit.exists():
        junit.unlink()
    if selected is not None:
        regex = "^(" + "|".join(re.escape(n) for n in selected) + ")$"
        select_args = ["-R", regex]
    else:
        regex = "^(" + "|".join(labels) + ")$"
        select_args = ["-L", regex]
    cmd = [str(ctest), "--test-dir", str(build_dir), "-C", args.config, *select_args,
           "--output-junit", str(junit), "--no-tests=error", "-j", str(max(1, args.ctest_jobs))] + OUTPUT_FLAGS
    print(f"check: ctest {' '.join(select_args[:1])} '{regex}'", flush=True)
    started = time.time() - 1.0
    run_logged(cmd, out / "ctest.log", True)
    labels_of = test_labels(ctest, build_dir, args.config)
    result["tests"] = summarize(junit, labels_of)
    n = len(result["tests"])
    passed = sum(1 for t in result["tests"] if t["ok"])
    result["counts"] = {"tests": n, "passed": passed, "failed": n - passed}
    faults = gpu_faults_since(started)
    result["gpuFaults"] = faults if faults is not None else "unknown: the kernel log could not be read"
    if faults:
        print(f"check: {len(faults)} GPU fault(s) in the kernel log during the run:", flush=True)
        for line in faults[:20]:
            print("  " + line, flush=True)
    result["ok"] = result["build"]["ok"] and n > 0 and passed == n and not faults
    return finish(result, out)


def finish(result, out, code=None):
    text = json.dumps(result, separators=(",", ":"))
    (out / "check.json").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    if code is not None:
        return code
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
