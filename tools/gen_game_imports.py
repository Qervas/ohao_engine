#!/usr/bin/env python3
"""gen_game_imports -- what a game module imports from the engine on Windows, as a .def file (cmake/ohao_games.cmake).

A game is a DLL that calls into the executable that loads it (game/loader.hpp). On Linux the executable exports every
symbol it has (ENABLE_EXPORTS) and dlopen binds the module to them. An MSVC executable exports only what a .def file
lists, at most 65535 names, and the engine's objects define about a thousand external names each (RTTI, inline and
template copies), so the list is the names the games' objects leave undefined that an engine object defines:

    python tools/gen_game_imports.py --engine ENGINE.rsp --games GAMES.rsp --def OUT.def --lib OUT.lib
                                     --lib-exe LIB.EXE [--cache FILE] [--stamp FILE]

ENGINE.rsp and GAMES.rsp list object files, one a line. The .def (EXPORTS) and the games' import library made from it
(lib.exe, the DLL named "ohao_engine.exe": game/module_imports_win.cpp) are rewritten only when the list changes, so
an edit that imports nothing new relinks no executable. The hosts link the .def (/DEF). Exit 1, naming each, when a game reads engine data (a DLL reaches data only
through __declspec(dllimport), which a game module, bound late, cannot use: make it a function or an inline
constant) or keeps its own copy of a mutable engine variable (an inline variable or an inline function's static: on
Windows each module has its own, so the game and the engine would disagree; define it in a .cpp).
Test-only Python is the standard library; this one runs in the build.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

HOST = "ohao_engine.exe"
IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SCN_LNK_COMDAT = 0x1000
IMAGE_SCN_MEM_WRITE = 0x80000000
BIGOBJ_CLASS_ID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")


def read_symbols(path):
    """{name: [defined, function, writable comdat]} for each external symbol of a COFF object (plain or /bigobj)."""
    with open(path, "rb") as f:
        data = f.read()
    sig1, sig2 = struct.unpack_from("<HH", data, 0)
    if sig1 == 0 and sig2 == 0xFFFF and data[12:28] == BIGOBJ_CLASS_ID:
        nsec, symtab, nsym = struct.unpack_from("<III", data, 44)
        sections_at, sym_size = 56, 20
    else:
        nsec, = struct.unpack_from("<H", data, 2)
        symtab, nsym, opt = struct.unpack_from("<IIH", data, 8)
        sections_at, sym_size = 20 + opt, 18
    flags = [struct.unpack_from("<I", data, sections_at + 40 * i + 36)[0] for i in range(nsec)]
    strings = symtab + nsym * sym_size
    out = {}
    i = 0
    while i < nsym:
        at = symtab + i * sym_size
        raw = data[at:at + 8]
        if sym_size == 18:
            value, section, typ, cls, aux = struct.unpack_from("<IhHBB", data, at + 8)
        else:
            value, section, typ, cls, aux = struct.unpack_from("<IiHBB", data, at + 8)
        i += 1 + aux
        if cls != IMAGE_SYM_CLASS_EXTERNAL:
            continue
        if raw[:4] == b"\0\0\0\0":
            off = struct.unpack_from("<I", raw, 4)[0]
            end = data.index(b"\0", strings + off)
            name = data[strings + off:end].decode("latin-1")
        else:
            name = raw.rstrip(b"\0").decode("latin-1")
        defined = section > 0 or (section == 0 and value > 0)
        function = (typ & 0x30) == 0x20
        writable = section > 0 and bool(flags[section - 1] & IMAGE_SCN_MEM_WRITE) \
            and bool(flags[section - 1] & IMAGE_SCN_LNK_COMDAT)
        prev = out.get(name)
        if prev is None or (defined and not prev[0]):
            out[name] = [defined, function, writable]
    return out


def load(paths, cache):
    """{path: symbols}, from the cache where an object's size and time are unchanged."""
    out = {}
    for p in paths:
        st = os.stat(p)
        key = f"{st.st_size}:{st.st_mtime_ns}"
        hit = cache.get(p)
        if hit and hit["key"] == key:
            out[p] = hit["symbols"]
            continue
        symbols = read_symbols(p)
        cache[p] = {"key": key, "symbols": symbols}
        out[p] = symbols
    return out


# Writable state each module may keep its own copy of, harmlessly: RTTI descriptors (MSVC compares types by name),
# EnTT's type_index and type_id statics (EnTT compares types by their name's hash; the per-module index orders nothing
# here), a log message's per-thread buffer, and the C++ library's own (its namespace, its reserved C names).
def harmless(name):
    """A decorated name of such state (a thread-safe static's guard, ?$TSS0@, as what it guards)."""
    if name.startswith(("??_R", "?buf@?1???$emit@")):
        return True
    if name.startswith("_"):   # C linkage, a name reserved to the implementation (the C++ library's)
        return True
    if "@entt@@" in name and ("type_index@" in name or "type_id@" in name):
        return True
    return name.split("@@")[0].endswith("@std")


def lines(rsp):
    with open(rsp, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip()]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--engine", required=True, help="a file listing the engine's object files")
    ap.add_argument("--games", required=True, help="a file listing the games' object files")
    ap.add_argument("--def", dest="out", required=True)
    ap.add_argument("--lib", required=True, help="the games' import library")
    ap.add_argument("--lib-exe", required=True, help="lib.exe")
    ap.add_argument("--cache", help="the objects' symbols kept between runs")
    ap.add_argument("--stamp", help="touched on success")
    args = ap.parse_args(argv)

    cache = {}
    if args.cache and os.path.isfile(args.cache):
        try:
            with open(args.cache, encoding="utf-8") as f:
                cache = json.load(f)
        except ValueError:
            cache = {}
    engine_paths, game_paths = lines(args.engine), lines(args.games)
    engine = load(engine_paths, cache)
    games = load(game_paths, cache)
    if args.cache:
        kept = {p: cache[p] for p in engine_paths + game_paths}
        with open(args.cache, "w", encoding="utf-8") as f:
            json.dump(kept, f)

    defined = {}   # name -> function?
    shared_state = set()
    for symbols in engine.values():
        for name, (is_def, function, writable) in symbols.items():
            if is_def:
                defined.setdefault(name, function)
                if writable and not function:
                    shared_state.add(name)
    imports, data, copies = set(), set(), set()
    for symbols in games.values():
        for name, (is_def, function, writable) in symbols.items():
            if not is_def:
                ref = name[6:] if name.startswith("__imp_") else name
                if ref in defined:
                    (imports if defined[ref] else data).add(ref)
            elif name in shared_state and writable and not function and not harmless(name):
                copies.add(name)
    problems = [f"a game reads engine data {n}: make it a function or an inline constant" for n in sorted(data)]
    problems += [f"a game keeps its own copy of engine state {n}: define it in one .cpp" for n in sorted(copies)]
    if problems:
        print("gen_game_imports:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    if len(imports) > 65535:
        print(f"gen_game_imports: {len(imports)} imports, past the 65535 an executable exports", file=sys.stderr)
        return 1
    text = "EXPORTS\n" + "".join(f"    {n}\n" for n in sorted(imports))
    old = None
    if os.path.isfile(args.out):
        with open(args.out, encoding="utf-8") as f:
            old = f.read()
    if old != text or not os.path.isfile(args.lib):
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        r = subprocess.run([args.lib_exe, "/NOLOGO", "/MACHINE:X64", f"/DEF:{args.out}", f"/NAME:{HOST}",
                            f"/OUT:{args.lib}"], capture_output=True, text=True)
        if r.returncode != 0:
            os.remove(args.out)   # made again by the next build
            print(f"gen_game_imports: lib.exe failed:\n{r.stdout}{r.stderr}", file=sys.stderr)
            return 1
    if args.stamp:
        with open(args.stamp, "w", encoding="utf-8") as f:
            f.write(f"{len(imports)} imports\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
