#!/usr/bin/env python3
"""gen_bindings -- every shader's bindings for C++, from Slang's reflection (system design 4.2, step 2e).

The shader build runs it (shaders/CMakeLists.txt): each entry's compile writes its reflection JSON next to its
SPIR-V, and this writes the headers from them, only those whose text changed:

    python tools/gen_bindings.py --manifest build/shader_reflection/manifest.tsv --stamp FILE
    python tools/gen_bindings.py --check    exit 1 naming each descriptor binding C++ types as a number

The header of a shader file with entry points, ohao/render/shader_bindings/<flattened path>.hpp, namespace
ohao::shader::<flattened path>, holds one `Binding` per parameter (set, binding, descriptor type, count), each
set's bindings in order (`kSet<N>`, what a descriptor set layout is made from), the stages the file's entry points
run in (`kStages`), and the push-constant struct with every offset and the size static_asserted. C++ names these;
it never types a binding number or a push-constant layout again.
"""
import argparse
import json
import os
import re
import sys

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


def merged(path, jsons):
    """One reflection for the file from its entries' (each compiled alone): the parameters every entry shares, and
    every entry point."""
    refls = [json.load(open(j, encoding="utf-8")) for j in jsons]
    for j, r in zip(jsons[1:], refls[1:]):
        if r["parameters"] != refls[0]["parameters"]:
            raise RuntimeError(f"{path}: the entries of {os.path.basename(j)} and {os.path.basename(jsons[0])} "
                               "see different parameters")
    return {"parameters": refls[0]["parameters"], "entryPoints": [e for r in refls for e in r.get("entryPoints", [])]}


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
        clash = [n for i, n in sets.get(s, []) if i == b["index"]]
        if clash:
            raise RuntimeError(f"{path}: set {s} binding {b['index']} is both {clash[0]} and {p['name']}")
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
        out.append(f"inline constexpr uint32_t kPushSize = sizeof({push['name']});")
    out += ["", f"}} // namespace ohao::shader::{name}", ""]
    if push:
        fields = " ".join(f"X(T, {f['name']}, {f['binding']['offset']})" for f in push["fields"])
        out += ["// Every push-constant field and its offset: OHAO_CHECK_PUSH(CppStruct, " + name + ") holds a C++",
                "// struct to them.", f"#define OHAO_PUSH_FIELDS_{name}(T, X) {fields}", ""]
    return "\n".join(out)


# A descriptor binding typed as a number in C++ (vertex-input bindings are not descriptors and are not matched).
TYPED = [re.compile(p) for p in (
    r"dstBinding\s*=\s*\d", r"\blayoutBinding\(\s*\d", r"\bdescriptorWrite\([^,()]+,\s*\d",
    r"VkDescriptorSetLayoutBinding\s*\{\s*\d", r"\{\s*\d+u?\s*,\s*VK_DESCRIPTOR_TYPE_",
    r"bindings?\[\w+\]\.binding\s*=", r"\bdstBinding\s*=\s*\w+\s*\+", r"\badd(Image)?\(\s*\d+\s*,",
    r"(?<!shader::)\blayoutBinding\(\s*[a-z]\w*\s*,", r"\b(rtc::)?descriptorWrite\(")]   # the old helpers took numbers
ANY_TYPED = re.compile("|".join(f"(?:{r.pattern})" for r in TYPED))


def typed_bindings():
    """file:line of every descriptor binding C++ types as a number instead of naming a generated Binding."""
    hits = []
    for d, _, files in os.walk(os.path.join(ROOT, "ohao")):
        if os.path.abspath(d).startswith(OUT):
            continue
        for f in files:
            if not f.endswith((".cpp", ".hpp")):
                continue
            p = os.path.join(d, f)
            text = open(p, encoding="utf-8").read()
            if not ANY_TYPED.search(text):
                continue
            for n, line in enumerate(text.splitlines(), 1):
                code = line.split("//")[0]
                if any(r.search(code) for r in TYPED):
                    hits.append(f"{os.path.relpath(p, ROOT)}:{n}: {line.strip()[:100]}")
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", help="the build's (shader file, reflection JSON) pairs, one per entry: write the headers")
    ap.add_argument("--stamp", help="a file touched once the headers are current (the build's record)")
    ap.add_argument("--check", action="store_true", help="exit 1 naming each binding number C++ types")
    args = ap.parse_args()
    if args.check:
        typed = typed_bindings()
        for h in typed:
            print(f"gen_bindings: a typed binding number (name the generated Binding): {h}")
        return 1 if typed else 0
    if not args.manifest:
        ap.error("--manifest or --check")
    key = lambda p: os.path.normcase(os.path.normpath(p))
    jsons = {}
    for line in open(args.manifest, encoding="utf-8"):
        if line.strip():
            src, js = line.rstrip("\n").split("\t")
            jsons.setdefault(key(src), []).append(js)
    files = shader_files()
    if {key(p) for p, _ in files} != set(jsons):
        raise SystemExit("gen_bindings: the build's shaders and the files with [shader(...)] entry points differ "
                         f"(re-run cmake): {sorted({key(p) for p, _ in files} ^ set(jsons))}")
    want = {flat(p) + ".hpp": header(p, st, merged(p, jsons[key(p)])) for p, st in files}
    # all.hpp includes every header: shader_bindings.cpp compiles it, so each static_assert is checked every build.
    want["all.hpp"] = "// Generated by tools/gen_bindings.py: every shader's bindings. Do not edit.\n#pragma once\n" + \
        "".join(f'#include "render/shader_bindings/{f}"\n' for f in sorted(want))
    have = {f for f in os.listdir(OUT) if f.endswith(".hpp") and f != "binding.hpp"} if os.path.isdir(OUT) else set()
    stale = sorted(f for f, text in want.items()
                   if not os.path.exists(os.path.join(OUT, f)) or open(os.path.join(OUT, f), encoding="utf-8").read() != text)
    extra = sorted(have - set(want))
    os.makedirs(OUT, exist_ok=True)
    for f in stale:
        open(os.path.join(OUT, f), "w", encoding="utf-8", newline="\n").write(want[f])
    for f in extra:
        os.remove(os.path.join(OUT, f))
    if stale or extra:
        print(f"gen_bindings: {len(want)} headers, written: {' '.join(stale) or '-'}, removed: {' '.join(extra) or '-'}")
    if args.stamp:
        open(args.stamp, "w").close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
