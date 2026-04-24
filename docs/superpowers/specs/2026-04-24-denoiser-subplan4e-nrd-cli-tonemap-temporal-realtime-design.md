# Denoiser Pipeline Sub-plan 4.E — `--denoise=nrd` + Tonemap + Temporal + Realtime — Design

**Date:** 2026-04-24
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 (NRD integration), **final** of five sub-plans
**Predecessors:** 4.A (library+CMake), 4.B (API expansion), 4.C (first dispatch), 4.D (remodulation compositor). Plus refactor split of `path_tracer.cpp` into 5 files.

---

## 1. Goal

Ship `--denoise=nrd` end-to-end as a first-class peer to OIDN / OptiX. Three deliverables:

1. **Offline tonemap integration** — NRD-composed HDR (binding 29 from 4.D) runs through a tonemap compute pass to a new RGBA8 output (binding 30). env_demo's existing "save beauty" flow transparently picks this up when `--denoise=nrd` is active, producing a properly-lit PNG just like OIDN does today.
2. **Temporal accumulation** — `NrdCameraInputs` stops hard-coding `frameIndex=0` and identity prev matrices. Real per-frame state (`m_historyFrameCount`, captured `m_prevViewMatrix`) is fed into NRD. This is where NRD's actual quality lives; at `frameIndex=0` the denoiser is spatial-only (fancy blur). Temporal reprojection + multi-frame accumulation is what makes NRD look dramatic.
3. **Realtime integration** — the interactive GLFW viewer gets `--denoise=nrd`. NRD runs per-frame in the render loop at 60+ fps. Walking-camera tests show visible quality improvement over raw-PT beauty without hitches. This is the "jaw-drop moment" — the payoff for Sub-plans 1 through 4.

After 4.E merges, Phase 4 (NRD integration) is complete.

## 2. Non-Goals

- **DLSS Ray Reconstruction** — Phase 5.
- **Tuning UI for `ReblurSettings`** — take NRD defaults throughout. An ImGui panel is a separate future task.
- **Multi-spp offline AOV accumulation** — parked. NRD's denoised output at spp=1 is what we ship. If the user runs `env_demo --denoise=nrd scene.glb env.hdr out.png 256`, the 256 raygen samples accumulate into beauty as usual, but NRD sees only the final 1-spp AOV (last-sample-wins since 3.C.6) — that's accepted for 4.E.
- **NRD in the deferred pipeline** — 4.E is strictly RT-mode. Deferred NRD would require wiring GBuffer-derived AOVs into NRD's inputs, which is a separate architecture.
- **Changes to the compose shader or 4.D compositor** — 4.E only adds a tonemap pass after compose; compose itself is untouched.
- **`ReblurSettings` / temporal-stability tuning beyond defaults** — if defaults produce artifacts in realtime, we surface them as parked follow-ups, not in-scope tuning.
- **Regression tests for the new CLI flag across all 5 examples** — smoke is "binary accepts `--denoise=nrd` and produces a non-black PNG for env_demo + cornell_box"; fuller regression is deferred.

## 3. Decisions

1. **`DenoiseMode::NRD = 3`** added to the enum in `ohao/render/rt/denoise/denoise_types.hpp`. Parser accepts `"nrd"` (case-insensitive). Round-trip via `denoiseModeName`. Matches the OIDN / OptiX pattern from 1.A / 1.B.
2. **New `NrdTonemap` class** at `ohao/render/rt/denoise/nrd_tonemap.{hpp,cpp}` — sibling to `NrdCompositor` (4.D), same 3-method shape (`initialize / shutdown / dispatch`). PIMPL with unique_ptr<Impl>; `OHAO_NRD=OFF` stub; `m_nrdTonemap` declared unconditionally on PathTracer (4.C ABI-hoist pattern).
3. **New `shaders/rt/nrd_tonemap.comp`** — 8×8 workgroup, 2 bindings (in: rgba32f HDR / out: rgba8 tonemapped). Tonemap math is the same ACES curve + gamma the raygen uses for its own binding-2 write; extract to a shared GLSL include if raygen's tonemap is non-trivial, otherwise copy-paste.
4. **New binding 30 (RGBA8 UNORM)** — PathTracer-owned, NOT added to PT's RT descriptor layout (same YAGNI rationale as binding 29). Usage `STORAGE | TRANSFER_SRC`. Lives alongside `m_nrdComposedImage` on PathTracer. Per-instance `m_nrdTonemapFirstFrame` latch gates the UNDEFINED→GENERAL transition (same pattern as `m_nrdComposeFirstFrame`), reset in `createImages()` for resize correctness.
5. **Dispatch site:** `PathTracer::render()` — after the compose dispatch from 4.D and inside the same `if (m_nrdCompositor)` block. Adds the tonemap dispatch guarded by `if (m_nrdTonemap)`. Binding 29 GENERAL→SHADER_READ barrier, binding 30 UNDEFINED/GENERAL→GENERAL barrier, dispatch, then binding 29 back to GENERAL.
6. **VulkanRenderer mode-aware output accessor.** New `VulkanRenderer::getFinalBeautyImage() const` returns binding 30 if `m_denoiseMode == DenoiseMode::NRD`, else binding 2 (`m_outputImage`). Every downstream consumer — `readbackOutputImage()`, the OIDN / OptiX post-process paths, env_demo's final save, the interactive GLFW blit — calls `getFinalBeautyImage()` instead of hard-coding binding 2. That's the one-site change that makes every example transparent to NRD mode.
7. **Temporal state.** Add `glm::mat4 m_prevViewMatrix{1.0f}` to PathTracer. At the top of `render()` (before `vkCmdTraceRaysKHR`), capture the current view matrix (`glm::inverse(pc.invView)`) for next frame; the value captured on frame N-1 is what gets fed into NRD for frame N. First frame has `m_prevViewMatrix == identity` — correct (no history).
8. **`frameIndex = m_historyFrameCount`.** Already the monotonic per-PathTracer frame counter used by 3.A motion vectors. Hand-off to NRD's `CommonSettings.frameIndex` just needs to replace the hard-coded `0`.
9. **Consume parked 4.C I5 (explicit `projMatrixPrev`).** `NrdCameraInputs` gains a `projMatrixPrev` field (std::array<float, 16>). Default to identity; `setCommonSettings` stops silently mirroring current proj to prev. For 4.E we'll feed current proj as prev because proj rarely changes; if it does (FOV change, resize), the implementer routes real prev proj through.
10. **Interactive viewer integration.** `interactive` CLI parses `--denoise=nrd` like the offline examples. The viewer's render loop calls `PathTracer::render()` every frame; NRD runs per frame in the same render-graph. GLFW blits `getFinalBeautyImage()`. Expected ~50-75 fps on a mid-range GPU (NRD adds ~1-3ms to a ~15ms frame).
11. **Fallback semantics.** `--denoise=nrd` on an `OHAO_NRD=OFF` build logs "NRD disabled at build time" and falls through to `DenoiseMode::None`. Matches the OptiX-stub-fallback precedent from 1.B.
12. **Beauty untouched for non-NRD modes.** `--denoise=none`, `--denoise=oidn`, `--denoise=optix` remain bit-for-bit equivalent to pre-4.E. Binding 2 / the accum buffer / the OIDN input — none of those code paths change.
13. **Three tasks, visibly-demoed each.** T1 produces a properly-lit offline PNG. T2 shows visible temporal improvement (lower noise at same camera-held-still spp=1). T3 is the realtime demo — 60+fps interactive NRD on a moving scene.

## 4. Architecture

### 4.1 Component map

```
PathTracer  (complete state after 4.A–4.E)
  ├─ m_nrdDenoiser          : std::unique_ptr<NrdDenoiser>     (4.C)
  ├─ m_nrdCompositor        : std::unique_ptr<NrdCompositor>   (4.D)
  ├─ m_nrdTonemap           : std::unique_ptr<NrdTonemap>      (NEW, 4.E)
  ├─ m_nrdComposedImage/View/Memory                            (binding 29, 4.D)
  ├─ m_nrdTonemappedImage/View/Memory                          (binding 30, NEW 4.E)
  ├─ m_nrdComposeFirstFrame  : bool                            (4.D)
  ├─ m_nrdTonemapFirstFrame  : bool                            (NEW 4.E)
  ├─ m_prevViewMatrix        : glm::mat4                       (NEW 4.E)
  └─ m_historyFrameCount     : uint32_t                        (exists, fed as NRD frameIndex)

NrdTonemap  (public PIMPL — mirrors NrdCompositor API exactly)
  ├─ bool initialize(VkDevice, VkPhysicalDevice, uint32_t w, uint32_t h)
  ├─ void shutdown()
  └─ void dispatch(VkCommandBuffer, const NrdTonemapInputs&)

NrdTonemapInputs (public struct)
  struct NrdTonemapInputs {
      VkImageView composedHDR;   // PT binding 29 (RGBA32F)
      VkImageView tonemappedOut; // PT binding 30 (RGBA8)
  };

VulkanRenderer additions
  └─ VkImage getFinalBeautyImage() const  // binding 30 if NRD mode, binding 2 otherwise

DenoiseMode additions
  └─ NRD = 3 (in enum), parse accepts "nrd", denoiseModeName returns "nrd"

NrdCameraInputs additions (consumes 4.C I5 follow-up)
  └─ std::array<float, 16> projMatrixPrev {};  // explicit, no longer silent-copy-of-proj
```

### 4.2 GLSL compute shader — `shaders/rt/nrd_tonemap.comp`

~20 lines:

```glsl
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly  image2D inHDR;
layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D outLDR;

vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outLDR);
    if (p.x >= size.x || p.y >= size.y) return;

    vec3 hdr = imageLoad(inHDR, p).rgb;
    vec3 mapped = acesFilm(hdr);
    vec3 srgb = pow(mapped, vec3(1.0 / 2.2));
    imageStore(outLDR, p, vec4(srgb, 1.0));
}
```

If raygen's tonemap uses a different curve (e.g. filmic Hable), this file copies that verbatim. The T1 implementer confirms the exact tonemap by grep-ing raygen shaders for `aces`/`tonemap`/`hable` and mirroring.

### 4.3 Per-frame data flow (NRD mode, any example)

```
PathTracer::render()
  ├─ Capture m_prevViewMatrix at frame start (for NEXT frame's NRD input)
  ├─ Populate NrdCameraInputs with:
  │    viewMatrix = current_V
  │    viewMatrixPrev = m_prevViewMatrix    (captured from previous frame)
  │    projMatrix = current_P
  │    projMatrixPrev = m_prevProjMatrix    (captured from previous frame — NEW)
  │    frameIndex = m_historyFrameCount     (real counter, not 0)
  ├─ vkCmdTraceRaysKHR                       (unchanged)
  ├─ NRD denoise                              (4.C — now with real temporal state!)
  ├─ NRD compose                              (4.D → binding 29 HDR)
  ├─ NRD tonemap                              (NEW 4.E → binding 30 RGBA8)
  ├─ m_prevViewMatrix = current_V            (save for next frame)
  ├─ m_prevProjMatrix = current_P
  └─ return
downstream (env_demo / interactive)
  └─ readbackOutputImage / blit → VulkanRenderer::getFinalBeautyImage()
                                    → binding 30 (NRD mode) or binding 2 (other)
                                    → save PNG / blit to swapchain
```

### 4.4 Binding plan

| Binding | Purpose | Descriptor set | Added in |
|---|---|---|---|
| 29 | NRD composed HDR (RGBA32F) | compose set (4.D) + tonemap set (in) | 4.D |
| 30 | NRD tonemapped (RGBA8 UNORM) | tonemap set (out) only | **4.E** |

Binding 30 lives ONLY in `NrdTonemap`'s 2-binding compute layout. PathTracer's RT descriptor layout stays at 29 bindings; the 4.D decision "don't add compose outputs to RT layout" applies equally to 4.E's tonemap output.

### 4.5 CLI + fallback

- All 5 examples (`cornell_box`, `model_viewer`, `env_demo`, `interactive`, `turntable`) accept `--denoise=nrd` via the existing `parseDenoiseMode` helper (no per-example wiring changes needed — 1.A centralized this).
- `--denoise=nrd` on an `OHAO_NRD=OFF` build logs "NRD disabled at build time" and falls through to `DenoiseMode::None`. Mirrors the OptiX 1.B fallback.
- `--denoise=nrd` with a failed `NrdDenoiser`/`NrdCompositor`/`NrdTonemap` init also falls through to `None` and logs once.

### 4.6 Interactive viewer integration

`examples/interactive.cpp`:
- Parses `--denoise=nrd`.
- When active, the viewer's per-frame render path (which already calls `VulkanRenderer::renderPathTraced()` every frame) automatically runs NRD end-to-end because the flag propagates via `RTRenderSettings::denoiseMode`.
- The GLFW blit needs to read `getFinalBeautyImage()` instead of the current hard-coded `m_outputImage` getter. One accessor swap.
- Expected perf: NRD adds ~1-3ms per frame on mid-range GPUs; at 1920×1080 with spp=1, total frame time ~15-20ms = 50-75 fps.

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `ohao/render/rt/denoise/denoise_types.hpp` | Add `NRD = 3` to enum; uncomment the future stub. |
| `ohao/render/rt/denoise/denoise_types.cpp` | Parser accepts `"nrd"`; `denoiseModeName` returns `"nrd"`. |
| `ohao/render/rt/denoise/nrd_tonemap.hpp` (NEW) | `NrdTonemap` class + `NrdTonemapInputs` struct. |
| `ohao/render/rt/denoise/nrd_tonemap.cpp` (NEW) | PIMPL impl with compute pipeline + OFF stub. |
| `shaders/rt/nrd_tonemap.comp` (NEW) | 8×8 compute, 2 bindings, ACES tonemap. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` | Add `projMatrixPrev` field to `NrdCameraInputs` (consumes 4.C I5 follow-up). |
| `ohao/render/rt/denoise/nrd_denoise.cpp` | `setCommonSettings` uses `projMatrixPrev` explicitly instead of mirroring current. |
| `ohao/render/rt/path_tracer.hpp` | Add `m_nrdTonemap`, `m_nrdTonemappedImage/View/Memory`, `m_nrdTonemapFirstFrame`, `m_prevViewMatrix`, `m_prevProjMatrix`, accessor pair (`getNrdTonemappedAOV()` / `getNrdTonemappedAOVImage()`). |
| `ohao/render/rt/path_tracer_images.cpp` | Allocate binding 30; reset `m_nrdTonemapFirstFrame = true` in createImages; cleanup in destroyImages. |
| `ohao/render/rt/path_tracer.cpp` | Initialize `m_nrdTonemap` alongside `m_nrdCompositor` in init; shutdown in destroy. |
| `ohao/render/rt/path_tracer_render.cpp` | Capture prev V/P at top; feed real temporal state into `NrdCameraInputs`; insert tonemap dispatch after compose with barriers. |
| `ohao/render/rt/rt_profile_renderer.hpp` | Add 2 pure virtuals + forwarders for binding 30 accessors. |
| `ohao/gpu/vulkan/renderer.hpp` | Declare `getFinalBeautyImage()` + `getNrdTonemappedAOVImage()` passthroughs. |
| `ohao/gpu/vulkan/renderer.cpp` | Implement `getFinalBeautyImage()` (mode-aware) + passthrough accessor. Route `readbackOutputImage` / OIDN post-process / save paths through `getFinalBeautyImage()`. |
| `examples/interactive.cpp` | Parse `--denoise=nrd`; update GLFW blit to read `getFinalBeautyImage()` instead of hard-coded binding 2. |
| `examples/env_demo.cpp` / `cornell_box.cpp` / `model_viewer.cpp` / `turntable.cpp` | No changes needed — they already use `parseDenoiseMode` and the central renderer output accessor. Verify by grep. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | T1/T2/T3 entries with renders/4e_* references. |
| `CLAUDE.md` | Binding 30 row; note `--denoise=nrd` is now live. |

### 5.2 What does NOT change

- The compose shader or `NrdCompositor` (4.D) — untouched.
- The NRD denoise pipeline or `NrdDenoiser` API (4.C) — untouched except for the new `projMatrixPrev` field in `NrdCameraInputs`.
- Raygen shaders — unchanged. Raygen still writes binding 2 for raw-PT mode; that path is selected when NRD mode is off.
- OIDN / OptiX post-process paths — they continue to run their own algorithm when selected; they just read from the mode-aware `getFinalBeautyImage()`.
- Deferred pipeline — 4.E is strictly RT.

## 6. Verification

### 6.1 T1 verification (CLI + offline tonemap)

```bash
./build/env_demo <mesh.glb> <env.hdr> renders/4e_t1/nrd_offline.png 1 --denoise=nrd
./build/env_demo <mesh.glb> <env.hdr> renders/4e_t1/oidn_offline.png 1 --denoise=oidn
```
Expected: both PNGs look properly lit (not black). `nrd_offline.png` is recognizably denoised at 1spp — cleaner than raw 1spp would be, though perhaps less sharp than OIDN at 1spp because NRD is spatial-only without temporal history.

### 6.2 T2 verification (temporal)

Stepped-camera test (camera held at position A for N frames, then jumps 10° to position B for N frames):
- Frame 0: `frameIndex=0`, `viewMatrixPrev` = identity. Spatial-only. Noisy.
- Frames 1..N-1: `frameIndex++`, prev V matches current V. Temporal accumulates, output gets cleaner.
- Frame N (camera jump): NRD detects disocclusion via prev-V mismatch, discards stale history, rebuilds.
- Frames N+1..2N-1: temporal accumulates again in the new position.

Render frames 10, 50 at position A (should be much cleaner than frame 0). Dump and compare visually.

**Use the interactive viewer for T2's test** — env_demo is one-shot so doesn't naturally exercise temporal accumulation. `interactive` already runs the per-frame render loop; T2 leaves the camera stationary for ~60 frames and captures screenshots at frame 0 + frame 50. The frame-50 capture should be visibly cleaner because NRD has accumulated history:

```bash
./build/interactive <scene> <env> --denoise=nrd
# capture screenshot at frame 0 (just after open) and at frame 50 (~1 sec of
# holding the camera still); compare the two in renders/4e_t2/
```

If `interactive` has no built-in screenshot key, T2 adds one (1-2 line change) or — as a fallback — modifies env_demo to dispatch raygen+NRD in an N-iteration loop before saving, with N configurable via `--nrd-warmup=N`. Implementer picks the cleaner path.

### 6.3 T3 verification (realtime)

```bash
./build/interactive <mesh.glb> <env.hdr> --denoise=nrd
```
Expected: window opens, NRD init logs fire once, orbit/pan the camera — scene stays visibly clean (no firefly spam), framerate 50-75 fps on mid-range GPU. No stuttering, no validation errors. Capture 3-5 screenshots (still, mid-orbit, post-orbit-settle) to `renders/4e_t3/`.

### 6.4 Invariants to preserve

- **`--denoise=oidn`** still produces identical OIDN output to pre-4.E. No pixel drift outside the PT's existing sampler non-determinism.
- **`--denoise=optix`** (when OptiX is available) likewise bit-identical.
- **`--denoise=none`** bit-identical.
- **`OHAO_NRD=OFF` builds** still compile and run cleanly; `--denoise=nrd` falls back to `None`.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| NRD tonemap produces visibly different colors than raygen's in-shader tonemap for the raw-PT path | T1 implementer extracts raygen's exact tonemap math and mirrors it in `nrd_tonemap.comp`. If raygen uses something exotic (filmic Hable), copy that exact curve. |
| At `frameIndex=0` with temporal-history-on, NRD misbehaves on first frame (ghosting, black regions, oversmoothing) | NRD handles `frameIndex=0` as a bootstrap (spatial-only). Verified working already in 4.C. T2 just ensures subsequent frames produce sensible output. |
| Camera-jump scene causes NRD to keep stale history and ghost | NRD's reprojection uses viewMatrixPrev to detect disocclusion. Passing real prev-V fixes this. If ghosting still happens, it's an NRD `ReblurSettings::antilagSettings` tuning issue — park as 4.E follow-up. |
| Interactive viewer's frame time exceeds 20ms and feels sluggish | NRD v4.17.2 REBLUR adds 1-3ms on a mid-range GPU at 1920x1080. If > 3ms, inspect via RenderDoc for dispatch bottleneck. Accept for 4.E; tune in a future sub-plan. |
| `getFinalBeautyImage()` call-site drift — some consumer still hard-codes binding 2 | T1 grep-audits all references to `m_outputImage` / `getOutputImage` / `readbackOutputImage` to ensure every consumer routes through the new mode-aware accessor. |
| `projMatrixPrev` field breaks existing NrdCameraInputs consumers (e.g. 4.C's probe) | The probe zero-inits the struct so `projMatrixPrev` defaults to all-zero; NRD tolerates this at frameIndex=0 (no history). Verify via probe smoke after T2. |
| Interactive viewer's current code path doesn't wire `RTRenderSettings::denoiseMode` through | Inspect `examples/interactive.cpp` in T3. If the settings don't propagate, add the plumbing — likely one `renderer.setDenoiseMode(mode)` call after argument parse. |

## 8. Success Criteria

1. `DenoiseMode::NRD` parsed + round-tripped by `parseDenoiseMode`/`denoiseModeName`.
2. `shaders/rt/nrd_tonemap.comp` builds to SPV.
3. `NrdTonemap` PIMPL class exists; builds ON + OFF; OFF stub returns false cleanly.
4. Binding 30 allocated as RGBA8 storage image, cleaned up, first-frame latch reset on (re)creation.
5. `PathTracer::render()` in NRD mode: raygen → NRD denoise → compose → tonemap → binding 30.
6. `m_prevViewMatrix` + `m_prevProjMatrix` captured at render start, fed as `viewMatrixPrev` / `projMatrixPrev`.
7. `frameIndex = m_historyFrameCount` replaces hard-coded 0.
8. `VulkanRenderer::getFinalBeautyImage()` returns binding 30 for NRD, binding 2 otherwise. All consumers (env_demo save, readback, interactive blit) use this accessor.
9. `env_demo --denoise=nrd` at spp=1 produces a properly-lit PNG (not black).
10. Stepped-camera interactive test shows visible quality improvement between frame 0 (spatial-only) and frame 50 (temporal-accumulated).
11. Interactive viewer at 1920x1080, spp=1, `--denoise=nrd` runs at ≥ 50 fps on the developer's hardware.
12. All other `--denoise=*` modes remain bit-identical to pre-4.E for same seed/scene.
13. `OHAO_NRD=OFF` build still compiles and runs; `--denoise=nrd` falls through to `None` with a single log line.
14. Verification log has T1/T2/T3 entries with render-folder PNG references.

## 9. Task Shape (preview for writing-plans)

Three tasks, each producing a visible demo:

- **T1 — CLI + offline tonemap path.** Add `DenoiseMode::NRD` enum + parser. Create `NrdTonemap` PIMPL + shader. Allocate binding 30. Add `getFinalBeautyImage()` mode-aware accessor. Dispatch tonemap in `render()` after compose. Wire all 5 examples to pick up `--denoise=nrd` via the central accessor. **Verify:** `env_demo --denoise=nrd` produces properly-lit PNG at spp=1. No temporal yet (`frameIndex=0`).
- **T2 — Temporal state wiring.** Add `m_prevViewMatrix` / `m_prevProjMatrix` PathTracer members. Capture at render start. Extend `NrdCameraInputs` with `projMatrixPrev` (consumes 4.C I5). Feed real `frameIndex = m_historyFrameCount`, `viewMatrixPrev`, `projMatrixPrev` into `setCommonSettings`. **Verify:** stepped-camera dispatch loop shows temporal accumulation produces visibly-cleaner output by frame 10-50 than frame 0. Interactive viewer captures screenshot pair.
- **T3 — Realtime viewer integration + final verification.** Wire `--denoise=nrd` into `examples/interactive.cpp`. Update GLFW blit to read `getFinalBeautyImage()`. Verify 50+ fps at 1920x1080 on a moving scene. Capture still + mid-orbit + settled screenshots to `renders/4e_t3/`. Update verification log + CLAUDE.md. **The jaw-drop moment.**

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the 3-task implementation plan.
