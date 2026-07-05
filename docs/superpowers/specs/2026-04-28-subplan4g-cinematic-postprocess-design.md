# Sub-plan 4.G — Cinematic Post-Process Pipeline — Design

**Date:** 2026-04-28
**Status:** Approved — autonomous execution under "loop until cinematic" directive.
**Phase:** Photorealism push, Axis 1 (biggest visual lever).

---

## 1. Goal

Transform `--denoise=nrd` (and all other denoise modes) output from "viewport screenshot" into "cinematic frame" by adding a post-process chain after the existing tonemap path.

Concrete deliverable: a frame with the same geometry that currently looks like a tech demo now looks like a Blender Cycles still — filmic contrast, soft highlight bloom, vignetted corners, slight color grade.

## 2. Non-Goals

- ReSTIR DI (4.H).
- Scene composition / asset upgrades (4.I).
- Auto-exposure (manual EV in push constant; full auto-exposure histogram pass = future).
- Lens flare, chromatic aberration, film grain (parked — diminishing visual returns vs. impl cost).
- Replacing OHAO's existing deferred-pipeline post (this is RT-mode + NRD only).

## 3. Decisions

### 3.1 Architecture: one composite shader, multi-pass bloom

Two new compute shaders + new HDR intermediate binding:

```
Existing:  NRD composed (29 RGBA32F HDR)  →  nrd_tonemap.comp  →  binding 30 RGBA8 (tonemapped)

New 4.G:   NRD composed (29 RGBA32F HDR)
              ↓
              ├─ cinematic_bloom_extract.comp → bloom_threshold (binding 31, RGBA16F)
              ├─ cinematic_bloom_blur.comp ×3 (separable Gaussian, 3 mip levels)
              ↓
           cinematic_composite.comp (replaces nrd_tonemap.comp):
              - reads binding 29 (main HDR) + bloom buffer + depth (sky composite, T1 4.F)
              - applies filmic tonemap (AgX or Hable)
              - applies vignette
              - applies subtle color grade (LUT or analytic)
              - writes binding 30 (RGBA8 final)
```

`nrd_tonemap.comp` is **replaced** by `cinematic_composite.comp`. NrdTonemap class becomes NrdCinematicPost (or stays named NrdTonemap with broader scope — naming TBD in T1).

### 3.2 Tonemap curve: AgX

Replace ACES with AgX. AgX has better shadow rolloff, less saturation crush in highlights, more film-like contrast. The OCIO AgX implementation is short and self-contained — 50 lines of GLSL. ACES is too "video" for cinematic.

AgX reference (the simplified path-tracer-friendly version):
```glsl
// AgX log curve fit, no 3D LUT (matches OCIO AgX Base contrast)
const mat3 AgXInsetMatrix = mat3(
    0.856627153315983, 0.137318972929847, 0.11189821299995,
    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
    0.0482516061458583, 0.101439036467562, 0.811302368396859);
// ... (full impl in shader)
```

### 3.3 Bloom: 3-level mip-chain Gaussian

Standard physically-plausible bloom:
1. **Threshold pass**: extract pixels above `1.0` HDR luminance, downsample 2× (half-res RGBA16F).
2. **Blur passes**: separable Gaussian on each mip level (sigma scales with mip).
3. **Combine in composite**: weighted sum of mip levels, added to main HDR before tonemap.

3-mip is enough for game-quality bloom (Doom Eternal uses 6, Frostbite uses 12 — we settle for 3 to keep dispatch count low).

### 3.4 Vignette: analytic

Radial darkening at corners. Simple `1 - smoothstep(0.3, 0.9, length(uv - 0.5)) * 0.5` after tonemap. Strength configurable.

### 3.5 Color grade: subtle warm contrast lift

Single-pass analytic grade in tonemap-output space:
- Slight saturation boost (×1.1)
- Slight contrast curve (gain ×1.08, lift +0.02)
- Warm tint (multiply RGB by `vec3(1.02, 1.00, 0.98)`)

No LUT for 4.G — analytic is cheap and enough.

### 3.6 Apply path: NRD mode + non-NRD modes

The cinematic composite shader replaces the NRD tonemap path. But the non-NRD modes (None/OIDN/OptiX) currently use raygen's in-shader ACES tonemap to write binding 2 directly. Two options:

- **A. Cinematic only for NRD mode.** Other modes keep their existing tonemap; non-NRD output looks unchanged. Simpler, but creates a visible quality split between modes.
- **B. Cinematic for all modes.** Raygen still writes HDR to accum buffer; cinematic composite runs unconditionally after raygen-or-NRD; final output to binding 30. All modes look cinematic. Bigger change — raygen needs to stop tonemapping internally.

Pick **B** in spirit but ship **A** for 4.G scope. 4.G's success = cinematic NRD. 4.H+ can broaden if user wants OIDN to also look cinematic (likely yes, but separate task).

### 3.7 Push-constant control

Cinematic composite shader push constants:
```glsl
layout(push_constant) uniform CinematicPC {
    mat4 invView;          // for sky reconstruction (preserves T1)
    mat4 invProj;
    vec2 extent;
    float envIntensity;
    float exposure;        // EV-style multiplier on HDR before tonemap, default 1.0
    float bloomStrength;   // 0..1, default 0.6
    float vignetteStrength;// 0..1, default 0.4
    float saturation;      // default 1.1
    float contrast;        // gain, default 1.08
    vec3  tint;            // RGB multiplier, default {1.02, 1.00, 0.98}
    float _pad;
};
```

Engine pre-seeds with defaults; future ImGui panel (out of 4.G scope) can tune.

## 4. Architecture

### 4.1 Component map

```
PathTracer (after 4.F):
  ├─ m_nrdDenoiser     (4.C)
  ├─ m_nrdCompositor   (4.D)
  ├─ m_nrdTonemap      (4.E) ← will become or be wrapped by m_cinematic
  └─ m_nrdComposedImage/View/Memory                  (binding 29)
  └─ m_nrdTonemappedImage/View/Memory                (binding 30)

New 4.G:
  └─ m_cinematicBloomImage/View/Memory               (binding 31, RGBA16F half-res)
  └─ m_cinematicBloomMips[3]                         (mip chain for separable blur)

NrdCinematicPost (renamed/expanded NrdTonemap) class
  ├─ initialize       — creates 3 compute pipelines (extract/blur/composite)
  ├─ shutdown
  └─ dispatch(VkCommandBuffer, NrdCinematicInputs)
        - bloom extract (HDR → mip0 threshold)
        - bloom blur Gaussian H + V × 3 mip levels
        - composite (combine + filmic + vignette + grade)
```

### 4.2 Per-frame flow (NRD mode after 4.G)

```
PathTracer::render()
  ├─ raygen → AOVs (unchanged)
  ├─ NRD denoise → bindings 27/28 (unchanged)
  ├─ NRD compose → binding 29 HDR (unchanged)
  ├─ NEW: cinematic bloom chain → bloom mip chain
  ├─ NEW: cinematic composite (replaces tonemap) → binding 30 RGBA8 final
  └─ readback path via getPixels() (unchanged — still reads binding 30)
```

env_demo / interactive transparently pick up the cinematic output through the existing `getPixels()` NRD branch.

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `shaders/rt/cinematic_bloom_extract.comp` (NEW) | HDR → half-res threshold + downsample |
| `shaders/rt/cinematic_bloom_blur.comp` (NEW) | Separable Gaussian (push const for axis: 0=H, 1=V) |
| `shaders/rt/cinematic_composite.comp` (NEW) | Replaces nrd_tonemap.comp logic + adds bloom combine + filmic + vignette + grade |
| `shaders/rt/nrd_tonemap.comp` | DELETED — superseded by cinematic_composite |
| `ohao/render/rt/denoise/nrd_tonemap.{hpp,cpp}` | Renamed → `nrd_cinematic.{hpp,cpp}`. Class `NrdTonemap` → `NrdCinematicPost`. Holds 3 compute pipelines instead of 1. |
| `ohao/render/rt/path_tracer.hpp` | `m_nrdTonemap` → `m_cinematicPost`. New bloom-mip image members. |
| `ohao/render/rt/path_tracer_images.cpp` | Allocate bloom mip chain (3 levels). Cleanup. |
| `ohao/render/rt/path_tracer_render.cpp` | Dispatch chain: extract → 3× blur → composite. Push-constant defaults. |
| `ohao/render/rt/rt_profile_renderer.hpp` | Renamed accessor virtuals if needed. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | 4.G entry with before/after PNG pair |
| `CLAUDE.md` | Note 4.G cinematic post shipped |

### 5.2 What does NOT change

- NRD denoise / compose pipelines (4.C/4.D sealed).
- Raygen shaders (no shader-side change).
- Binding 29 HDR remains the source — cinematic pipeline reads it.
- Other denoise modes (None/OIDN/OptiX) — out of scope; they keep current tonemap.
- `getPixels()` NRD branch — still reads binding 30.

## 6. Verification

`renders/4g/`:
- `nrd_before_4g.png` — current 4.F output for reference (regen from existing build)
- `nrd_after_4g.png` — post-4.G with full cinematic chain
- `nrd_4g_bloom_only.png` — bloom strength 1, vignette/grade off (isolate bloom contribution)
- `nrd_4g_grade_only.png` — bloom off, grade+vignette on
- `nrd_4g_full.png` — production defaults

Acceptance: side-by-side `nrd_before_4g.png` vs `nrd_4g_full.png` must show a dramatic visual improvement — cinematic curve, soft glow on highlights, darkened corners, warm cast. If it still looks like a tech demo, T-task 4.G failed and we iterate inside this sub-plan before moving to 4.H.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| AgX implementation has subtle math errors → wrong colors | Reference Troy Sobotka's GLSL port; copy verbatim with attribution |
| 3-mip bloom too low-frequency → bloom looks chunky | Tune sigma per mip; if needed bump to 4 mips |
| Push-constant size hits 256-byte Vulkan limit | Compute layout: 64+64+8+4+4+4+4+4+12+4 = 172 bytes. Safe. |
| Bloom dispatch count adds frame time → fps regression in interactive | 1 extract + 6 blur (3 levels × H+V) + 1 composite = 8 dispatches at half-res = cheap. <0.5ms on modern GPU. |
| Cinematic composite breaks env-sky path from 4.F T1 | composite shader retains the depth-sentinel branch; identical logic preserved |

## 8. Success Criteria

1. New 3 compute shaders build to SPV.
2. NrdCinematicPost class compiles ON + OFF.
3. Binding 31 bloom mip chain allocated + cleaned up.
4. `env_demo --denoise=nrd` renders with visible bloom on highlights + vignetted corners + filmic contrast.
5. Side-by-side before/after PNG pair shows dramatic improvement.
6. Beauty path for non-NRD modes unchanged.
7. OHAO_NRD=OFF still builds clean (cinematic shader compiles unconditionally; pipeline init guarded).

## 9. Task Shape

Single big task (multi-shader + class rename + plumbing). Subdivide if implementer estimates > 1 hour:
- **T1**: Shaders + NrdCinematicPost class skeleton (3 pipelines, push constants, dispatch ordering)
- **T2**: PathTracer wiring (bloom mip images, dispatch chain in render(), barriers)
- **T3**: Verification renders + log entry

## 10. Next Step

Implementation plan: minimal — the spec is detailed enough. Single combined plan file with 3 tasks.
