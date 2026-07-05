# Denoiser Pipeline Sub-plan 4.D — Remodulation Compositor — Design

**Date:** 2026-04-24
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 (NRD integration), fourth of five sub-plans
**Predecessors:** 4.A–4.C (NRD library + API + first REBLUR dispatch), 3.C.6 (demodulated albedo + specular-color AOVs), 3.D (hit-distance in radiance alpha)

---

## 1. Goal

Combine NRD's denoised diffuse radiance (binding 27) and denoised specular radiance (binding 28) with the demodulated diffuse albedo (binding 24) and specular color / F0 (binding 25) to reconstruct a plausible HDR beauty signal:

```
composed(x, y) = denoisedDiff.rgb × diffAlbedo.rgb + denoisedSpec.rgb × specColor.rgb
```

This is the "pay the albedo back" step that closes the demodulation loop opened by Sub-plan 3.C.6. It's the first time we produce a non-trivial NRD-denoised color image.

Output lands in a new PathTracer-owned storage image (binding 29, RGBA32F HDR). env_demo gains `--dump-nrd-composed=<path>` to dump it. The existing beauty path (binding 2, tonemapped) stays bit-identical.

## 2. Non-Goals

- `DenoiseMode::NRD` CLI flag or runtime switch — 4.E.
- Realtime / per-frame temporal state — 4.E must route real `frameIndex` + `viewMatrixPrev` + `jitterPrev` (hard-coded zero in 4.C).
- Tonemap integration / writing to binding 2 — 4.E.
- Sum-match RMSE harness (1% validation against raw PT) — parked follow-up from 4.C.
- Any change to raygen shaders or beauty accumulation.
- Dynamic resolution / realtime pacing tuning.
- NRD `ReblurSettings` tuning — take NRD defaults.

## 3. Decisions

1. **`NrdCompositor` PIMPL** under `ohao/render/rt/denoise/nrd_compose.{hpp,cpp}` — sibling class to `NrdDenoiser` (4.A–4.C), `OidnDenoiser` (1.A), `OptixDenoiser` (1.B). Same design pattern: header declares a PIMPL class with lifecycle + dispatch methods; `.cpp` holds a `struct Impl` with Vulkan objects.
2. **GLSL compute shader** `shaders/rt/nrd_compose.comp` — 8×8 workgroup, reads 4 bindings (24 diffAlbedo, 25 specColor, 27 denoisedDiff, 28 denoisedSpec), writes 1 (29 composed HDR). Minimal logic: two `imageLoad`s + multiplies + sum + `imageStore`. Drops alpha of 27/28 (hit-distance).
3. **Binding 29 = RGBA32F storage image** owned by PathTracer, but **NOT added to PathTracer's RT descriptor set layout**. Raygen / closest-hit / miss never sample or write it — only the compose compute pipeline does, via its own descriptor set layout. PathTracer owns the `VkImage`/`VkImageView`/`VkDeviceMemory` trio and exposes accessors; `NrdCompositor::dispatch()` receives the view as an `NrdComposeInputs` field. This saves one slot in PT's bindings array and matches YAGNI (don't reserve a descriptor slot no RT shader reads). Alpha = 1.0 (reserved for future use; consumers read `.rgb` only). Image usage `STORAGE | TRANSFER_SRC`.
4. **Dispatch site: `PathTracer::render()`** after `m_nrdDenoiser->denoise(cmd)` in the same frame. Matches the 4.C pattern (denoise + compose co-located with AOV production).
5. **Ownership:** `std::unique_ptr<NrdCompositor> m_nrdCompositor` as a persistent PathTracer member, initialized alongside `m_nrdDenoiser` in `PathTracer::init`, destroyed in `PathTracer::destroy()`. **Must hoist the member outside the `#ifdef OHAO_NRD_ENABLED` guard** to avoid the ABI-mismatch bug discovered in 4.C T2 (`OHAO_NRD_ENABLED` is PRIVATE on `ohao_renderer`).
6. **Out-of-line `PathTracer` ctor** already exists from 4.C — extending it to `make_unique<NrdCompositor>()` in the same init body is safe.
7. **Readback helpers:** `VulkanRenderer::readbackNrdComposed(std::vector<float>& data, uint32_t& w, uint32_t& h)` mirrors the existing RGBA32F readback pattern. `env_demo` calls it then `dumpRGBA32FStream` which already tonemaps RGBA32F HDR → RGB PNG.
8. **IRTRendererProfile extension:** add `getNrdComposedAOV()` / `getNrdComposedAOVImage()` pure virtuals + `RTProfileRendererBase` forwarders. Same pattern as 4.C T2's bindings 27/28 accessor addition.
9. **Barrier model:** before compose — transition bindings 24/25/27/28 from GENERAL (post-NRD state after `restoreInitialState=true`) → SHADER_READ_ONLY_OPTIMAL, binding 29 from UNDEFINED (first frame) or GENERAL → GENERAL. After compose — transition 24/25/27/28 back to GENERAL. Binding 29 stays in GENERAL; readback transitions to TRANSFER_SRC in VulkanRenderer.
10. **No stub fallback for OFF builds.** `OHAO_NRD=OFF` already compiles `nrd_denoise.cpp` to empty stubs; `nrd_compose.cpp` follows the same pattern. PathTracer holds `std::unique_ptr<NrdCompositor>` unconditionally but the stub impl's `initialize()` returns false.
11. **Compose dispatch skipped on init failure.** `PathTracer::render()` guards `if (m_nrdCompositor && m_compositorReady)` around the dispatch. Binding 29 stays zero in this case — env_demo's composed-dump returns all-black, which is the expected "compose didn't run" signal.
12. **Alpha channel policy on binding 29:** write `1.0f`. Future sub-plans may repurpose alpha for a validity mask or post-composite hit-distance; for 4.D, keep it simple.
13. **Beauty invariant:** binding 2 (existing tonemapped output, rendered by the path tracer into `m_outputImage`) is untouched. env_demo's existing `out.png` must match pre-4.D sha256.

## 4. Architecture

### 4.1 Component map

```
PathTracer  (owns AOVs + NRD lifecycle + compose lifecycle)
  ├─ m_nrdDenoiser           : std::unique_ptr<NrdDenoiser>      (from 4.C)
  ├─ m_nrdCompositor         : std::unique_ptr<NrdCompositor>    (NEW, 4.D)
  ├─ m_nrdComposedImage      / View / Memory                     (NEW, binding 29)
  └─ m_compositorReady       : bool                              (NEW, gates dispatch)

NrdCompositor  (our public PIMPL — public surface tiny)
  ├─ bool initialize(VkDevice, VkPhysicalDevice, uint32_t w, uint32_t h)
  ├─ void shutdown()
  └─ void dispatch(VkCommandBuffer, const NrdComposeInputs&)

NrdCompositor::Impl  (internal)
  ├─ VkShaderModule          composeShader
  ├─ VkDescriptorSetLayout   setLayout
  ├─ VkPipelineLayout        pipelineLayout
  ├─ VkPipeline              composePipeline
  ├─ VkDescriptorPool        descPool
  ├─ VkDescriptorSet         descSet
  ├─ uint32_t                width, height
  └─ VkDevice                device

NrdComposeInputs (public struct in nrd_compose.hpp)
  struct NrdComposeInputs {
      VkImageView diffRadiance;   // binding 0 in compose shader ← binding 27 in PT layout
      VkImageView specRadiance;   // binding 1                  ← binding 28
      VkImageView diffAlbedo;     // binding 2                  ← binding 24
      VkImageView specColor;      // binding 3                  ← binding 25
      VkImageView composedOut;    // binding 4                  ← binding 29
  };
```

### 4.2 Compute shader — `shaders/rt/nrd_compose.comp`

```glsl
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly image2D inDiffRad;    // denoised diff  (27)
layout(set = 0, binding = 1, rgba32f) uniform readonly image2D inSpecRad;    // denoised spec  (28)
layout(set = 0, binding = 2, rgba8)   uniform readonly image2D inDiffAlbedo; // demod albedo   (24)
layout(set = 0, binding = 3, rgba8)   uniform readonly image2D inSpecColor;  // demod F0/color (25)
layout(set = 0, binding = 4, rgba32f) uniform writeonly image2D outComposed; // binding 29

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);

    vec4 diffRad    = imageLoad(inDiffRad,    p);    // .rgb = denoised radiance, .a = hit-dist (ignored)
    vec4 specRad    = imageLoad(inSpecRad,    p);
    vec4 diffAlbedo = imageLoad(inDiffAlbedo, p);    // RGBA8 UNORM in [0,1]
    vec4 specColor  = imageLoad(inSpecColor,  p);

    vec3 composed = diffRad.rgb * diffAlbedo.rgb
                  + specRad.rgb * specColor.rgb;

    imageStore(outComposed, p, vec4(composed, 1.0));
}
```

Declarative simplicity is the point. 4.D is one multiply + one add per channel. If the output looks wrong, the bug is in the inputs (AOVs from 3.C.6, denoised outputs from 4.C), not in this shader.

### 4.3 Per-frame data flow

```
PathTracer::render()
  ├─ raygen + AOV writes 19..26                     (unchanged from 4.C)
  ├─ barrier 27, 28: UNDEFINED → GENERAL            (4.C T2)
  ├─ barrier 29: UNDEFINED → GENERAL                (NEW, 4.D — first call; subsequent frames GENERAL → GENERAL)
  ├─ vkCmdTraceRaysKHR                              (unchanged)
  ├─ NRD denoise dispatch                           (4.C — writes 27, 28)
  ├─ barrier 24/25/27/28: GENERAL → SHADER_READ     (NEW, 4.D)
  ├─ m_nrdCompositor->dispatch(cmd, ...)            (NEW, 4.D — writes 29)
  ├─ barrier 24/25/27/28: SHADER_READ → GENERAL     (NEW, 4.D, restore)
  └─ return
env_demo outside PathTracer
  └─ readbackNrdComposed → host → dumpRGBA32FStream
```

### 4.4 Binding plan

| Binding | Role | Stage used | Descriptor set | Source |
|---|---|---|---|---|
| 24 | Diff albedo (RGBA8 UNORM) | RAYGEN (write) + COMPUTE (read) | PT RT set (slot 24) + compose set (slot 2) | Raygen 3.C.6 |
| 25 | Spec color / F0 (RGBA8 UNORM) | RAYGEN (write) + COMPUTE (read) | PT RT set (slot 25) + compose set (slot 3) | Raygen 3.C.6 |
| 27 | NRD denoised diff (RGBA32F) | NRD compute (write) + COMPUTE (read) | PT RT set (slot 27) + compose set (slot 0) | NRD 4.C |
| 28 | NRD denoised spec (RGBA32F) | NRD compute (write) + COMPUTE (read) | PT RT set (slot 28) + compose set (slot 1) | NRD 4.C |
| 29 | NRD composed HDR (RGBA32F) | COMPUTE (write, 4.D) | compose set (slot 4) only | 4.D |

Note: bindings 24/25/27/28 live in BOTH descriptor sets (PathTracer's RT layout *and* compose's compute layout). The same `VkImage` is referenced by two separate `VkDescriptorImageInfo` writes into two separate sets. This is legal Vulkan. Binding 29 only exists in compose's layout.

### 4.5 `NrdCompositor::initialize` — pipeline creation

1. `vkCreateShaderModule` from SPIR-V blob of `nrd_compose.comp`.
2. `VkDescriptorSetLayout` with 5 `STORAGE_IMAGE` bindings (all `SHADER_STAGE_COMPUTE_BIT`).
3. `VkPipelineLayout` from that set layout.
4. `VkComputePipelineCreateInfo` + `vkCreateComputePipelines`.
5. `VkDescriptorPool` (5 STORAGE_IMAGE slots, maxSets=1) + `vkAllocateDescriptorSets`.
6. Cache `VkDescriptorSet` handle.

Per-frame, `dispatch()`:
1. `vkUpdateDescriptorSets` with 5 `VkDescriptorImageInfo` (all `IMAGE_LAYOUT_GENERAL` for writeonly/readonly storage images).
2. `vkCmdBindPipeline(cmd, COMPUTE, composePipeline)`.
3. `vkCmdBindDescriptorSets(cmd, COMPUTE, pipelineLayout, 0, 1, &descSet, 0, nullptr)`.
4. `vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1)`.

Note: descriptor set layout in `NrdCompositor` is separate from PathTracer's 30-binding RT descriptor set. Compose is a distinct compute pipeline with its own layout.

## 5. Integration points

### 5.1 File map

| Path | Change |
|------|--------|
| `ohao/render/rt/denoise/nrd_compose.hpp` (NEW) | Public API + `NrdComposeInputs` struct |
| `ohao/render/rt/denoise/nrd_compose.cpp` (NEW) | PIMPL impl with compute pipeline + dispatch. OFF-branch stub (identical pattern to `nrd_denoise.cpp`) |
| `shaders/rt/nrd_compose.comp` (NEW) | GLSL compute shader (25 lines) |
| `shaders/CMakeLists.txt` (modify) | Add `nrd_compose.comp` to the SPIR-V build list |
| `ohao/render/rt/path_tracer.hpp` (modify) | `m_nrdCompositor` unique_ptr member, `m_nrdComposedImage/View/Memory`, accessors, forward-decl for `NrdCompositor` |
| `ohao/render/rt/path_tracer.cpp` (modify) | Image creation + cleanup for binding 29 (owned by PT but NOT in RT descriptor layout). Barriers for binding 29 (UNDEFINED→GENERAL). `m_nrdCompositor` init/shutdown. Dispatch call in `render()` after denoise. `m_nrdCompositor->dispatch()` receives binding 29's view via `NrdComposeInputs`. |
| `ohao/render/rt/rt_profile_renderer.hpp` (modify) | Add `getNrdComposedAOV()` / `getNrdComposedAOVImage()` pure virtuals + `RTProfileRendererBase` forwarders |
| `ohao/gpu/vulkan/renderer.hpp` (modify) | `readbackNrdComposed(data, w, h)` declaration + passthrough accessor |
| `ohao/gpu/vulkan/renderer.cpp` (modify) | `readbackNrdComposed` impl (mirror of `readbackDenoisedDiffuse`) |
| `examples/env_demo.cpp` (modify) | `--dump-nrd-composed=<path>` CLI flag + readback + dump |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` (modify) | 4.D entry with PNG references |
| `CLAUDE.md` (modify) | Add binding 29 to the descriptor bindings table |

### 5.2 What does NOT change

- Raygen shaders — AOV production unchanged.
- `NrdDenoiser` API surface — 4.C's `initialize / shutdown / setCommonSettings / setInputResources / denoise` all untouched.
- Existing readback helpers for bindings 22/23/24/25/27/28 — reused.
- VulkanRenderer's existing `init` / extension-capture flow — compose doesn't need extensions; Vulkan 1.3 compute is already in.
- Other examples (cornell_box, model_viewer, interactive, turntable) — no new flags. They don't dump composed output.
- Beauty path: binding 2 (`m_outputImage`) stays the raw PT's tonemapped RGBA8.

## 6. Verification

1. **Build clean, NRD ON:** `cmake --build build -j8`. NrdCompositor links, `nrd_compose.comp.spv` builds.
2. **Build clean, NRD OFF:** `cmake -B build-nonrd -S . -DOHAO_NRD=OFF && cmake --build build-nonrd -j8`. Stub `nrd_compose.cpp` compiles empty; PathTracer's compose hook short-circuits.
3. **Probe:** `[NRD compose] pipeline ready @ WxH` logged at `PathTracer::init` alongside `[NRD] persistent instance ready`.
4. **Primary smoke (the demo):**
   ```bash
   ./build/env_demo <mesh.glb> <env.hdr> /tmp/beauty_4d.png 1 \
       --dump-nrd-diffuse=/tmp/nrd_diff.png \
       --dump-nrd-specular=/tmp/nrd_spec.png \
       --dump-diff-albedo=/tmp/albedo.png \
       --dump-spec-color=/tmp/f0.png \
       --dump-nrd-composed=/tmp/composed.png
   ```
   Expected: all dumps written; `/tmp/composed.png` shows recognizable object colors (because albedo was re-multiplied in); much cleaner than raw 1spp PT beauty; roughly plausible vs high-spp reference.
5. **Reference comparison:** render the same scene at spp=256 as raw PT baseline (`./build/env_demo … /tmp/ref.png 256`), compare `/tmp/composed.png` against `/tmp/ref.png` by eye. They should look in the same lighting ballpark; composed can lose some specular detail due to NRD's spatial-only filter at frameIndex=0.
6. **Beauty bit-identical:** `out.png` from the above run matches the pre-4.D sha256 at the same scene + spp + seed (PT is deterministic at spp=1+Sobol).
7. **Zero new Vulkan validation errors** in the compose dispatch path.
8. **Verification log** appended with command + three PNG references (raw 1spp beauty, NRD composed 1spp, raw 256spp reference) + one-paragraph observation.
9. **CLAUDE.md binding table** includes binding 29.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| Composed output is black because binding 29 never got a write | Log on init failure; PathTracer logs if `m_compositorReady == false` at render time. A black PNG with no compositor-init log means shader SPV wasn't built or descriptor-set binding mismatched. |
| Binding 29's format (RGBA32F) produces tonemapper surprises when env_demo's `dumpRGBA32FStream` runs (it expects HDR floats with implicit tonemap → RGB) | We already use this helper for bindings 22/23 (raw diff/spec radiance dumps); path is well-trodden. |
| Demod AOVs (24/25) have lost precision due to RGBA8 UNORM → compose result shows banding on polished dielectrics (F0 = 0.04 → 10/255) | Known issue from 3.C.6, parked follow-up. 4.D just inherits the imprecision. If visible banding appears in composed output, it's an RGBA8 parent problem, not a 4.D bug. |
| NRD denoised output (27/28) alpha channel = hit-distance leaks into compose if `.rgb` mask is omitted | Shader explicitly uses `.rgb` on 27/28. Alpha drop is explicit. |
| Compose dispatch runs before NRD has finished writing (missing barrier) | ResourceSnapshot restoreInitialState in 4.C left 27/28 in GENERAL; we insert explicit GENERAL→SHADER_READ barrier in render(). |
| Binding 29's descriptor-set stage flag mismatch — RT descriptor layout uses RAYGEN_BIT_KHR for all bindings, but binding 29 is written by COMPUTE | Our compose compute pipeline has its OWN descriptor set layout (5 bindings, all COMPUTE_BIT). Binding 29 sits in BOTH layouts: RT layout for its descriptor slot (never touched by RT shaders — stage flag is cosmetic), and compose layout (COMPUTE_BIT, actually written). Two different VkDescriptorSets write to the same VkImage. This is legal Vulkan; verify no validation warnings. |
| `PathTracer` already carries 2000+ lines; 4.D adds ~100 more (binding 29 + compositor wiring) | Accept. If 4.E adds much more, split to `path_tracer_denoise.cpp`. Flagged as parked follow-up. |

## 8. Success criteria

1. `NrdCompositor` class exists with `initialize/shutdown/dispatch` methods; compiles and links under both OHAO_NRD=ON and OFF.
2. `shaders/rt/nrd_compose.comp` builds to SPV.
3. Binding 29 is a PathTracer-owned RGBA32F storage image with descriptor layout slot + pool allocation + writes + barriers.
4. `m_nrdCompositor` is a persistent PathTracer member (unconditionally declared per the 4.C ABI-hoist pattern).
5. `PathTracer::render()` dispatches compose after NRD denoise completes.
6. `VulkanRenderer::readbackNrdComposed` + env_demo `--dump-nrd-composed=<path>` dump a non-black HDR PNG that shows recognizable object colors.
7. Composed PNG at spp=1 is dramatically cleaner than raw PT beauty at spp=1, and lighting is plausible vs raw-PT-at-spp=256 reference.
8. Zero new Vulkan validation errors.
9. Beauty (binding 2 output) bit-identical to pre-4.D.
10. Verification log + CLAUDE.md updated.

## 9. Task shape (preview for writing-plans)

- **T1** — `NrdCompositor` PIMPL (hpp + cpp with Impl struct + OFF stub) + `nrd_compose.comp` GLSL shader + CMake shader-build wiring. Pipeline creates + destroys cleanly; `dispatch()` exists but no caller yet. Build clean ON + OFF. No runtime wiring — this task proves the compute pipeline is buildable in isolation.
- **T2** — PathTracer integration: binding 29 image/view/memory, descriptor layout growth 29→30, pool bump, writes, barriers, `m_nrdCompositor` persistent member, dispatch call in `render()` after NRD denoise. Accessor chain through `IRTRendererProfile` → `VulkanRenderer`. env_demo `--dump-nrd-composed=` CLI + readback. Visual verification + verification log entry. CLAUDE.md update.

## 10. Next step

Invoke `superpowers:writing-plans` to generate the 2-task implementation plan.
