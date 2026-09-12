# Changelog

All notable changes to OHAO Engine are documented here. Newest first.

## Current — C++20 hybrid Vulkan renderer

Standalone pure-C++ engine (no Godot host). Hybrid path: KHR path tracer + deferred raster + RT shadows/GI, shared scene/materials/TLAS.

### Removed

Recorded here because this file promises "all notable changes" and three
removals had gone undocumented -- which left the section below reading as
current when it describes code that is gone.

- **Differentiable renderer -> its own repository.** `ohao/diff`, `tests/diff`,
  `shaders/diff` and `shaders/includes/diff` now live in `ohao_diff`: a
  Vulkan-compute path tracer with Path Replay Backpropagation and an explicit
  boundary term, gated by 83 unit tests, 75 GPU checks and a three-way
  comparison against Mitsuba 3. It **vendors** `material/ggx_aniso.glsl`,
  `rt/env_sampling.glsl`, `rt/mis.glsl`, `pbr_unpack.glsl`, `EnvCDF` and
  `RTAccelerationStructure` from here on purpose, so the two renderers cannot
  disagree about their surface physics -- change any of those and run
  `ohao_diff/tools/check_vendor_drift.sh`.
- **Inverse-rendering lab** (`e0a260b`). `inverse_fit`, `ohao/inverse/`,
  `tools/inverse_lab/`, `tools/inverse_c1/` and the figure/result packs under
  `docs/media/inverse/`. **The section immediately below documents that stack
  and is kept as the record of what was built** -- it is history, not a
  description of the current tree.
- **Animation, the OptiX denoiser, the tscn loader, scene serialization**
  (`0873766`). `--denoise=optix` is still parsed and falls back to OIDN with a
  warning; the live denoise set is `none|oidn|nrd|atrous|dlss`.

### Inverse rendering (Phase A–C1 hybrid)

- **Modular split**: `examples/inverse_fit.cpp` is a thin CLI; pipeline in `ohao/inverse/` — `fit_config`, `scene_builder`, `io`, `render_session`, `export_dataset`, `staged_fit`, `visual_polish`, `fit_engine` (+ loss/optimizer/quality).
- **Metal pass**: preset-first conductor/dielectric mode (hero glints no longer flip product floors to metal); stronger priors + post-schedule `metal_lock` stage; spheres chart targets mid-metal (0.55) not chrome; NN specular **hold** when SHOW already excellent.
- **Inverse Lab (L1–L2 frontier protocol)**: multi-view capture (`--export-capture`), train-only fit (`--lab-bundle`), spatial ground maps (`--map-res`), train/holdout/relight **PSNR+SSIM** + `lab_metrics.json`, **LABTEST** bar (holdout ≥28 dB, relight ≥26 dB, ≥8 dB gain).
- **`inverse_fit`**: multi-surface PBR + key/fill/rim + HDRI env scale.
- **B6 gap-close**: multi-start init, specular-weighted loss, high-spp BRDF stages, light regularizer, tighter light bounds, BRDF-pre for specular targets.
- **C1 neural θ prior**: `tools/inverse_c1/` train/infer/eval; unified 12D studio θ across levels.
- **Generalization ladder**: L0→L1 multi-preset → L2 domain-rand (cam/lights/hero) → L2e exposure jitter; `--domain-rand`, `--export-views`, `--export-exposure-jitter`.
- **`--nn-model` / `--theta-init`**: seed FD refine from learned prior.
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
