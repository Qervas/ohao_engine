---
name: build
description: Configure and build OHAO (C++20, CMake, MSVC) and its shaders. Use before running anything after a code change, after adding a source file, or after editing anything under shaders/.
---

# Build OHAO

The shortest path is `python tools/check.py` (the `verify` skill): it builds the shaders
first, then everything, then runs the tests. Use the commands below when you only want
a build.

Use the real CMake: on this machine conda's `cmake` shadows it on PATH, so always name
the scoop shim. Never build with more than `-j6`; heavier load has crashed this machine.
Run one build at a time.

1. **Configure** on a fresh `build/`, or after adding a new directory or target. New
   files under an existing directory need nothing: sources and shaders are globbed with
   `CONFIGURE_DEPENDS`, so the next build re-runs the configure step itself.

```console
$ C:/Users/djmax/scoop/shims/cmake -B build -S .   # slow
```

2. **Build** (Release, everything). One target only, when you know which:
   `--target <name>` (`ohao`, `scene_tests`, …). Every target that loads SPIR-V depends
   on `shaders`, so it is rebuilt when a shader changed.

```console
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6   # slow
```

3. **Shaders alone**, when you want only them:

```console
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release --target shaders   # slow
```

## Reading failures

- Look for `error C` (compiler) and `error LNK` (linker) lines. `LNK4006` warnings are
  expected: `/FORCE:MULTIPLE` hides duplicates from stb and NRI's VMA.
- A new target that links but crashes right after "resizing to WxH" picked NRI's copy of
  VMA with a different configuration: see the VMA note in `AGENTS.md`.
- After restoring a file from a backup copy, `touch` it: MSBuild trusts mtimes.

Binaries land in `build/Release/`. The full gotcha list is in `AGENTS.md`.
