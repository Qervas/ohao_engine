# Changelog

All notable changes to OHAO Engine are documented here. Newest first.

## Current — C++20 hybrid Vulkan renderer

Standalone pure-C++ engine (no Godot host). Hybrid path: KHR path tracer + deferred raster + RT shadows/GI, shared scene/materials/TLAS.

### Inverse rendering (Phase A–C1 hybrid)

- **`inverse_fit`**: multi-surface PBR + key/fill/rim + HDRI env scale.
- **B6 gap-close**: multi-start init, specular-weighted loss, high-spp BRDF stages, light regularizer, tighter light bounds, BRDF-pre for specular targets.
- **C1 neural θ prior**: `tools/inverse_c1/` train/infer; `--export-dataset` writes `config.json` + pairs; `--nn-model` / `--theta-init` seed FD refine.
- **`--target-image`**: external LDR photo path (+ `--exposure` / `--fit-exposure`).
- **`--map-ground`**: 2×2 ground albedo tiles (shared rough/metal) — pragmatic texture step.
- **Schedule**: multi-start → env → lights → brdf_pre → albedo → brdf → pedestal → lights2 → refine.
- **`--preset`**: lantern, helmet, bottle, spheres, toycar, boombox, outdoor, mirror, chess, cornell.
- Roadmap in `docs/inverse.md` + `tools/inverse_c1/README.md`.

### Refactored to C++20

- **Language standard**: codebase targets **C++20** (`CMAKE_CXX_STANDARD 20`).
- **Core**: `Result` / error handling, concepts, layout traits, `[[nodiscard]]`, `string_view` / `span` APIs.
- **Modules**: subsystem entry headers (`core`, `gpu`, `render`, `rt`, `scene`, `physics`, `audio`) with shared RT settings / denoise policy metaprogramming.
- **Examples**: shared `examples/example_cli.hpp` for denoise/mode/spp parsing across demos.

### Rendering & RT

- Path tracer (offline + realtime profiles), MIS, env-map importance sampling, Sobol/PCG samplers.
- Denoisers: OIDN, OptiX (optional), NRD REBLUR, DLSS Ray Reconstruction.
- ReSTIR GI (realtime), DLSS-RR upscaling, NRD cinematic post (bloom / grade / DoF).
- Deferred pipeline: G-buffer, CSM, SSAO, SSR, SSS, TAA, bloom, ACES.
- Hybrid: RT shadows + 1-bounce RT GI on the deferred path.

### Reliability

- Golden-image harness (`tests/golden/`) + pre-push hook.
- Lazy PathTracer profile init (avoids dual full-res OOM on 8 GiB GPUs).
- Restored PathTracer output-res / AOV / reservoir members required by the offline/realtime image path.
- `model_viewer` default resolution **1920×1080** (was 4K).

### Demos (unchanged set)

| Binary | Role |
|--------|------|
| `cornell_box` | Offline / hybrid reference |
| `model_viewer` | GLB/OBJ in framed room |
| `env_demo` | Model + HDRI |
| `turntable` | Orbit frame sequences |
| `interactive` | Live GLFW viewer |

### Build

```bash
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build -j$(nproc)
```

Requires CMake 3.20+, Vulkan 1.3+ with RT extensions, a **C++20** compiler.

---

## Earlier eras (archived)

### [0.2.0] — Godot GDExtension phase

Transitioned to a Godot 4.x GDExtension plugin (`OhaoViewport`, editor integration). That host path is no longer the primary product line; the tree is the standalone Vulkan hybrid above.

### [0.1.0] — Initial standalone

Vulkan PBR, custom physics experiments, actor-component scene, ImGui editor, OBJ, shadows, multi-light.
