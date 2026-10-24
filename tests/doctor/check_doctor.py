#!/usr/bin/env python3
"""check_doctor -- `ohao doctor` (sub-project 4, test 5).

    python tests/doctor/check_doctor.py          CPU: fake shader trees and the real tree, --cpu-only
    python tests/doctor/check_doctor.py --gpu    also the device checks (a Vulkan device)

Last line is a JSON summary; exit 0 = pass.
  t5_* a fake shader tree (temp directory) with one missing and one stale SPIR-V is
  reported exactly; a SPIR-V is stale when an #include of its source (quoted, relative to
  the including file, or <...> from the include roots), transitively, is newer; an
  unresolved include is named; --cpu-only creates no device and lists no device check;
  every check has the documented shape; the real tree passes.
  s6_* (sub-project 2b, step 6) Slang: a .slang file with [shader(...)] entry points is a
  source, one SPIR-V per entry (<file>.spv for one, <file>.<entry>.spv for several), a file
  without one is a module; an imported module newer than the SPIR-V makes it stale; an
  unresolved import is named; tools.slangc finds slangc at shaders/slangc.version's version.
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OHAO = os.path.join(ROOT, "build", "Release", "ohao.exe" if os.name == "nt" else "ohao")
DEVICE_CHECKS = {"vulkan.instance", "vulkan.device", "vulkan.rt", "vulkan.validation", "gpu.wedged"}
CPU_CHECKS = ["shaders.complete", "shaders.fresh", "assets.present", "tools.glslc", "tools.slangc", "tools.cmake",
              "python.deps"]

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


def doctor(*args):
    r = subprocess.run([OHAO, "doctor", *args], capture_output=True, text=True, cwd=ROOT)
    lines = r.stdout.strip().splitlines()
    assert lines, f"doctor printed nothing (exit {r.returncode}): {r.stderr[-300:]}"
    doc = json.loads(lines[-1])
    assert "result" in doc, f"a result document: {lines[-1][:200]}"
    return r.returncode, doc["result"], {c["id"]: c for c in doc["result"]["checks"]}


def write(path, text, mtime):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)
    os.utime(path, (mtime, mtime))


T0 = 1_700_000_000   # sources at T0, SPIR-V at T0 + 100 unless a case says otherwise


def fake_tree(tmp):
    """shaders/a.vert includes <common.glsl> (in includes/), which includes "detail.glsl";
    shaders/sub/b.frag includes "inc.glsl" (beside it); shaders/c.comp has no SPIR-V."""
    sh, spv = os.path.join(tmp, "shaders"), os.path.join(tmp, "spv")
    write(os.path.join(sh, "a.vert"), '#version 460\n#include <common.glsl>\nvoid main() {}\n', T0)
    write(os.path.join(sh, "includes", "common.glsl"), '#include "detail.glsl"\n', T0)
    write(os.path.join(sh, "includes", "detail.glsl"), '// detail\n', T0)
    write(os.path.join(sh, "sub", "b.frag"), '#version 460\n  #include "inc.glsl"\nvoid main() {}\n', T0)
    write(os.path.join(sh, "sub", "inc.glsl"), '// inc\n', T0)
    write(os.path.join(sh, "c.comp"), '#version 460\nvoid main() {}\n', T0)
    write(os.path.join(sh, "includes", "not_a_stage.glsl"), '// never compiled alone\n', T0)
    write(os.path.join(spv, "a.vert.spv"), "spv", T0 + 100)
    write(os.path.join(spv, "sub_b.frag.spv"), "spv", T0 + 100)
    return sh, spv


def tree_doctor(tmp, sh, spv):
    return doctor("--cpu-only", "--root", tmp, "--shader-dir", sh, "--spirv-dir", spv)


def t5_a_missing_spirv_is_named():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        rc, res, checks = tree_doctor(tmp, sh, spv)
        c = checks["shaders.complete"]
        assert c["ok"] is False and c["level"] == "error", f"shaders.complete fails: {c}"
        assert "1 of 3 SPIR-V missing" in c["detail"] and "c.comp.spv" in c["detail"], f"names c.comp.spv: {c['detail']}"
        assert "a.vert.spv" not in c["detail"] and "sub_b.frag.spv" not in c["detail"], "and only it"
        assert "not_a_stage" not in c["detail"], "a .glsl include is not a stage source"
        assert rc == 1 and res["ok"] is False, f"an error-level failure: exit 1 (got {rc})"
        assert c.get("fix"), "a failing check says how to fix it"


def t5_fresh_spirv_passes_and_a_newer_include_makes_it_stale():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        _, _, checks = tree_doctor(tmp, sh, spv)
        assert checks["shaders.fresh"]["ok"] is True, f"everything older than its SPIR-V: fresh ({checks['shaders.fresh']})"
        os.utime(os.path.join(sh, "sub", "inc.glsl"), (T0 + 200, T0 + 200))   # a quoted include beside b.frag
        _, _, checks = tree_doctor(tmp, sh, spv)
        f = checks["shaders.fresh"]
        assert f["ok"] is False and "sub_b.frag.spv" in f["detail"] and "a.vert.spv" not in f["detail"], \
            f"exactly sub_b.frag.spv is stale: {f['detail']}"


def t5_staleness_follows_includes_transitively():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        os.utime(os.path.join(sh, "includes", "detail.glsl"), (T0 + 200, T0 + 200))   # a.vert -> <common> -> "detail"
        _, _, checks = tree_doctor(tmp, sh, spv)
        f = checks["shaders.fresh"]
        assert f["ok"] is False and "a.vert.spv" in f["detail"] and "sub_b.frag.spv" not in f["detail"], \
            f"exactly a.vert.spv is stale, through two includes: {f['detail']}"


def t5_a_newer_source_is_stale():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        os.utime(os.path.join(sh, "a.vert"), (T0 + 200, T0 + 200))
        _, _, checks = tree_doctor(tmp, sh, spv)
        assert "a.vert.spv" in checks["shaders.fresh"]["detail"], f"{checks['shaders.fresh']}"


def t5_an_unresolved_include_is_named():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        write(os.path.join(sh, "sub", "b.frag"), '#version 460\n#include "inc.glsl"\n#include "gone.glsl"\n', T0)
        _, _, checks = tree_doctor(tmp, sh, spv)
        f = checks["shaders.fresh"]
        assert "gone.glsl" in f["detail"] and "sub/b.frag" in f["detail"], f"names the include and its file: {f['detail']}"


def t5_cpu_only_lists_no_device_check_and_every_check_has_its_shape():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = fake_tree(tmp)
        _, res, checks = tree_doctor(tmp, sh, spv)
        assert res["cpuOnly"] is True, "says it is --cpu-only"
        assert not DEVICE_CHECKS & set(checks), f"no device check: {sorted(DEVICE_CHECKS & set(checks))}"
        assert [c["id"] for c in res["checks"]] == CPU_CHECKS, f"the CPU checks, in order: {[c['id'] for c in res['checks']]}"
        for c in res["checks"]:
            keys = set(c)
            assert {"id", "ok", "level", "detail"} <= keys <= {"id", "ok", "level", "detail", "fix"}, f"shape of {c}"
            assert c["level"] in ("error", "warn") and isinstance(c["ok"], bool), f"types of {c}"
            assert ("fix" in c) == (not c["ok"]), f"fix exactly when failing: {c}"
        assert checks["assets.present"]["ok"] is False, "a root without assets/scenes fails assets.present"


def t5_the_real_tree_passes_cpu_only():
    rc, res, checks = doctor("--cpu-only")
    for cid in ("shaders.complete", "shaders.fresh", "assets.present"):
        assert checks[cid]["ok"] is True, f"{cid}: {checks[cid]}"
    assert rc == 0 and res["ok"] is True, f"exit 0, ok (got {rc}): {[c for c in res['checks'] if not c['ok']]}"


SLANG_ONE = ('import shared.layout;\n[[vk::binding(0, 0)]] RWStructuredBuffer<uint> data;\n'
             '[shader("compute")]\n[numthreads(64, 1, 1)]\nvoid kernel(uint3 id : SV_DispatchThreadID)\n{ data[id.x] = 1; }\n')
SLANG_TWO = ('[shader("compute")]\n[numthreads(64, 1, 1)]\nvoid first(uint3 id : SV_DispatchThreadID) {}\n\n'
             '[shader("compute")]\n[numthreads(64, 1, 1)]\nvoid second(uint3 id : SV_DispatchThreadID) {}\n')


def slang_tree(tmp):
    """bench/k.slang (one entry, imports shared.layout), cull/two.slang (two entries),
    shared/layout.slang (a module: no entry point). SPIR-V for k and for two's first entry."""
    sh, spv = os.path.join(tmp, "shaders"), os.path.join(tmp, "spv")
    write(os.path.join(sh, "bench", "k.slang"), SLANG_ONE, T0)
    write(os.path.join(sh, "cull", "two.slang"), SLANG_TWO, T0)
    write(os.path.join(sh, "shared", "layout.slang"), "static const uint kX = 1u;\n", T0)
    write(os.path.join(spv, "bench_k.slang.spv"), "spv", T0 + 100)
    write(os.path.join(spv, "cull_two.slang.first.spv"), "spv", T0 + 100)
    return sh, spv


def s6_slang_sources_are_files_with_entry_points_named_per_entry():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = slang_tree(tmp)
        _, _, checks = tree_doctor(tmp, sh, spv)
        c = checks["shaders.complete"]
        assert c["ok"] is False and "1 of 3 SPIR-V missing" in c["detail"], f"3 SPIR-V expected, 1 missing: {c}"
        assert "cull_two.slang.second.spv" in c["detail"], f"a second entry point is <file>.<entry>.spv: {c['detail']}"
        assert "bench_k" not in c["detail"] and "layout" not in c["detail"], \
            f"a single entry is <file>.spv; a module is no source: {c['detail']}"


def s6_an_imported_module_newer_than_the_spirv_makes_it_stale():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = slang_tree(tmp)
        _, _, checks = tree_doctor(tmp, sh, spv)
        assert checks["shaders.fresh"]["ok"] is True, f"fresh at first: {checks['shaders.fresh']}"
        os.utime(os.path.join(sh, "shared", "layout.slang"), (T0 + 200, T0 + 200))
        _, _, checks = tree_doctor(tmp, sh, spv)
        f = checks["shaders.fresh"]
        assert f["ok"] is False and "bench_k.slang.spv" in f["detail"] and "cull_two" not in f["detail"], \
            f"exactly the importer is stale: {f['detail']}"


def s6_an_unresolved_import_is_named():
    with tempfile.TemporaryDirectory() as tmp:
        sh, spv = slang_tree(tmp)
        write(os.path.join(sh, "bench", "k.slang"), "import shared.gone;\n" + SLANG_ONE, T0)
        _, _, checks = tree_doctor(tmp, sh, spv)
        f = checks["shaders.fresh"]
        assert "shared/gone.slang" in f["detail"] and "bench/k.slang" in f["detail"], f"names it: {f['detail']}"


def s6_tools_slangc_is_found_at_the_pinned_version():
    with open(os.path.join(ROOT, "shaders", "slangc.version")) as f:
        pinned = f.readline().strip()
    _, _, checks = doctor("--cpu-only")
    c = checks["tools.slangc"]
    assert c["ok"] is True and c["level"] == "error", f"tools.slangc passes: {c}"
    assert f"({pinned}" in c["detail"], f"reports the pinned version {pinned}: {c['detail']}"


def t5_device_checks_pass_on_this_machine():
    rc, res, checks = doctor()
    assert res["cpuOnly"] is False, "not --cpu-only"
    for cid in sorted(DEVICE_CHECKS):
        assert cid in checks, f"{cid} listed"
    for cid in ("vulkan.instance", "vulkan.device", "gpu.wedged"):
        assert checks[cid]["ok"] is True, f"{cid}: {checks[cid]}"
    assert rc == 0 and res["ok"] is True, f"exit 0 (got {rc})"


def main():
    gpu = "--gpu" in sys.argv
    print("check_doctor (ohao doctor)" + (" --gpu" if gpu else ""))
    if not os.path.isfile(OHAO):
        print(f"  {OHAO} not built")
        tests = []
        global failed
        failed += 1
        failures.append("ohao not built")
    elif gpu:
        tests = [t5_device_checks_pass_on_this_machine]
    else:
        tests = [t5_a_missing_spirv_is_named, t5_fresh_spirv_passes_and_a_newer_include_makes_it_stale,
                 t5_staleness_follows_includes_transitively, t5_a_newer_source_is_stale,
                 t5_an_unresolved_include_is_named, t5_cpu_only_lists_no_device_check_and_every_check_has_its_shape,
                 t5_the_real_tree_passes_cpu_only, s6_slang_sources_are_files_with_entry_points_named_per_entry,
                 s6_an_imported_module_newer_than_the_spirv_makes_it_stale, s6_an_unresolved_import_is_named,
                 s6_tools_slangc_is_found_at_the_pinned_version]
    for fn in tests:
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed")
    print(json.dumps({"suite": "check_doctor_gpu" if gpu else "check_doctor", "passed": passed, "failed": failed,
                      "failures": failures}, separators=(",", ":")))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
