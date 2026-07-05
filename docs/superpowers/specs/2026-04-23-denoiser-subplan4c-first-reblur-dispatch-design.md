# Denoiser Pipeline Sub-plan 4.C — First REBLUR Dispatch — Design

**Date:** 2026-04-23
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 (NRD integration), third of five sub-plans
**Predecessors:** 4.A (NRD library + CMake), 4.B (NRD API expansion — CommonSettings + input image stash)

---

## 1. Goal

Run an actual REBLUR_DIFFUSE_SPECULAR compute dispatch on our AOVs and produce denoised diffuse + specular radiance that can be dumped to disk for visual verification. First time NRD does real work in the pipeline.

This is the "dispatch works" milestone — NRD compute pipelines get created, resources get bound, `vkCmdDispatch` gets recorded, and the denoised textures contain sensible spatially-filtered content.

## 2. Non-Goals

- Compositing denoised output back into beauty — that is 4.D (remodulation compositor).
- `--denoise=nrd` CLI flag — 4.E.
- Realtime integration / per-frame temporal accumulation — 4.E.
- Multi-spp AOV accumulation semantics (whether our raygen should accumulate AOVs across samples) — 4.E.
- Tuning `ReblurSettings` — take NRD defaults now; 4.E exposes knobs.
- Dynamic render resolution — deferred indefinitely; NRD's `resourceSize`/`rectSize` stay wired to init-time dimensions.
- Skeletal-animation-aware motion vectors (3.F territory).
- D3D12 / Windows paths.

## 3. Decisions

1. **Integration: vendor `NRDIntegration.hpp` from NRD v4.17.** NVIDIA ships a reference integration helper inside the NRD repo under `Integration/`. It's the same helper NVIDIA's own samples use. It handles pipeline creation, descriptor pools, transient+permanent texture allocation, constant-buffer ring, sampler set. We copy it into `external/nrd_integration/` and link it as a small static lib on top of `ohao_renderer`. Our `NrdDenoiser` PIMPL wraps the integration — public API stays identical to 4.B surface plus one new method.
2. **Dispatch site: inside `PathTracer::render()`**, after the last `vkCmdTraceRaysKHR` and before the function returns. Directly replaces the 4.B probe. Keeps AOV production and denoise consumption co-located, avoids exposing AOV image views publicly from PathTracer (4.E will factor them out when the realtime path needs it).
3. **Output storage: two new PathTracer-owned storage images at bindings 27 + 28**, RGBA32F, matching bindings 22/23. User-provided via NRD's UserPool so we can read them back without extra copies.
4. **Persistent `NrdDenoiser` member.** The 4.B probe was a scoped local in `PathTracer::init` that initialized + shut down in a single block. 4.C promotes it to a `std::unique_ptr<NrdDenoiser>` member that lives for PathTracer's lifetime. Also fixes the "probe fires twice" parked follow-up.
5. **SPP = 1 for verification.** Clearest "noisy input → denoised output" signal. No multi-sample AOV-accumulation plumbing required. Multi-spp correctness is a 4.E concern.
6. **`frameIndex = 0` on every run.** Spatial-only filtering; temporal reprojection is a no-op (no history). Validating the temporal path is explicitly 4.E's job — 4.C proves the dispatch + resource flow.
7. **Verification: two new CLI flags on `env_demo`.** `--dump-nrd-diffuse=<file.png>` and `--dump-nrd-specular=<file.png>`. Matches the existing `--dump-diffuse=` / `--dump-specular=` pattern. Visual "smoother than raw" comparison in the verification log.
8. **Barrier model:** NRD input AOVs transition `GENERAL → SHADER_READ_ONLY_OPTIMAL` before NRDIntegration dispatches and back to `GENERAL` after. The new bindings 27/28 go `UNDEFINED → GENERAL` before NRD dispatches. NRDIntegration manages barriers for its own internal textures — ours are our responsibility on either side of the `denoise()` call.
9. **Beauty-untouched invariant.** 4.C adds only. Beauty PNG must match pre-4.C bit-for-bit. Any drift is a correctness bug.
10. **Opt-out path preserved.** Building with `-DOHAO_NRD=OFF` still compiles and runs. NRD-dependent code guarded by `#ifdef OHAO_NRD_ENABLED` in PathTracer.

## 4. Architecture

### 4.1 Component map

```
PathTracer  (owns AOVs + descriptor set)
  ├─ m_nrdDenoiser : std::unique_ptr<NrdDenoiser>         (persistent, replaces 4.B probe)
  ├─ m_outDiffRadianceImage / View                        (new, binding 27)
  └─ m_outSpecRadianceImage / View                        (new, binding 28)

NrdDenoiser  (our public PIMPL)
  ├─ initialize / shutdown               (4.A, unchanged)
  ├─ setCommonSettings / setInputImages  (4.B, unchanged)
  └─ denoise(VkCommandBuffer cmd)        (NEW, 4.C)

NrdDenoiser::Impl  (internal)
  ├─ nrd::Instance*  instance            (from 4.A)
  ├─ NrdInputImages  inputs              (from 4.B)
  └─ NRDIntegration  integration         (NEW, 4.C — NVIDIA reference helper)
        owns: VkPipeline cache per NRD-spec dispatch,
              VkDescriptorPool + layouts,
              transient + permanent texture pool,
              constant-buffer ring,
              sampler set.
```

### 4.2 Public API additions

`ohao/render/rt/denoise/nrd_denoise.hpp` gains one method:

```cpp
/// Record NRD REBLUR_DIFFUSE_SPECULAR compute dispatches onto `cmd`.
/// Must be called after setCommonSettings() + setInputImages() in the same frame.
/// Assumes input AOVs are in SHADER_READ_ONLY_OPTIMAL and output AOVs in GENERAL.
/// Returns false if NRD rejects the dispatch (logs error).
bool denoise(VkCommandBuffer cmd);
```

`NrdInputImages` (4.B) already carries `outDiffRadianceHitDist` / `outSpecRadianceHitDist` fields — no struct change needed.

### 4.3 Per-frame data flow (offline, spp=1)

```
PathTracer::render(spp=1)
  ├─ Transition bindings 6,7,19..26 UNDEFINED→GENERAL   (existing)
  ├─ Transition bindings 27,28       UNDEFINED→GENERAL  (NEW)
  ├─ Bind RT pipeline + descriptor set 0
  ├─ vkCmdTraceRaysKHR()                                ← writes AOVs 19..26
  ├─ Barrier: bindings 19,20,22,23,26 GENERAL→SHADER_READ   (NEW)
  ├─ m_nrdDenoiser->setCommonSettings(cameraInputs)
  ├─ m_nrdDenoiser->setInputImages({views for 19,20,22,23,26, 27,28})
  ├─ m_nrdDenoiser->denoise(cmd)                        ← NEW — NRDIntegration records its dispatches
  ├─ Barrier: bindings 19,20,22,23,26 SHADER_READ→GENERAL  (return to idle state)
  └─ return
env_demo  (outside PathTracer)
  └─ readback bindings 27/28 → host → PNG dump
```

### 4.4 NRD slot binding table (REBLUR_DIFFUSE_SPECULAR)

| NRD slot                     | Our binding    | Image format / semantic                       |
|------------------------------|----------------|-----------------------------------------------|
| `IN_MV`                      | 19             | Motion vectors (RG16F)                        |
| `IN_NORMAL_ROUGHNESS`        | 26             | Packed normal + roughness (R10G10B10A2)       |
| `IN_VIEWZ`                   | 20             | Depth as view-space Z (R32F, 1e30 sentinel)   |
| `IN_DIFF_RADIANCE_HITDIST`   | 22             | Demod diffuse radiance + hit-dist (RGBA32F)   |
| `IN_SPEC_RADIANCE_HITDIST`   | 23             | Demod specular radiance + hit-dist (RGBA32F)  |
| `OUT_DIFF_RADIANCE_HITDIST`  | **27 (NEW)**   | Denoised diffuse + hit-dist (RGBA32F)         |
| `OUT_SPEC_RADIANCE_HITDIST`  | **28 (NEW)**   | Denoised specular + hit-dist (RGBA32F)        |

Bindings 21 (standalone roughness R16F), 24 (diffuse albedo RGBA8), 25 (specular color RGBA8) are **not** consumed by NRD in 4.C — they stay populated for 4.D remodulation.

### 4.5 Image usage flag note

NRD's `IN_*` slots are sampled textures; our bindings 19/20/22/23/26 currently declare `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. 4.C adds `VK_IMAGE_USAGE_SAMPLED_BIT` to those usage flags. NRD's `OUT_*` slots are storage images — bindings 27 + 28 use `STORAGE | TRANSFER_SRC` like their peers.

## 5. Integration Points

### 5.1 File map

| Path                                                        | Change                                                                                        |
|-------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `external/nrd_integration/NRDIntegration.hpp`               | NEW — vendored from `build/_deps/nrd-src/Integration/NRDIntegration.h`                        |
| `external/nrd_integration/NRDIntegration.cpp`               | NEW — vendored from `build/_deps/nrd-src/Integration/NRDIntegration.cpp`                      |
| `external/cmake/nrd.cmake`                                  | Extend: add a small static lib target compiling NRDIntegration, link it into `ohao_renderer`  |
| `ohao/render/rt/denoise/nrd_denoise.hpp`                    | Add `bool denoise(VkCommandBuffer)`                                                           |
| `ohao/render/rt/denoise/nrd_denoise.cpp`                    | Extend `Impl` with `NRDIntegration integration`, implement `denoise()` delegating to it       |
| `ohao/render/rt/path_tracer.hpp`                            | Add `m_nrdDenoiser`, `m_outDiffRadianceImage`/`View`, `m_outSpecRadianceImage`/`View`, accessors for bindings 27/28 |
| `ohao/render/rt/path_tracer.cpp`                            | Allocate out images + views in init, promote probe to persistent member, add SAMPLED usage to in-AOVs, barriers, dispatch wiring, cleanup in shutdown |
| `examples/env_demo/main.cpp`                                | `--dump-nrd-diffuse=<path>` / `--dump-nrd-specular=<path>` CLI flags using existing readback helper pattern |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 4.C entry with raw-vs-denoised visual evidence + observation                         |
| `CLAUDE.md`                                                 | Append bindings 27 + 28 to the Path Tracer descriptor bindings table                          |

### 5.2 What does NOT change

- Raygen shaders (all 3) — unchanged from 4.B. AOV production is already correct.
- `NrdInputImages` struct — already carries `outDiffRadianceHitDist` / `outSpecRadianceHitDist`.
- `NrdCameraInputs` struct — already carries everything `setCommonSettings` needs.
- `VulkanRenderer` — no NRD awareness yet (4.E wires it).
- Other examples (cornell_box, model_viewer, turntable, interactive) — no new flags; they continue to work with `OHAO_NRD=ON` but don't dump NRD outputs.

## 6. Verification

1. **Build clean, NRD ON:** `cmake --build build -j8` with `OHAO_NRD=ON` links cleanly. No unresolved symbols from NRDIntegration. No new warnings.
2. **Build clean, NRD OFF:** `cmake -B build-nonrd -S . -DOHAO_NRD=OFF && cmake --build build-nonrd -j8` succeeds. 4.C code inside `#ifdef OHAO_NRD_ENABLED` compiles away; NRDIntegration not pulled.
3. **Probe replacement:** running any example logs `[NRD] persistent instance ready @ WxH` once at PathTracer init (replacing 4.B's one-shot probe). No "probe accepted + immediately shut down" log.
4. **One-shot dispatch (primary verification):**
   ```
   ./build/env_demo assets/DamagedHelmet.glb assets/env/studio.hdr out.png 1 \
       --dump-diffuse=raw_diff.png \
       --dump-nrd-diffuse=nrd_diff.png \
       --dump-specular=raw_spec.png \
       --dump-nrd-specular=nrd_spec.png
   ```
   Expected: all four dumps written. `nrd_diff.png` is visibly smoother than `raw_diff.png` (1spp noise → spatial filter). Same relationship for specular.
5. **Validation-clean:** Vulkan validation layer reports zero errors across the NRDIntegration dispatch path.
6. **Beauty untouched:** `out.png` matches the pre-4.C checksum at the same seed / scene / spp. This is a pure AOV-side addition.
7. **Verification log entry:** appended to `tests/reference_scenes/custom/envlit_turntable/verification_log.md` with PNG paths + one-paragraph observation.
8. **CLAUDE.md binding table** includes bindings 27 + 28.

## 7. Risks

| Risk                                                                                  | Mitigation                                                                                               |
|---------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| NRDIntegration's Vulkan idioms don't match our split-lib layout / descriptor flow     | NRDIntegration is Vulkan-native; NVIDIA samples run on Vulkan. T1 budget includes integration shape match — first pass may need minor adapter (e.g., our VmaAllocator vs NRDIntegration's raw Vulkan alloc). |
| Image usage flag bump on bindings 19/20/22/23/26 (add `SAMPLED_BIT`) breaks existing readbacks | We keep `TRANSFER_SRC_BIT`; only adding a flag, not removing. Readback paths are untouched. Verify existing `--dump-diffuse=` still works post-change. |
| At `frameIndex=0` REBLUR's output looks pathological because temporal heuristics misfire on no-history | Expected: spatial-only output. If visibly broken (all-black / NaN), check NRD's "disable temporal" settings; last-resort escape hatch is `ReblurSettings::disableHistoryFix = true` or similar — but defaults should handle `frameIndex=0`. |
| Barrier state drift between NRDIntegration and our own barriers                       | NRDIntegration handles barriers for slots it owns (transient pool). We handle barriers for OUR images on either side of `denoise()`. Document the handoff contract in `nrd_denoise.cpp`. |
| `PathTracer.cpp` growing past its already-bloated size                                | Accept for 4.C. If 4.E adds another 80+ lines of NRD plumbing, split to `path_tracer_denoise.cpp` as a scope-bounded follow-up. |
| NRDIntegration pulls in heavyweight dependencies (e.g., glfw, spdlog)                 | It doesn't — it's a Vulkan-only header+cpp pair. Verify during T1 by inspecting NRDIntegration's includes before vendoring. |
| Vendored NRDIntegration drifts from upstream NRD                                      | Tag vendored files with source version (v4.17.2) in a leading comment. 4.A pins NRD FetchContent to v4.17.2; same tag applies to integration source. |
| License: NRDIntegration ships under NVIDIA RTX SDKs License (same as NRD)             | Fine for embedding (same precedent as rest of NRD). Vendored files carry upstream license headers. |

## 8. Success Criteria

1. `m_nrdDenoiser` is a persistent PathTracer member — probe replaced.
2. Bindings 27 + 28 allocated as RGBA32F storage images with views, registered in descriptor layout, transitioned, barrier-clean.
3. `NrdDenoiser::denoise(VkCommandBuffer)` records NRD compute dispatches via NRDIntegration.
4. `env_demo` with `--dump-nrd-diffuse=...` / `--dump-nrd-specular=...` produces visibly-smoother PNGs than matching raw AOV dumps at spp=1.
5. No Vulkan validation errors during NRD dispatch path.
6. Beauty PNG bit-identical to pre-4.C.
7. `-DOHAO_NRD=OFF` build still runs and produces identical output to 4.B OFF-build.
8. Verification log updated; CLAUDE.md binding table updated.

## 9. Task Shape (preview for writing-plans)

- **T1** — Vendor NRDIntegration into `external/nrd_integration/`, extend `external/cmake/nrd.cmake`, stub it into `NrdDenoiser::Impl` without wiring yet. Verify builds clean with `OHAO_NRD=ON` and `OHAO_NRD=OFF`. Verify existing 4.B probe still fires and passes.
- **T2** — Allocate bindings 27 + 28 storage images + views in PathTracer, extend descriptor set layout + pool, add SAMPLED usage flag to in-AOVs (19/20/22/23/26), add accessors, promote 4.B probe to persistent `m_nrdDenoiser` member. Barriers UNDEFINED→GENERAL for 27/28. No dispatch yet; smoke test passes; beauty untouched.
- **T3** — Implement `NrdDenoiser::denoise(cmd)` wiring NRDIntegration's per-frame dispatch call. Hook into `PathTracer::render()` after raygen. Add `--dump-nrd-diffuse=` / `--dump-nrd-specular=` flags in `env_demo`. Visual verification + verification log entry. CLAUDE.md update.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the 3-task implementation plan.
