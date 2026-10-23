---
name: build
description: Configure and build OHAO (C++20, CMake, MSVC) and its shaders. Use before running anything after a code change, after adding a source file, or after editing anything under shaders/.
---

# Build OHAO

Use the real CMake: on this machine conda's `cmake` shadows it on PATH, so always name
the scoop shim. Never build with more than `-j6`; heavier load has crashed this machine.
Run one build at a time.

1. **Configure** when a source file or directory was added or removed (sources are
   globbed), when a `CMakeLists.txt` changed, or on a fresh `build/`:

```console
$ C:/Users/djmax/scoop/shims/cmake -B build -S .   # slow
```

2. **Build** (Release, everything). One target only, when you know which:
   `--target <name>` (`ohao`, `scene_tests`, …).

```console
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6   # slow
```

3. **Shaders**, whenever anything under `shaders/` changed. The `shaders` target is not
   part of a default build, so skipping it runs stale SPIR-V while everything looks green:

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
