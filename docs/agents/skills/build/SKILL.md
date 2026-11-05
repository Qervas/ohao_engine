---
name: build
description: Configure and build OHAO (C++20, CMake; GCC and Ninja on Linux, MSVC on Windows) and its shaders. Use before running anything after a code change, after adding a source file, or after editing anything under shaders/.
---

# Build OHAO

The shortest path is `python tools/check.py` (the `verify` skill): it builds the shaders
first, then everything, then runs the tests. Use the commands below when you only want
a build. Run one build at a time, and never while tests are using the binaries.

1. **Configure** on a fresh `build/`, or after adding a new directory or target. New
   files under an existing directory need nothing: sources and shaders are globbed with
   `CONFIGURE_DEPENDS`, so the next build re-runs the configure step itself.

Linux (the platform in daily use): Ninja Multi-Config, which keeps the `build/Release/`
layout every harness reads, and the pinned slangc from Slang's own release (Arch's
`shader-slang` writes C++ the CPU twins cannot compile).

```console
$ cmake -B build -S . -G "Ninja Multi-Config" -DSLANGC=$HOME/.local/opt/slang-2025.11/bin/slangc   # slow
```

2. **Build** (Release, everything). One target only, when you know which:
   `--target <name>` (`ohao`, `scene_tests`, …). Every target that loads SPIR-V depends
   on `shaders`, so it is rebuilt when a shader changed.

```console
$ cmake --build build --config Release -j16   # slow
```

3. **Shaders alone**, when you want only them:

```console
$ cmake --build build --config Release --target shaders   # slow
```

Windows: the same three steps through the real CMake, the scoop shim
`C:/Users/djmax/scoop/shims/cmake` (conda's `cmake` shadows it on PATH), with the Visual
Studio generator (no `-G`, no `-DSLANGC`: slangc comes from the Vulkan SDK) and at most
`-j6` (heavier load crashed that laptop). Binaries are `*.exe` there.

## Reading failures

- Linux: look for `error:` lines (GCC) and `undefined reference` (the linker). Duplicate
  symbols from stb and NRI's VMA are allowed by `--allow-multiple-definition`.
- Windows: look for `error C` (compiler) and `error LNK` (linker) lines. `LNK4006` warnings
  are expected: `/FORCE:MULTIPLE` hides the same duplicates.
- A new target that links but crashes right after "resizing to WxH" picked NRI's copy of
  VMA with a different configuration: see the VMA note in `AGENTS.md`.
- After restoring a file from a backup copy, `touch` it: the build trusts mtimes.

Binaries land in `build/Release/`. The full gotcha list is in `AGENTS.md`.
