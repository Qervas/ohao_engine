---
name: build
description: Configure and build OHAO (C++20, CMake, GCC 16 on Fedora) and its shaders. Use before running anything after a code change, after adding a source file, or after editing anything under shaders/.
---

# Build OHAO

1. **Configure** when a directory or target was added, a `CMakeLists.txt` changed, or on a fresh
   `build/` (new files in globbed directories are picked up by the build itself). Name the repo's
   uv Python, or the harnesses run without numpy:

```
cmake -B build -S . -G "Ninja Multi-Config" -DSLANGC=$HOME/.local/opt/slang-2026.20/bin/slangc \
  -DSLANGC_CPP=$HOME/.local/opt/slang-2025.11/bin/slangc -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

2. **Build** (Release, everything), or one target when you know which (`--target ohao`):

```
cmake --build build --config Release -j16
```

3. **Shaders** are built by any target that loads SPIR-V; after a shader-only change,
   `--target shaders`. Skipping them runs stale SPIR-V while everything looks green.

Binaries land in `build/Release/`, SPIR-V in `build/Release/bin/shaders/`. A new target that
crashes right after "resizing to WxH" picked NRI's copy of VMA (AGENTS.md, the VMA gotcha).
