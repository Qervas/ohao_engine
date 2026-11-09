#!/usr/bin/env python3
"""gen_bindings -- every shader's bindings for C++, from Slang's reflection (system design 4.2, step 2e).

    python tools/gen_bindings.py            regenerate ohao/render/shader_bindings/*.hpp
    python tools/gen_bindings.py --check    exit 1 (naming the files) if any is out of date or missing

For each shader file with an entry point (or a ray tracing library), slangc's -reflection-json gives its
parameters; the header, ohao/render/shader_bindings/<flattened path>.hpp, namespace ohao::shader::<flattened
path>, holds one `Binding` per parameter (set, binding, descriptor type, count), each set's bindings in order
(`kSet<N>`, what a descriptor set layout is made from), the stages the file's entry points run in (`kStages`),
and the push-constant struct with every offset and the size static_asserted. C++ names these; it never types a
binding number or a push-constant layout again.

slangc is the pinned one (shaders/slangc.version), found as the build finds it: $SLANGC, then
~/.local/opt/slang-<pin>/bin, then PATH.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(ROOT, "shaders")
OUT = os.path.join(ROOT, "ohao", "render", "shader_bindings")
SKIP_DIRS = {"shared", "includes"}   # modules: no entry point, nothing bound

STAGE_BITS = {
    "vertex": "VK_SHADER_STAGE_VERTEX_BIT", "fragment": "VK_SHADER_STAGE_FRAGMENT_BIT",
    "compute": "VK_SHADER_STAGE_COMPUTE_BIT", "amplification": "VK_SHADER_STAGE_TASK_BIT_EXT",
    "mesh": "VK_SHADER_STAGE_MESH_BIT_EXT", "raygeneration": "VK_SHADER_STAGE_RAYGEN_BIT_KHR",
    "miss": "VK_SHADER_STAGE_MISS_BIT_KHR", "closesthit": "VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR",
    "anyhit": "VK_SHADER_STAGE_ANY_HIT_BIT_KHR", "intersection": "VK_SHADER_STAGE_INTERSECTION_BIT_KHR",
    "callable": "VK_SHADER_STAGE_CALLABLE_BIT_KHR",
}
SHADER_ATTR = re.compile(r'^\s*\[shader\("([a-z]+)"\)\]\s*$')


def slangc():
    pin = open(os.path.join(SHADERS, "slangc.version"), encoding="utf-8").read().split()[0]
    for c in (os.environ.get("SLANGC"), os.path.expanduser(f"~/.local/opt/slang-{pin}/bin/slangc"),
              shutil.which("slangc")):
        if c and os.path.exists(c):
            return c
    sys.exit("gen_bindings: no slangc (set SLANGC)")


def shader_files():
    """(path, stages): every .slang with [shader(...)] entry points, as the build compiles them."""
    out = []
    for d, dirs, files in os.walk(SHADERS):
        dirs[:] = [x for x in dirs if os.path.relpath(os.path.join(d, x), SHADERS).split(os.sep)[0] not in SKIP_DIRS]
        for f in sorted(files):
            if not f.endswith(".slang"):
                continue
            p = os.path.join(d, f)
            stages = [m.group(1) for m in map(SHADER_ATTR.match, open(p, encoding="utf-8")) if m]
            if stages:
                out.append((p, stages))
    return sorted(out)


def flat(path):
    return os.path.relpath(path, SHADERS)[:-len(".slang")].replace(os.sep, "_").replace("/", "_")


def reflect(compiler, path, tmp):
    """The reflection JSON of `path` (every entry point shares its parameters: checked by the caller)."""
    name = flat(path)
    out = os.path.join(tmp, name + ".json")
    cmd = [compiler, path, "-target", "spirv", "-profile", "spirv_1_6", "-fvk-use-entrypoint-name",
           "-matrix-layout-column-major", "-I", SHADERS, "-o", os.path.join(tmp, name + ".spv"),
           "-reflection-json", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"slangc failed on {path}:\n{r.stderr[-2000:]}")
    return json.load(open(out, encoding="utf-8"))


def descriptor(t):
    """(VkDescriptorType, count) of a resource type; count 0 = unsized."""
    count = 1
    if t["kind"] == "array":
        count = t.get("elementCount", 0)
        t = t["elementType"]
    k = t["kind"]
    if k == "samplerState":
        return "VK_DESCRIPTOR_TYPE_SAMPLER", count
    if k == "constantBuffer":
        return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER", count
    if k == "resource":
        shape = t.get("baseShape")
        if shape in ("structuredBuffer", "byteAddressBuffer"):
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER", count
        if shape == "accelerationStructure":
            return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR", count
        if shape.startswith("texture"):
            if t.get("access") == "readWrite":
                return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE", count
            if t.get("combined"):
                return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", count
            return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE", count
    raise ValueError(f"unknown resource type {json.dumps(t)[:200]}")


SCALAR_CPP = {"float32": "float", "uint32": "uint32_t", "int32": "int32_t"}
VEC_CPP = {"float32": "glm::vec", "uint32": "glm::uvec", "int32": "glm::ivec"}


def cpp_type(t, structs):
    """(C++ type, size) of a push-constant field; nested structs are added to `structs`."""
    k = t["kind"]
    if k == "scalar":
        return SCALAR_CPP[t["scalarType"]], 4
    if k == "vector":
        n = t["elementCount"]
        return f'{VEC_CPP[t["elementType"]["scalarType"]]}{n}', 4 * n
    if k == "matrix":
        r, c = t["rowCount"], t["columnCount"]
        assert t["elementType"]["scalarType"] == "float32" and r == c == 4, t
        return "glm::mat4", 64
    if k == "array":
        e, size = cpp_type(t["elementType"], structs)
        n = t["elementCount"]
        stride = t.get("uniformStride", size)
        assert stride == size, f"an array whose stride {stride} is not its element's size {size}"
        return f"std::array<{e}, {n}>", size * n
    if k == "struct":
        structs.append(t)
        return t["name"], next(s["value"] for s in t.get("sizes", [{"kind": "uniform", "value": 0}])
                               if s["kind"] == "uniform") if t.get("sizes") else 0
    raise ValueError(f"push constant field kind {k}")


def struct_code(t, structs_out):
    """C++ for one reflected struct: fields at their offsets, padding where the shader leaves a gap."""
    lines, asserts, nested, at, pad = [], [], [], 0, 0
    name = t["name"]
    for f in t["fields"]:
        off = f["binding"]["offset"]
        ty, size = cpp_type(f["type"], nested)
        size = f["binding"].get("size", size)
        if off > at:
            lines.append(f"    uint8_t gap{pad}[{off - at}];")
            pad += 1
        lines.append(f"    {ty} {f['name']};")
        asserts.append(f"static_assert(offsetof({name}, {f['name']}) == {off});")
        at = off + size
    total = next((s["value"] for s in t.get("sizes", []) if s["kind"] == "uniform"), at)
    if total > at:
        lines.append(f"    uint8_t gap{pad}[{total - at}];")
    for n in nested:
        struct_code(n, structs_out)
    code = f"struct {name} {{\n" + "\n".join(lines) + "\n};\n" + "\n".join(asserts) + \
        f"\nstatic_assert(sizeof({name}) == {total});\n"
    if code not in structs_out:
        structs_out.append(code)


def header(path, stages, refl):
    name = flat(path)
    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
    entry_stages = [e.get("stage") for e in refl.get("entryPoints", [])]
    stage_names = sorted({s for s in (entry_stages if all(entry_stages) else []) + stages})
    bits = " | ".join(STAGE_BITS[s] for s in stage_names) or "0"
    bindings, sets, structs, push = [], {}, [], None
    for p in refl["parameters"]:
        b = p["binding"]
        if b["kind"] == "pushConstantBuffer":
            push = p["type"]["elementType"]
            continue
        if b["kind"] != "descriptorTableSlot":
            continue
        ty, count = descriptor(p["type"])
        s = b.get("space", 0)
        bindings.append(f"inline constexpr Binding {p['name']}{{{s}, {b['index']}, {ty}, {count}}};")
        sets.setdefault(s, []).append((b["index"], p["name"]))
    if push:
        struct_code(push, structs)
    out = [f"// Generated by tools/gen_bindings.py from {rel} (Slang reflection). Do not edit.",
           "#pragma once", '#include "render/shader_bindings/binding.hpp"', "",
           f"namespace ohao::shader::{name} {{", "",
           "// The stages this file's entry points run in.",
           f"inline constexpr VkShaderStageFlags kStages = {bits};", ""]
    out += bindings
    for s in sorted(sets):
        names = [n for _, n in sorted(sets[s])]
        out += ["", f"// Set {s}, in binding order: its descriptor set layout (layoutBindings).",
                f"inline constexpr std::array<Binding, {len(names)}> kSet{s}{{{', '.join(names)}}};"]
    if push:
        out += ["", f"// The push constants (the shader's {push['name']}).", ""] + structs
        out.append(f"using PushConstants = {push['name']};")
    out += ["", f"}} // namespace ohao::shader::{name}", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="exit 1 if any header is out of date")
    args = ap.parse_args()
    compiler = slangc()
    files = shader_files()
    with tempfile.TemporaryDirectory() as tmp:
        with ThreadPoolExecutor(max_workers=4) as pool:
            refls = list(pool.map(lambda f: reflect(compiler, f[0], tmp), files))
    want = {flat(p) + ".hpp": header(p, st, r) for (p, st), r in zip(files, refls)}
    # all.hpp includes every header: shader_bindings.cpp compiles it, so each static_assert is checked every build.
    want["all.hpp"] = "// Generated by tools/gen_bindings.py: every shader's bindings. Do not edit.\n#pragma once\n" + \
        "".join(f'#include "render/shader_bindings/{f}"\n' for f in sorted(want))
    have = {f for f in os.listdir(OUT) if f.endswith(".hpp") and f != "binding.hpp"} if os.path.isdir(OUT) else set()
    stale = sorted(f for f, text in want.items()
                   if not os.path.exists(os.path.join(OUT, f)) or open(os.path.join(OUT, f), encoding="utf-8").read() != text)
    extra = sorted(have - set(want))
    if args.check:
        for f in stale:
            print(f"gen_bindings: {f} is out of date (python tools/gen_bindings.py)")
        for f in extra:
            print(f"gen_bindings: {f} has no shader")
        return 1 if stale or extra else 0
    os.makedirs(OUT, exist_ok=True)
    for f in stale:
        open(os.path.join(OUT, f), "w", encoding="utf-8", newline="\n").write(want[f])
    for f in extra:
        os.remove(os.path.join(OUT, f))
    print(f"gen_bindings: {len(want)} headers, {len(stale)} written, {len(extra)} removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
