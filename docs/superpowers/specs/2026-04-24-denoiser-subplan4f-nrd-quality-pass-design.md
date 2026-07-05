# Denoiser Pipeline Sub-plan 4.F — NRD Quality Pass — Design

**Date:** 2026-04-24
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 Quality Pass (post-4.E follow-up)
**Predecessors:** 4.A–4.E (NRD integration). 4.F does NOT add new denoiser backends; it raises quality of the existing NRD path.

---

## 1. Goal

Take `--denoise=nrd` from "proof-of-concept that works end-to-end" (4.E state) to "visually close to raw PT 64spp on static camera, no ghosting on typical interactive motion, no black env background."

Concrete acceptance:
- Offline `env_demo --denoise=nrd mesh env out.png 4` ≥ 95% visually close to `env_demo mesh env out.png 64 --denoise=none` (raw PT 64spp reference).
- Interactive viewer held still for 30+ frames: visually clean ("looks like a game"), no residual 1spp noise.
- Interactive viewer during slow/medium camera motion: clean, minor disocclusion artifacts at silhouettes only (expected NRD behavior).
- Env background visible (not black) in all NRD output.

## 2. Non-Goals

- **ReSTIR DI (task #20).** Separate rendering-algorithm change — Sub-plan 4.G.
- **DLSS Ray Reconstruction.** Phase 5.
- **NRD in deferred pipeline.** RT-only.
- **Dynamic BLAS for animated meshes.** Tasks #81/#82.
- **Production ImGui UI for tuning `ReblurSettings` live.** Static defaults baked into `NrdDenoiser::initialize`.
- **Reference PSNR / SSIM harness.** Accept subjective visual match via verification-log PNG pairs.

## 3. Decisions

### 3.1 T1 — Env composite in tonemap shader

Problem: NRD compose (`denoisedDiff × albedo + denoisedSpec × F0`) writes zero for miss rays, so env pixels are black in `--denoise=nrd` output.

Solution: extend `shaders/rt/nrd_tonemap.comp` from 2 bindings (in-HDR + out-LDR) to 4 bindings:
- binding 0: in HDR (binding 29 composed, RGBA32F, readonly) — unchanged
- binding 1: out LDR (binding 30 tonemapped, RGBA8, writeonly) — unchanged
- binding 2 (NEW): in depth AOV (binding 20, R32F, readonly) — sky sentinel 1e30
- binding 3 (NEW): in env map sampler (the existing bindless env-map entry) — sample by ray direction

Shader logic: if `imageLoad(depth, p) >= 1e20`, compute ray direction from pixel, sample env map, apply env intensity + ACES + sRGB gamma, store. Else use existing path (ACES(binding29)).

Ray direction reconstruction uses camera invView/invProj — PathTracer must pass these via push constant (already uses them in raygen).

### 3.2 T2 — Motion vector + view-change correctness

Problem: interactive viewer doesn't signal camera-moved frames to NRD; motion vectors may be stale → ghosting during orbit.

Three sub-fixes:

**(a) Interactive wiring:** `examples/interactive.cpp` — whenever GLFW input moves the camera (mouse drag, WASD), call `renderer.notifyViewChanged()`. Typical location: input-handler block, or after `camera->update()` if the camera's "did view change this frame" bit exists. Simplest: always call it on any input event.

**(b) `pc.prevViewProj` audit:** confirm `m_prevViewProj` is captured at the END of render (representing THIS frame's VP) and used NEXT frame. Grep the existing code. Fix if off-by-one.

**(c) NRD bootstrap on view change:** when `m_viewChangedThisFrame == true`, pass `camera.frameIndex = 0` + identity prev matrices + reset `m_nrdTonemapFirstFrame = true` (optional — the reset from 4.E T2 I1 probably handles this). NRD treats the frame as fresh history → spatial-only for that frame → no stale-history ghosting.

**(d) MV debug dump:** add `--dump-mv` to env_demo (if not already via 3.A) — verify MVs are sensible on a stepped-camera test.

### 3.3 T3 — Multi-spp AOV accumulation (offline)

Problem: NRD sees 1spp input regardless of `--spp=N` command-line flag. High `N` only improves the raw-PT accumulation buffer (binding 1) for `--denoise=none` mode.

Solution: extend raygen shaders (pt_raygen.rgen + pt_raygen_offline.rgen + pt_raygen_realtime.rgen) — when in NRD mode AND `sampleIndex > 0`, AOVs (bindings 22/23/24/25/26) accumulate as running averages:

```glsl
// Old (overwrite, last-sample-wins):
imageStore(diffRadAOV, pixel, vec4(diffRad, hitDist));

// New (running-average for NRD mode):
vec4 prev = imageLoad(diffRadAOV, pixel);
float n = float(sampleIndex + 1);
vec4 running = mix(prev, vec4(diffRad, hitDist), 1.0 / n);
imageStore(diffRadAOV, pixel, running);
```

Gate on a push-constant bit `kPTFlagAccumulateAOVs` so non-NRD modes keep overwrite semantics (beauty-untouched invariant).

For realtime (interactive) path: N=1 always, so no difference. Realtime never triggers accumulation.

For offline (env_demo, turntable, etc): `--spp=4` with `--denoise=nrd` means 4 raygen passes each blend into AOVs → NRD sees 4spp-averaged input → significantly cleaner output.

### 3.4 T4 — Pixel jitter + ReblurSettings production tuning

**Jitter:** Halton(2,3) sequence — standard TAA jitter. Shift ray origin by ±0.5 pixel sub-pixel offset per frame. Fed to NRD via `CommonSettings.cameraJitter`. NRD uses jitter to undo the sub-pixel offset during reprojection, effectively giving temporal sample diversity even at 1spp/frame.

Implementation: raygen takes jitter as push-constant `vec2 jitter`. Pixel center = `pixelCenter + jitter`. `m_jitterPrev` member on PathTracer (like `m_prevViewMatrix`). Captured per frame, fed to NRD.

Gate: jitter only when `DenoiseMode::NRD` is active. Other modes get pixel-center sampling as before (Sobol determinism preserved).

**ReblurSettings:** `NrdDenoiser::initialize` currently calls `nrd::Integration::Initialize` without customizing `ReblurSettings`. Expose via a new method `NrdDenoiser::setReblurSettings(const ReblurSettings&)` called once after init with production defaults:

```cpp
nrd::ReblurSettings s = {};
s.hitDistanceParameters.A = 3.0f;        // NRD recommended for 1-4spp input
s.diffusePrepassBlurRadius = 30.0f;      // larger prepass for low-sample input
s.specularPrepassBlurRadius = 50.0f;
s.historyFixFrameNum = 3;                // faster history-fix for fast-moving cameras
s.antilagSettings.intensitySigmaScale = 2.0f;  // looser anti-lag
s.antilagSettings.hitDistanceSigmaScale = 2.0f;
s.maxAccumulatedFrameNum = 63;           // saturate temporal accumulation
s.maxFastAccumulatedFrameNum = 8;        // secondary history for fast motion
s.enableMaterialTestForDiffuse = true;
s.enableMaterialTestForSpecular = true;
```

These values are NVIDIA's REBLUR reference — calibrated on their NRDSample at 1spp/frame. Start here; tune only if specific artifacts appear.

## 4. Architecture

### 4.1 Component changes

| Component | Change |
|---|---|
| `shaders/rt/nrd_tonemap.comp` | +2 bindings (depth + env sampler), +env-composite branch |
| `ohao/render/rt/denoise/nrd_tonemap.cpp` | +2 layout bindings, +2 descriptor-set writes, +push-constant for camera inv matrices + env intensity |
| `shaders/rt/pt_raygen.rgen` (+ offline + realtime siblings) | +`kPTFlagAccumulateAOVs` branch in AOV writes (5 AOVs affected: 22/23/24/25/26) + jitter in ray origin |
| `ohao/render/rt/path_tracer_render.cpp` | Feed jitter + env intensity + cam-inv matrices to new push constants; conditionally set `kPTFlagAccumulateAOVs` bit |
| `ohao/render/rt/path_tracer.hpp` | `m_jitterCurrent`, `m_jitterPrev`, `m_haltonIndex` members |
| `ohao/render/rt/denoise/nrd_denoise.hpp/cpp` | New `setReblurSettings(const ohao::NrdReblurProfile&)` method; profile struct mirrors the few fields we tune (keeps NVIDIA's full struct out of our public surface) |
| `examples/interactive.cpp` | Call `renderer.notifyViewChanged()` on camera-move input |
| Verification log + CLAUDE.md | Updates for each task |

### 4.2 Per-frame flow (NRD mode, interactive with jitter)

```
compute Halton jitter for this frame → m_jitterCurrent
raygen (pixel + jitter offset, 1spp) → AOVs (overwrite — N=1)
NrdCameraInputs:
    viewMatrix = V, viewMatrixPrev = m_prevViewMatrix
    projMatrix = P, projMatrixPrev = m_prevProjMatrix
    jitter = m_jitterCurrent, jitterPrev = m_jitterPrev
    frameIndex = m_viewChangedThisFrame ? 0 : m_historyFrameCount   ← NEW in T2
NRD denoise (REBLUR with tuned settings) → bindings 27/28 denoised
NRD compose → binding 29 HDR
NRD tonemap with env blend → binding 30 RGBA8 (sky uses env, surface uses NRD)
capture m_prevViewMatrix = V; m_prevProjMatrix = P; m_jitterPrev = m_jitterCurrent
```

### 4.3 Offline --spp=N flow

```
for sample = 0..N-1:
    raygen (pixel + 0 jitter, sample N has different RNG seed) → AOVs (running average if sample > 0 else overwrite)
Single NRD denoise at the end → bindings 27/28
compose → tonemap with env blend → binding 30
```

One NRD dispatch per offline render (not N) — the AOV running average gives NRD clean N-spp input directly. frameIndex=0 for the single dispatch; no multi-frame history simulation needed.

## 5. Integration points

### 5.1 File map

| Path | Change |
|---|---|
| `shaders/rt/nrd_tonemap.comp` | Extended to 4 bindings + env-composite branch + push constants for inv view/proj + env intensity |
| `ohao/render/rt/denoise/nrd_tonemap.{hpp,cpp}` | NrdTonemapInputs gains `depthAOV`, `envMap` (VkImageView+VkSampler). `NrdTonemapInputs` + `dispatch` signatures extended. Push-constant struct for inv matrices. |
| `shaders/rt/pt_raygen.rgen` (+ 2 siblings) | Jitter in ray origin; `kPTFlagAccumulateAOVs` branch around every AOV imageStore |
| `ohao/render/rt/path_tracer_render.cpp` | Jitter capture + Halton sequence + NRD-mode bit + env push-constant |
| `ohao/render/rt/path_tracer.hpp` | Add `m_jitterCurrent`, `m_jitterPrev`, `m_haltonIndex`, reset-on-view-change wiring |
| `ohao/render/rt/denoise/nrd_denoise.{hpp,cpp}` | `setReblurSettings(const NrdReblurProfile&)` + production defaults |
| `examples/interactive.cpp` | `renderer.notifyViewChanged()` on camera input events |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | T1/T2/T3/T4 entries with render/4f_* PNG pairs |
| `CLAUDE.md` | Note 4.F quality pass shipped; env blend fixed; jitter + ReblurSettings tuned |

### 5.2 What does NOT change

- DenoiseMode enum (4.E sealed).
- NrdCompositor (4.D sealed).
- NrdDenoiser's NRI lifecycle (4.C sealed).
- Descriptor layouts for PT's main RT set (still 29 bindings).
- OIDN / OptiX / None paths (beauty-untouched invariant).
- OHAO_NRD=OFF still builds and runs cleanly.

## 6. Verification

### 6.1 T1 (env composite)

`renders/4f_t1/` contains three PNGs from a fixed scene (`realistic_female.glb` + `env_studio.hdr`):
- `nrd_before_t1.png` — current 4.E output (figure on black)
- `nrd_after_t1.png` — post-T1 (figure on lit env)
- `oidn_reference.png` — OIDN for comparison

Acceptance: `nrd_after_t1.png` env matches `oidn_reference.png` env within subjective "clearly same scene" bar.

### 6.2 T2 (motion correctness)

Interactive stepped-orbit capture:
- `renders/4f_t2/still_frame.png` — camera held still ~2 sec
- `renders/4f_t2/orbit_start.png` — just after orbit input starts
- `renders/4f_t2/orbit_settled.png` — after orbit stops, ~1 sec
- `renders/4f_t2/mv_dump.png` — motion vector visualization (camera panning)

Acceptance: `orbit_settled.png` has no ghosting trail. `mv_dump.png` shows coherent motion vectors (not garbage).

### 6.3 T3 (multi-spp offline)

`renders/4f_t3/` — env_demo runs at spp ∈ {1, 4, 16, 64}:
- `nrd_spp1.png`, `nrd_spp4.png`, `nrd_spp16.png`, `nrd_spp64.png`
- `none_spp64.png` — raw PT 64spp reference

Acceptance: monotonic quality improvement across spp; `nrd_spp16.png` ≥ 90% close to `none_spp64.png`.

### 6.4 T4 (jitter + ReblurSettings)

`renders/4f_t4/`:
- `interactive_held_still_60frames.png` — captured after 60 frames of held-still interactive
- `interactive_during_slow_pan.png` — captured mid-slow-pan
- `interactive_after_fast_whip.png` — captured ~15 frames after a fast mouse whip

Acceptance:
- Held-still frame 60: visually indistinguishable from raw PT 64spp reference.
- Slow pan: no noise, no ghosting.
- Post-fast-whip: some spatial-filter blur at silhouettes (expected), no stale-history ghosting, clean within 2-3 frames.

### 6.5 Invariants

- `--denoise=none|oidn|optix` bit-identical to pre-4.F at matching seed/scene.
- `OHAO_NRD=OFF` builds clean; `--denoise=nrd` still falls back to None.

## 7. Risks

| Risk | Mitigation |
|---|---|
| T1 env composite requires binding the env map (currently in PT's main descriptor set 12) to the tonemap shader | NrdTonemap gains one more descriptor-set entry for env sampler; PathTracer passes the env-map view via `NrdTonemapInputs`. |
| T3 raygen AOV accumulation changes the AOV content for ALL modes if not properly gated | `kPTFlagAccumulateAOVs` push-constant bit gates the new path. Only set when NRD mode is active. OIDN/None modes keep overwrite semantics — AOVs last-sample-wins as before. |
| T4 jitter introduces temporal flicker at static camera if jitter isn't consistent | NRD's `cameraJitter` / `cameraJitterPrev` fields tell NRD to account for sub-pixel offset during reprojection. Flicker should cancel. If not, reduce jitter amplitude to ±0.25 pixel. |
| T4 ReblurSettings production values may cause over-smoothing on detail features | Start with NVIDIA reference values. If details lose, tune `maxAccumulatedFrameNum` down (less history = less over-smoothing, more noise — tradeoff). |
| T2 `notifyViewChanged` spam (every mouse-move event) resets history too aggressively | Debounce: set the flag only if position/rotation delta > threshold. Or: let NRD handle it — its `historyFixFrameNum` setting (tuned in T4) already handles rapid-motion re-accumulation. |
| Composite env in tonemap requires env intensity + rotation — which PathTracer already has for raygen; must be threaded to tonemap | Push-constant block shared between raygen and tonemap's compose stage. Or `NrdTonemap` gets its own push-constant path. |

## 8. Success Criteria

1. `--denoise=nrd` at spp=1 renders env background (T1).
2. Interactive orbit has no ghosting trail (T2).
3. `env_demo --denoise=nrd --spp=4` is ≥ 90% close to `--denoise=none --spp=64` (T3).
4. Interactive held still ≥ 30 frames is visually close to raw PT 64spp reference (T4).
5. Non-NRD modes remain bit-equivalent to pre-4.F.
6. `OHAO_NRD=OFF` build clean.
7. Verification log has 4f_t1/t2/t3/t4 PNG pairs.

## 9. Task Shape (preview for writing-plans)

- **T1 — Env composite in tonemap.** Extend shader + NrdTonemap + plumbing for env-map and depth-AOV views. Biggest single visual win.
- **T2 — Motion correctness.** Interactive camera signaling + MV audit + bootstrap-on-view-change + MV debug dump.
- **T3 — Multi-spp AOV accumulation.** Raygen accumulation branch + kPTFlagAccumulateAOVs gate + env_demo `--spp=N` validated.
- **T4 — Jitter + ReblurSettings.** Halton jitter in raygen + m_jitterCurrent/Prev + NrdDenoiser::setReblurSettings + production defaults.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the 4-task implementation plan.
