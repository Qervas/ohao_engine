# Denoiser Sub-plan 4.F — NRD Quality Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring `--denoise=nrd` from proof-of-concept (4.E) to shippable realtime denoiser: env visible, no motion ghosting, multi-spp offline quality near-reference, production-tuned ReblurSettings with pixel jitter.

**Architecture:** Four independent quality-lift tasks, each touching a narrow slice. T1 extends tonemap shader with env blend. T2 fixes interactive view-change signaling + MV. T3 adds raygen AOV accumulation gated on NRD mode. T4 adds Halton jitter + production ReblurSettings.

**Tech Stack:** GLSL 460 compute + RT; Vulkan 1.3; NVIDIA NRD v4.17.2 REBLUR; existing jitter/TAA conventions.

**Spec:** `docs/superpowers/specs/2026-04-24-denoiser-subplan4f-nrd-quality-pass-design.md` — follow spec §3 decisions + §4 architecture + §5 integration points.

---

## Shared Context (every task reads this)

### Pre-existing state (after 4.E merge)

- `DenoiseMode::NRD` enum + CLI parse works across all 5 examples.
- PathTracer has `m_nrdDenoiser` + `m_nrdCompositor` + `m_nrdTonemap` persistent members.
- PathTracer captures `m_prevViewMatrix` + `m_prevProjMatrix` each frame; `frameIndex = m_historyFrameCount`.
- Bindings 27 (denoised diff), 28 (denoised spec), 29 (composed HDR), 30 (tonemapped RGBA8) all allocated.
- `shaders/rt/nrd_tonemap.comp` has 2 bindings (HDR in, LDR out), ACES + sRGB.
- Known limitation (what T1 fixes): env miss rays produce zero in composed; tonemap outputs black for those pixels.

### Invariants across all 4 tasks

1. `--denoise=none|oidn|optix` bit-identical to pre-4.F at matching seed/scene.
2. `OHAO_NRD=OFF` builds clean; `--denoise=nrd` falls back to None.
3. ABI-hoist discipline maintained (unconditional members, no `#ifdef OHAO_NRD_ENABLED` in headers).
4. Each task produces one commit (plus optional polish commits from code-review findings).

### Files you'll touch (by task)

| Task | Primary files |
|---|---|
| T1 | `shaders/rt/nrd_tonemap.comp`, `ohao/render/rt/denoise/nrd_tonemap.{hpp,cpp}`, `ohao/render/rt/path_tracer_render.cpp`, `ohao/render/rt/rt_profile_renderer.hpp` |
| T2 | `examples/interactive.cpp`, `ohao/render/rt/path_tracer_render.cpp` (view-change frameIndex=0 bootstrap), `ohao/gpu/vulkan/renderer.cpp` (MV readback/debug dump if needed) |
| T3 | `shaders/rt/pt_raygen.rgen` (+ pt_raygen_offline.rgen + pt_raygen_realtime.rgen), `ohao/render/rt/path_tracer_render.cpp` (gate push-constant flag) |
| T4 | `ohao/render/rt/denoise/nrd_denoise.{hpp,cpp}` (setReblurSettings + production defaults), raygen shaders (pixel jitter), `ohao/render/rt/path_tracer.hpp` + render.cpp (m_jitterCurrent/Prev + Halton) |

---

## Task 1: Env composite in tonemap shader

**Goal:** `--denoise=nrd` output has env background lit (not black).

**Spec reference:** §3.1 + §4.2

**Files:**
- Modify: `shaders/rt/nrd_tonemap.comp` (extend to 4 bindings, add env-composite branch)
- Modify: `ohao/render/rt/denoise/nrd_tonemap.{hpp,cpp}` (descriptor layout +2, inputs struct +2, dispatch updates + push constants for cam inv matrices + env intensity)
- Modify: `ohao/render/rt/path_tracer_render.cpp` (populate new NrdTonemapInputs fields)
- Modify: `ohao/render/rt/rt_profile_renderer.hpp` (add env-map accessor forwarders if not already there)

### Step 1.1 — Extend shader

- [ ] **Shader: add depth + env sampler bindings + env composite logic**

Open `shaders/rt/nrd_tonemap.comp`. Current is 2-binding (HDR in, LDR out). Replace with:

```glsl
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly  image2D inHDR;          // composed HDR (binding 29)
layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D outLDR;         // tonemapped (binding 30)
layout(set = 0, binding = 2, r32f)    uniform readonly  image2D inDepth;        // depth AOV (binding 20)
layout(set = 0, binding = 3)          uniform sampler2D envMap;                 // equirectangular env

layout(push_constant) uniform PushConstants {
    mat4 invView;         // camera inverse view
    mat4 invProj;         // camera inverse projection
    vec2 extent;          // render target size (for pixel → NDC)
    float envIntensity;   // env exposure multiplier
    float _pad;
} pc;

const float PI = 3.14159265359;

vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 sampleEnv(vec3 dir) {
    // Equirectangular sampling
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    vec2 uv = vec2(phi / (2.0 * PI) + 0.5, 0.5 - theta / PI);
    return texture(envMap, uv).rgb * pc.envIntensity;
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outLDR);
    if (p.x >= size.x || p.y >= size.y) return;

    vec3 hdr;
    float depth = imageLoad(inDepth, p).r;
    if (depth >= 1e20) {
        // Sky — reconstruct ray from pixel + sample env
        vec2 ndc = (vec2(p) + 0.5) / vec2(size) * 2.0 - 1.0;
        ndc.y = -ndc.y;   // Vulkan Y-flip (image origin top-left vs NDC bottom-left)
        vec4 target = pc.invProj * vec4(ndc, 1.0, 1.0);
        vec4 world  = pc.invView * vec4(normalize(target.xyz), 0.0);
        hdr = sampleEnv(normalize(world.xyz));
    } else {
        // Surface — use NRD composed
        hdr = imageLoad(inHDR, p).rgb;
    }

    vec3 mapped = acesFilm(hdr);
    vec3 srgb   = pow(mapped, vec3(1.0 / 2.2));
    imageStore(outLDR, p, vec4(srgb, 1.0));
}
```

- [ ] **Verify shader compiles**

```bash
cmake --build build --target shaders -j8 2>&1 | grep -iE "error|rt_nrd_tonemap" | head
ls build/shaders/rt_nrd_tonemap.comp.spv
```

### Step 1.2 — Extend NrdTonemap class

- [ ] **Update NrdTonemapInputs struct**

In `ohao/render/rt/denoise/nrd_tonemap.hpp`, change `NrdTonemapInputs` to:

```cpp
struct NrdTonemapInputs {
    VkImageView composedHDR     = VK_NULL_HANDLE;  // PT binding 29
    VkImageView tonemappedOut   = VK_NULL_HANDLE;  // PT binding 30
    VkImageView depthAOV        = VK_NULL_HANDLE;  // PT binding 20 (NEW 4.F T1)
    VkImageView envMapView      = VK_NULL_HANDLE;  // HDR env map (NEW 4.F T1)
    VkSampler   envMapSampler   = VK_NULL_HANDLE;  // linear sampler (NEW 4.F T1)

    // Per-frame push-constant contents (NEW 4.F T1)
    std::array<float, 16> invView   {};
    std::array<float, 16> invProj   {};
    std::array<float, 2>  extent    {};   // W, H
    float envIntensity              = 1.0f;
};
```

- [ ] **Extend descriptor layout from 2 to 4 bindings**

In `ohao/render/rt/denoise/nrd_tonemap.cpp` `initialize()`: bindings array grows to 4. Position 2 is `STORAGE_IMAGE` (depth AOV). Position 3 is `COMBINED_IMAGE_SAMPLER` (env map — note different type). Pool sizes grow: `STORAGE_IMAGE` +1 → 3, `COMBINED_IMAGE_SAMPLER` +1 → 1.

- [ ] **Add push constants to pipeline layout**

The push-constant struct mirrors shader. ~144 bytes (2 mat4 + vec2 + float + pad). Add `VkPushConstantRange` for `VK_SHADER_STAGE_COMPUTE_BIT`.

- [ ] **Extend dispatch() to write all 4 descriptors + vkCmdPushConstants**

`dispatch()` now updates 4 descriptors (the 2 existing + depth AOV + env sampler), writes push constants via `vkCmdPushConstants`, then binds + dispatches as before.

### Step 1.3 — Plumb through PathTracer

- [ ] **Populate NrdTonemapInputs in render() with depth + env + cam inv matrices**

In `path_tracer_render.cpp` NRD dispatch block where `NrdTonemapInputs ti` is created, populate:
```cpp
ti.composedHDR   = m_nrdComposedView;
ti.tonemappedOut = m_nrdTonemappedView;
ti.depthAOV      = m_depthAOVView;          // binding 20
ti.envMapView    = m_envMapView;            // from env map setter
ti.envMapSampler = m_envMapSampler;
std::memcpy(ti.invView.data(), glm::value_ptr(pc.invView), sizeof(float) * 16);
std::memcpy(ti.invProj.data(), glm::value_ptr(pc.invProj), sizeof(float) * 16);
ti.extent = {float(m_width), float(m_height)};
ti.envIntensity = m_envCDFIntegral > 0.0f ? 1.0f : 0.0f;  // 0 = no env loaded
m_nrdTonemap->dispatch(cmd, ti);
```

**Verify** `m_envMapView` / `m_envMapSampler` members exist on PathTracer. If not (env map is in bindless set 12), expose via a new setter or use the same views raygen uses.

- [ ] **Barrier binding 20 (depth AOV) to GENERAL→SHADER_READ before tonemap dispatch**

In `path_tracer_render.cpp` — the input barrier block before compose already transitions bindings 24/25/27/28. Add binding 20 to that list (raygen writes it earlier; tonemap now reads it).

### Step 1.4 — Build + verify

- [ ] **Build clean ON + OFF**

```bash
cmake --build build -j8 2>&1 | tail -3
cmake --build build-nonrd -j8 --target cornell_box 2>&1 | tail -3
```

- [ ] **Visual verification**

```bash
mkdir -p renders/4f_t1
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_t1/nrd_env_composited.png 1 --denoise=nrd 2>&1 | tail -3
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_t1/oidn_reference.png 1 --denoise=oidn 2>&1 | tail -3
```

Open `nrd_env_composited.png` — figure on LIT env background, not black. Compare with `oidn_reference.png` — env should look similar (both sample same HDR).

### Step 1.5 — Commit T1

- [ ] **Commit**

```bash
git add shaders/rt/nrd_tonemap.comp \
        ohao/render/rt/denoise/nrd_tonemap.hpp ohao/render/rt/denoise/nrd_tonemap.cpp \
        ohao/render/rt/path_tracer_render.cpp \
        ohao/render/rt/rt_profile_renderer.hpp
git commit -m "feat(rt): env composite in NRD tonemap (Sub-plan 4.F T1)"
```

---

## Task 2: Motion vector + view-change correctness

**Goal:** Interactive orbit has no ghosting trails; disocclusion behavior correct.

**Spec reference:** §3.2 + §6.2

### Step 2.1 — Interactive viewport signals view changes

- [ ] **Call renderer.notifyViewChanged() on camera input**

In `examples/interactive.cpp`, find the camera-update block (where mouse drag / WASD processes each frame). After `camera->update()` (or equivalent), if any input caused motion:

```cpp
if (cameraMoved) {  // however the camera class reports this; or just always after input events
    renderer.notifyViewChanged();
}
```

If the camera class doesn't expose "did move", call `notifyViewChanged` unconditionally every frame. NRD's frameIndex reset on view change is handled downstream — worst case we get spatial-only denoising more often than necessary, but no visual regression.

### Step 2.2 — NRD frameIndex=0 on view change

- [ ] **In path_tracer_render.cpp, bootstrap NRD on view change**

Find the line `camera.frameIndex = m_historyFrameCount;`. Replace with:
```cpp
camera.frameIndex = m_viewChangedThisFrame ? 0 : m_historyFrameCount;
```

When `m_viewChangedThisFrame == true`, prev matrices should also be identity (or the current V/P — NRD treats frameIndex=0 as "ignore history"). Since 4.E T2 I1 already resets m_prevViewMatrix/m_prevProjMatrix on resetAccumulation/resize, and `m_viewChangedThisFrame` doesn't currently trigger resetAccumulation (grep to verify), add an explicit reset:

```cpp
if (m_viewChangedThisFrame) {
    std::memcpy(camera.viewMatrixPrev.data(), glm::value_ptr(viewM), sizeof(float) * 16);
    std::memcpy(camera.projMatrixPrev.data(), glm::value_ptr(projM), sizeof(float) * 16);
}
```

### Step 2.3 — MV audit

- [ ] **Confirm pc.prevViewProj is captured at END of render**

Grep for `m_prevViewProj =` in path_tracer_render.cpp. Confirm it assigns AFTER the trace/dispatch. If it's at the beginning (capturing this frame's VP for this frame's MV), that's a bug — fix by moving to end.

- [ ] **Build + verify**

```bash
cmake --build build -j8 2>&1 | tail -3
timeout 15 ./build/interactive assets/realistic_female.glb assets/test_models/env_studio.hdr --denoise=nrd 2>&1 | grep -iE "NRD|denoise" | head -10
```

Visual test: orbit camera around quickly; release; the scene should NOT show ghosting trails.

### Step 2.4 — Commit T2

- [ ] **Commit**

```bash
git add examples/interactive.cpp ohao/render/rt/path_tracer_render.cpp
git commit -m "feat(rt): view-change signaling + NRD bootstrap (Sub-plan 4.F T2)"
```

---

## Task 3: Multi-spp AOV accumulation (offline)

**Goal:** `env_demo --denoise=nrd --spp=N` gives NRD N-spp averaged input → much cleaner output.

**Spec reference:** §3.3 + §6.3

### Step 3.1 — Add push-constant flag for AOV accumulation

- [ ] **In path_tracer_render.cpp, set bit when NRD mode active**

Find the `pc.control.x |= kPTFlagEnableAOVs;` line. Add:
```cpp
// Sub-plan 4.F T3: NRD mode accumulates AOVs over N samples for cleaner input
if (m_renderSettings.denoiseMode == DenoiseMode::NRD) {
    pc.control.x |= kPTFlagAccumulateAOVs;
}
```

Define `kPTFlagAccumulateAOVs` (likely `1 << 3` — check the existing constants in path_tracer_render.cpp):
```cpp
constexpr uint32_t kPTFlagAccumulateAOVs = 1u << 3;   // must match raygen
```

### Step 3.2 — Raygen AOV accumulation

- [ ] **Each of the 5 AOV imageStore sites in pt_raygen.rgen gets accumulation branch**

In `shaders/rt/pt_raygen.rgen`, find the 5 AOV imageStore sites (bindings 22, 23, 24, 25, 26). For each, wrap with:
```glsl
if ((pc.control.x & kPTFlagAccumulateAOVs) != 0 && pc.params.z > 0) {
    vec4 prev = imageLoad(diffRadAOV, pixel);
    float n = float(pc.params.z + 1);
    vec4 running = mix(prev, vec4(newValue, hitDist), 1.0 / n);
    imageStore(diffRadAOV, pixel, running);
} else {
    imageStore(diffRadAOV, pixel, vec4(newValue, hitDist));
}
```

(`pc.params.z` is sampleIndex per path_tracer existing convention.)

- [ ] **Mirror changes to pt_raygen_offline.rgen + pt_raygen_realtime.rgen**

Identical edits. Realtime never sets the flag (stays at overwrite) but shader stays compatible.

### Step 3.3 — Build + verify

- [ ] **Build**

```bash
cmake --build build --target shaders env_demo -j8 2>&1 | tail -3
```

- [ ] **Visual verification — spp progression**

```bash
mkdir -p renders/4f_t3
for spp in 1 4 16 64; do
    ./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_t3/nrd_spp${spp}.png ${spp} --denoise=nrd 2>&1 | tail -2
done
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_t3/none_spp64.png 64 --denoise=none 2>&1 | tail -2
```

Expected: `nrd_spp16.png` is noticeably cleaner than `nrd_spp1.png`, and close to `none_spp64.png` quality.

### Step 3.4 — Commit T3

- [ ] **Commit**

```bash
git add shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen shaders/rt/pt_raygen_realtime.rgen \
        ohao/render/rt/path_tracer_render.cpp
git commit -m "feat(rt): AOV accumulation for NRD multi-spp input (Sub-plan 4.F T3)"
```

---

## Task 4: Pixel jitter + ReblurSettings tuning

**Goal:** Realtime interactive 1spp looks close to static PT 64spp after ~30 frames. Production-quality settings.

**Spec reference:** §3.4 + §6.4

### Step 4.1 — Halton jitter in PathTracer

- [ ] **Add jitter members to path_tracer.hpp**

```cpp
// Sub-plan 4.F T4: TAA-style pixel jitter for NRD temporal sample diversity
glm::vec2 m_jitterCurrent{0.0f};
glm::vec2 m_jitterPrev{0.0f};
uint32_t  m_haltonIndex{0};
```

- [ ] **Halton sequence utility in render()**

In `path_tracer_render.cpp`, at the top of render():
```cpp
// Halton(2, 3) for pixel jitter
auto halton = [](uint32_t i, uint32_t base) -> float {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= base; r += f * (i % base); i /= base; }
    return r;
};
// Sub-plan 4.F T4: ±0.5 pixel jitter for NRD mode only
if (m_renderSettings.denoiseMode == DenoiseMode::NRD) {
    const uint32_t idx = (m_haltonIndex++ % 16) + 1;  // skip index 0 (always 0 for Halton)
    m_jitterCurrent = glm::vec2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
} else {
    m_jitterCurrent = glm::vec2(0.0f);
}
```

- [ ] **Push jitter to raygen via existing push constant (pc.tuning or similar) + feed to NRD**

Raygen needs to offset ray origin by `jitter / extent` in NDC. Add jitter to `PTPushConstants.tuning` or a new field. In raygen shader, inject into pixel-to-NDC conversion.

NRD side — populate `camera.jitter` / `camera.jitterPrev`:
```cpp
camera.jitter     = {m_jitterCurrent.x, m_jitterCurrent.y};
camera.jitterPrev = {m_jitterPrev.x,    m_jitterPrev.y};
```

At end of render:
```cpp
m_jitterPrev = m_jitterCurrent;
```

### Step 4.2 — Raygen pixel jitter

- [ ] **Add jitter to pixel center in all 3 raygens**

In `shaders/rt/pt_raygen.rgen` + siblings, find the pixel-to-NDC conversion. It looks like:
```glsl
vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);
vec2 uv = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
vec2 ndc = uv * 2.0 - 1.0;
```

Add jitter:
```glsl
vec2 jitter = vec2(pc.tuning.z, pc.tuning.w);  // unpack from push const
vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + vec2(0.5) + jitter;
```

### Step 4.3 — NrdDenoiser production ReblurSettings

- [ ] **Add setReblurSettings method**

In `ohao/render/rt/denoise/nrd_denoise.hpp`:
```cpp
/// Sub-plan 4.F T4: production-tuned REBLUR parameters.
struct NrdReblurProfile {
    float hitDistanceParamA = 3.0f;
    float diffusePrepassBlurRadius = 30.0f;
    float specularPrepassBlurRadius = 50.0f;
    uint32_t historyFixFrameNum = 3;
    uint32_t maxAccumulatedFrameNum = 63;
    uint32_t maxFastAccumulatedFrameNum = 8;
    float antilagIntensitySigmaScale = 2.0f;
    float antilagHitDistanceSigmaScale = 2.0f;
    bool enableMaterialTest = true;
};

void setReblurSettings(const NrdReblurProfile& profile);
```

In `nrd_denoise.cpp`:
```cpp
void NrdDenoiser::setReblurSettings(const NrdReblurProfile& p) {
    if (!m_impl->integrationReady) return;
    nrd::ReblurSettings s{};
    s.hitDistanceParameters.A = p.hitDistanceParamA;
    s.diffusePrepassBlurRadius = p.diffusePrepassBlurRadius;
    s.specularPrepassBlurRadius = p.specularPrepassBlurRadius;
    s.historyFixFrameNum = p.historyFixFrameNum;
    s.maxAccumulatedFrameNum = p.maxAccumulatedFrameNum;
    s.maxFastAccumulatedFrameNum = p.maxFastAccumulatedFrameNum;
    s.antilagSettings.intensitySigmaScale = p.antilagIntensitySigmaScale;
    s.antilagSettings.hitDistanceSigmaScale = p.antilagHitDistanceSigmaScale;
    s.enableMaterialTestForDiffuse = p.enableMaterialTest;
    s.enableMaterialTestForSpecular = p.enableMaterialTest;

    const nrd::Identifier id = 0;  // REBLUR_DIFFUSE_SPECULAR denoiser identifier
    nrd::SetDenoiserSettings(*m_impl->integration.GetInstance(), id, &s);
}
```

(Verify NRD v4.17 API: `GetInstance()` returns `nrd::Instance*`; `SetDenoiserSettings(instance&, identifier, settings_ptr)`. Check exact signature in vendored `NRD.h`.)

- [ ] **Call setReblurSettings from PathTracer after initialize**

In `path_tracer.cpp`'s `m_nrdDenoiser = std::make_unique<NrdDenoiser>(); ... initialize(...)` block:
```cpp
if (m_nrdDenoiser->initialize(...)) {
    ohao::NrdReblurProfile profile{};  // production defaults
    m_nrdDenoiser->setReblurSettings(profile);
    std::cout << "[NRD] persistent instance ready @ ..." << ...;
}
```

### Step 4.4 — Build + verify

- [ ] **Build**

```bash
cmake --build build -j8 2>&1 | tail -3
```

- [ ] **Interactive visual verification**

```bash
timeout 30 ./build/interactive assets/realistic_female.glb assets/test_models/env_studio.hdr --denoise=nrd 2>&1 | head -15
```

Hold camera still for 5-10 seconds — scene should stabilize and look clean. Orbit slowly — should remain clean. Orbit fast — disocclusion artifacts expected, but no ghosting.

### Step 4.5 — Commit T4

- [ ] **Commit**

```bash
git add shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen shaders/rt/pt_raygen_realtime.rgen \
        ohao/render/rt/denoise/nrd_denoise.hpp ohao/render/rt/denoise/nrd_denoise.cpp \
        ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp ohao/render/rt/path_tracer_render.cpp
git commit -m "feat(rt): pixel jitter + production ReblurSettings (Sub-plan 4.F T4)"
```

---

## Final Verification (after all 4 tasks)

- [ ] **Capture final comparison PNGs**

```bash
mkdir -p renders/4f_final
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_final/nrd_spp4.png 4 --denoise=nrd 2>&1 | tail -2
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_final/nrd_spp16.png 16 --denoise=nrd 2>&1 | tail -2
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_final/oidn_spp1.png 1 --denoise=oidn 2>&1 | tail -2
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4f_final/none_spp64.png 64 --denoise=none 2>&1 | tail -2
```

- [ ] **Update verification log**

Append 4.F T1/T2/T3/T4 entries + final PNG references in `tests/reference_scenes/custom/envlit_turntable/verification_log.md`.

- [ ] **Update CLAUDE.md**

Note 4.F shipped — env blend live, production ReblurSettings, jitter. One paragraph.

---

## Done

All 4 tasks merged → `--denoise=nrd` is at "AAA-feel" quality. Remaining gap to Alan Wake 2 class: ReSTIR DI (4.G) + DLSS RR (Phase 5).
