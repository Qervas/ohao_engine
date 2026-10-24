#!/usr/bin/env python3
"""check_layout -- the shared shader/C++ layout (sub-project 2b, step 6).

    python tests/layout/check_layout.py

Last line is a JSON summary; exit 0 = pass.
  s6_* shaders/shared/layout.json's generated files (layout.slang, layout.glsl,
  ohao/gpu/shared_layout.hpp) are current; an edit to layout.json is caught by --check;
  structs follow std430 (offsets and sizes, asserted in the header); a 3-vector, a
  non-scalar constant and a duplicate name are refused; the generated GLSL and Slang compile
  (glslc, slangc) and agree with the header on a sample struct.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_layout  # noqa: E402

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


SAMPLE = {"constants": [{"name": "kN", "type": "uint", "value": 7, "doc": "n"},
                        {"name": "kScale", "type": "float", "value": 0.5, "doc": "s"}],
          "structs": [{"name": "Sample", "doc": "s", "fields": [
              {"name": "a", "type": "uint"}, {"name": "b", "type": "float4"}, {"name": "c", "type": "float2"},
              {"name": "d", "type": "float4x4"}, {"name": "e", "type": "int"}]}]}


def generate(spec, tmp):
    path = os.path.join(tmp, "layout.json")
    with open(path, "w") as f:
        json.dump(spec, f)
    out = {k: os.path.join(tmp, f"layout.{k}") for k in ("slang", "glsl", "hpp")}
    args = ["--layout", path] + [x for k, v in out.items() for x in (f"--{k}", v)]
    return gen_layout.main(args), out, args


def s6_the_generated_files_are_current():
    rc = gen_layout.main(["--check"])
    assert rc == 0, "python tools/gen_layout.py --check: run python tools/gen_layout.py"


def s6_an_edit_to_layout_json_is_caught():
    with tempfile.TemporaryDirectory() as tmp:
        rc, _, args = generate(SAMPLE, tmp)
        assert rc == 0, "the sample generates"
        assert gen_layout.main(args + ["--check"]) == 0, "and is then current"
        spec = json.loads(json.dumps(SAMPLE))
        spec["constants"][0]["value"] = 8
        with open(os.path.join(tmp, "layout.json"), "w") as f:
            json.dump(spec, f)
        assert gen_layout.main(args + ["--check"]) == 1, "a changed value is out of date until regenerated"


def s6_structs_follow_std430_and_the_header_asserts_it():
    fields, size, align = gen_layout.std430(SAMPLE["structs"][0]["fields"])
    offsets = {n: o for n, _, o in fields}
    assert offsets == {"a": 0, "b": 16, "c": 32, "d": 48, "e": 112}, f"std430 offsets: {offsets}"
    assert size == 128 and align == 16, f"size 128 (rounded to 16), alignment 16: {size}, {align}"
    hpp = gen_layout.render_hpp(SAMPLE)
    for n, o in offsets.items():
        assert f"static_assert(offsetof(Sample, {n}) == {o});" in hpp, f"the header asserts {n}'s offset"
    assert "static_assert(sizeof(Sample) == 128);" in hpp and "alignas(16) std::array<float, 4> b" in hpp, hpp


def s6_a_bad_layout_is_refused():
    for bad, why in ((
            {"structs": [{"name": "S", "doc": "", "fields": [{"name": "p", "type": "float3"}]}]}, "use the 4-vector"),
            ({"constants": [{"name": "k", "type": "float4", "value": 1, "doc": ""}]}, "not a scalar"),
            ({"constants": [{"name": "k", "type": "uint", "value": 1, "doc": ""}],
              "structs": [{"name": "k", "doc": "", "fields": [{"name": "x", "type": "uint"}]}]}, "twice")):
        try:
            gen_layout.outputs(bad)
        except gen_layout.LayoutError as e:
            assert why in str(e), f"says why ({why}): {e}"
            continue
        raise AssertionError(f"refused: {bad}")


def compile_ok(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)[-600:]


def s6_the_generated_glsl_and_slang_compile():
    glslc = shutil.which("glslc")
    slangc = None
    for d in sorted(os.listdir("C:/VulkanSDK"), reverse=True) if os.path.isdir("C:/VulkanSDK") else []:
        cand = os.path.join("C:/VulkanSDK", d, "Bin", "slangc.exe")
        if os.path.isfile(cand):
            slangc = cand
            break
    slangc = slangc or shutil.which("slangc")
    assert glslc and slangc, f"glslc ({glslc}) and slangc ({slangc}) found"
    with tempfile.TemporaryDirectory() as tmp:
        shared = os.path.join(tmp, "shared")
        os.makedirs(shared)
        spec_path = os.path.join(tmp, "layout.json")
        with open(spec_path, "w") as f:
            json.dump(SAMPLE, f)
        assert gen_layout.main(["--layout", spec_path, "--slang", os.path.join(shared, "layout.slang"), "--glsl",
                                os.path.join(shared, "layout.glsl"), "--hpp", os.path.join(tmp, "layout.hpp")]) == 0
        comp = os.path.join(tmp, "t.comp")
        with open(comp, "w") as f:
            f.write('#version 460\n#include "shared/layout.glsl"\nlayout(local_size_x = 1) in;\n'
                    'layout(std430, binding = 0) buffer B { Sample s; uint n; };\n'
                    'void main() { n = kN + uint(kScale * 2.0) + s.a; }\n')
        ok, log = compile_ok([glslc, "--target-env=vulkan1.3", "-I", tmp, comp, "-o", os.path.join(tmp, "t.spv")])
        assert ok, f"the GLSL compiles: {log}"
        sl = os.path.join(tmp, "t.slang")
        with open(sl, "w") as f:
            f.write('import shared.layout;\nRWStructuredBuffer<Sample> s;\nRWStructuredBuffer<uint> n;\n'
                    '[shader("compute")]\n[numthreads(1, 1, 1)]\nvoid run(uint3 id : SV_DispatchThreadID)\n'
                    '{ n[0] = kN + uint(kScale * 2.0) + s[0].a; }\n')
        ok, log = compile_ok([slangc, sl, "-target", "spirv", "-profile", "spirv_1_6", "-entry", "run",
                              "-matrix-layout-column-major", "-I", tmp, "-o", os.path.join(tmp, "t2.spv")])
        assert ok, f"the Slang compiles: {log}"


def main():
    print("check_layout (shaders/shared/layout.json)")
    for fn in (s6_the_generated_files_are_current, s6_an_edit_to_layout_json_is_caught,
               s6_structs_follow_std430_and_the_header_asserts_it, s6_a_bad_layout_is_refused,
               s6_the_generated_glsl_and_slang_compile):
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed")
    print(json.dumps({"suite": "check_layout", "passed": passed, "failed": failed, "failures": failures},
                     separators=(",", ":")))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
