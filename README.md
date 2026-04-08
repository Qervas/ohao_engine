# OHAO Engine

A Vulkan 1.3 rendering engine with hardware ray tracing and Jolt Physics. Pure C++, no editor dependency.

## Features

- **Path Tracer** — Vulkan RT pipeline with NEE, Cook-Torrance GGX BRDF, progressive accumulation
- **Deferred Renderer** — GBuffer, CSM shadows, SSAO, bloom, TAA, ACES tonemapping
- **PBR Materials** — Per-pixel textures from GLTF/OBJ, specular-glossiness support
- **Jolt Physics** — AAA-grade physics with plugin architecture
- **3D Audio** — Positional audio via miniaudio
- **Model Loading** — GLTF/GLB (tinygltf), OBJ with auto-discovered textures

## Build

Requires: CMake 3.20+, Vulkan SDK, C++17 compiler (MSVC/GCC/Clang)

```bash
cmake -B build -S .
cmake --build build --config Release -j8
```

## Run

```bash
./build/Release/renderer_test output.png
```

Renders a path-traced scene to PNG. Edit `tests/renderer/renderer_test.cpp` to change scene/model/resolution/samples.

## Project Structure

```
src/
  renderer/           # Vulkan rendering (deferred + RT path tracer)
  engine/             # Scene graph, actors, components, asset loading
  physics/            # Jolt Physics backend
  audio/              # miniaudio backend
shaders/
  rt/                 # Path tracer shaders (raygen, closesthit, miss)
  gbuffer/            # Deferred GBuffer
  lighting/           # Deferred lighting
  post/               # Post-processing
tests/
  renderer/           # Standalone render test
external/             # Dependencies (Jolt, VMA, tinygltf, miniaudio, stb)
```

## License

[MIT License](LICENSE)
