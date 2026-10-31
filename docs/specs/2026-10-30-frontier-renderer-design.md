# The frontier renderer: retiring the legacy

**Status:** accepted (Frank, 2026-10-30: every ruling as recommended; F-R3 yes, add the DLSS SDK; F-R6 keep the fallback). Steps F0 to F4 are built (`docs/changes/2026-10-30-frontier-f0.md` to `2026-10-30-frontier-f4.md`; F4's cost misses four gated frame budgets, a ruling pending); F5 is in progress (F5a built, `docs/changes/2026-10-31-frontier-f5a.md`). F2 moved the constant ambient to F5 (with SSAO).
Frank, 2026-10-30: "any old code prohibits our frontier pacing that we need to remove it or retire it".

## 1. Why

OpenPBR step 1 (`docs/changes/2026-10-29-openpbr-step1.md`) put one surface model under the offline path tracer, deferred
lighting and RT GI. Around it, the renderer still carries several generations of older techniques. Each one is
something a frontier renderer does differently, and several are wrong in ways the referee cannot see:

- **Two path tracers.** The realtime profile is a separate 1,883-line GLSL raygen
  (`shaders/rt/pt_raygen_realtime.rgen`) on the pre-OpenPBR metal-rough BRDF (`ggx_aniso.glsl`, `pbr_unpack.glsl`).
  - Its closest hit reads material rows 0–2 only.
  - It carries ReSTIR GI (2021) with an unreachable legacy branch.
  - It also carries its own spatial denoiser and a firefly clamp.
- **Non-physical fills:**
  - a constant ambient of 0.35 when there is neither RT GI nor an environment;
  - SSAO multiplied over RT GI's specular;
  - `EmissiveProxyLight`: an emissive texture becomes a point light of intensity `min(power × 0.1, 20)` at the mesh's
    box centre, deferred only.
- **A display chain of fixed guesses:**
  - an ACES fit (Narkowicz), copied four times;
  - `pow(x, 1/2.2)` instead of the sRGB transfer function;
  - a fixed exposure of 0.6 (the path tracer uses 0.5);
  - a thresholded bloom clamped at luminance 4.
- **A Preetham sky with a procedural night.** It lights nothing.
- **One GI bounce at half resolution.** That is the referee's remaining gap: Cornell 0.881, spheres 0.767.
- **Direct lighting as a loop over every light in the froxel**, at 0.682 ms for Cornell's 12 lights. Only 8 lights can
  own an RT shadow channel.
- **GLSL beside Slang.** 30 GLSL stages and the generated `layout.glsl` remain.

## 2. What the frontier is (2026)

Sources were checked on 2026-10-30. Each entry is what this spec adopts.

- **Direct light: stochastic, ray-traced, resampled.**
  - Unreal Engine 5's MegaLights (Narkowicz and Costa, SIGGRAPH 2025, "Advances in Real-Time Rendering", "MegaLights:
    Stochastic Direct Lighting in Unreal Engine 5") targets 100+ shadowed, fully dynamic area lights with complex
    BRDFs. It works by sampling, tracing, shading and denoising, not by a per-pixel loop over lights.
  - The resampling is ReSTIR DI (Bitterli et al., SIGGRAPH 2020, "Spatiotemporal reservoir resampling for real-time
    ray tracing with dynamic direct lighting").
- **Indirect light: a world-space radiance cache behind a short path.**
  - NVIDIA's RTXGI 2.0 replaces probe grids with two world-space caches. The Spatially Hashed Radiance Cache (SHaRC)
    runs on any ray-tracing GPU. The Neural Radiance Cache (NRC; Müller et al., SIGGRAPH 2021, "Real-time Neural
    Radiance Caching for Path Tracing") needs tensor cores and is experimental.
  - RTX Path Tracing 1.9 turns SHaRC on by default ("more indirect lighting at about the same cost").
  - ReSTIR GI (Ouyang et al., HPG 2021) resamples secondary-hit radiance. ReSTIR PT (Lin et al., SIGGRAPH 2022,
    "Generalized Resampled Importance Sampling") reuses whole paths and is the research frontier.
- **Denoising:**
  - NVIDIA's NRD (ReBLUR and ReLAX) is vendor neutral, and OHAO already integrates it.
  - The neural denoisers have overtaken the heuristic ones: DLSS Ray Reconstruction, and AMD's FSR Redstone Ray
    Regeneration (RX 9000 only, 2025). Tests in Call of Duty: Black Ops 7 put DLSS RR first in stills and FSR's ahead
    in motion, both at under 3% of frame rate.
- **Sky:** Hillaire (EGSR 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"), as in
  Unreal's SkyAtmosphere. Already accepted (Frank, 2026-10-29).
- **Display:**
  - AgX is the neutral filmic default in Blender 4 and Filament, with little hue shift.
  - Khronos PBR Neutral keeps base colours faithful for material review. Khronos itself recommends filmic curves for
    strongly HDR scenes.
  - The ACES fit OHAO uses is the one Filament keeps only for backward compatibility.
- **Sampling:** Owen-scrambled Sobol (Burley, JCGT 2020, "Practical Hash-based Owen Scrambling") for both path tracer
  profiles.

## 3. The steps

Each step deletes what it replaces, in the same commit, and ends with every gate green. "Forward only": no flag keeps an
old path alive.

| Step | Builds | Retires |
|---|---|---|
| **F0** (built) | nothing new; the CSM fallback and the sky follow the scene's sun (both were stuck at straight down) | dead includes (`includes/common/*`, `light_attenuation.glsl`, a duplicate `pbr_unpack.glsl`), `GenerateShaderConstants.cmake`, `gpu/vulkan/material*` and `material_instance*` (no engine user), `render/async/async_compute_queue*` (no caller), the unbuilt `renderer_pipeline_tests.cpp`, the Python renderer bindings |
| **F1** one path tracer | the realtime profile runs `path_tracer.slang` (OpenPBR): few bounces, SHaRC terminating paths into the cache, the denoiser guides NRD and DLSS RR need, reprojected history; Owen-scrambled Sobol in both profiles | `pt_raygen_realtime.rgen` and its GLSL hit shaders, `ggx_aniso.glsl`, `pbr_unpack.glsl`, the GLSL sampler and its tables, `mis.glsl`, `pt_lights.glsl`, `env_sampling.glsl`, the old ReSTIR GI, `pt_spatial_denoise`, the SVGF `atrous` denoiser, NRD's "Legacy 4.B" branch, the dead anisotropy and SSS overrides, the duplicate profile metadata (`IRTRenderPipeline`/`RTProfileTraits` beside `rt_settings.hpp`) |
| **F2** the display chain | Hillaire sky and aerial perspective as the default environment (it lights, through the same paths an HDRI does); physical exposure (EV100) with histogram auto exposure; AgX by default, PBR Neutral selectable; the sRGB transfer function; one tonemap shared by every path; bloom by energy (no threshold) | `sky.frag` (Preetham, the procedural night), the constant ambient, the four ACES copies, `pow(1/2.2)`, the fixed 0.6 and 0.5 exposures, the bloom threshold and clamp |
| **F3** emission is light | emissive triangles are lights: power-weighted selection over emissive triangles in the path tracer, and in the deferred frame through RT GI's bounce (which brings back what it hits emits; F4's ReSTIR DI gives deferred direct emissive light), so a lamp's texture lights the room it is in | `EmissiveProxyLight`, `emissiveProxy()` |
| **F4** direct light | ReSTIR DI in the hybrid frame (MegaLights' structure: sample, trace, shade, denoise), one shadow ray a sample, any number of shadowed lights, area lights exact | the per-pixel loop over the froxel's lights for shadowed light, the 8 shadow channels, `shadowLightsDropped` |
| **F5** indirect light | SHaRC behind RT GI for every bounce; ReSTIR GI on the first; RT reflections through the same paths | one-bounce GI, SSAO in lighting (the material's AO texture stays), SSR, the split sum for occluded specular |
| **F6** Slang everywhere | the G-buffer, TAA, bloom, the gizmo and the remaining compute in Slang | every GLSL stage, `layout.glsl` |

## 4. Rulings proposed (Frank)

- **F-R1, one path tracer:** the realtime profile is the reference tracer at budgeted settings, and the GLSL ReSTIR GI
  goes. Recommended: the old one is a 2021 algorithm on the pre-OpenPBR BRDF. RTXPT's structure (short paths, SHaRC,
  DLSS RR) is the current reference. ReSTIR PT comes after SHaRC, if the referee asks for it.
- **F-R2, the radiance cache:** SHaRC first (any RT GPU, a hash table and two passes), NRC later as an option on
  tensor cores. Recommended.
- **F-R3, the neural denoiser:** add the DLSS SDK (`external/DLSS`, NVIDIA's public SDK and its license; it is absent
  today, so `--denoise dlss` is a stub) for DLSS RR on NVIDIA. NRD stays the vendor-neutral default. Recommended: this
  machine is an RTX 5070.
- **F-R4, the tonemap:** AgX by default; PBR Neutral for material review and the referee's PNGs. Recommended.
- **F-R5, exposure:** a physical camera (EV100, ISO, shutter, aperture, as in Lagarde and de Rousiers, SIGGRAPH 2014,
  "Moving Frostbite to PBR") with histogram auto exposure in windows. Captures keep a fixed EV, so goldens stay
  deterministic. Recommended.
- **F-R6, ray tracing required?** Keep CSM and the no-ray-query path as the fallback (now lit by the scene's sun), and
  keep it correct but not extended. Recommended for now: Call of Duty ships on GPUs without ray tracing. The
  alternative is to require `VK_KHR_ray_query` and delete CSM.
- **F-R7, emissive triangles:** both renderers sample emissive triangles as lights, by power, through one table;
  proxies go. Recommended.
- **F-R8, order:** F1 to F6 as listed. F1 first, because the second path tracer is the largest piece of old code and
  the referee must not have two answers. F4 before F5, because direct light is the larger and costlier term (Cornell's
  `lighting` at 0.682 ms).

## 4a. Corrections after the rulings (2026-10-30)

- **SHaRC's code cannot be vendored.** RTXGI, which contains SHaRC, is under the "NVIDIA RTX SDKs License": section 4(e)
  bars making the SDK subject to an open source license, for example by distributing it in source form. The repo is
  MIT. So F-R2 is built as OHAO's own Slang hash-grid radiance cache with SHaRC's design:
  - a camera-relative, logarithmically levelled hash grid of radiance;
  - updates from a sparse set of short paths, accumulated in integers;
  - a resolve pass that blends across frames and evicts stale entries;
  - paths terminated into the cache past their first bounce.

  The published sources are Binder et al., "Fast Path Space Filtering by Jittered Spatial Hashing" (SIGGRAPH 2019
  talks), and Gautron, "Real-Time Ray-Traced Ambient Occlusion of Complex Scenes using Spatial Hashing" (SIGGRAPH 2020
  talks), with NVIDIA's SharcGuide as the reference design. Integer accumulation keeps the shader-float-atomics lint
  and captures deterministic.
- **The DLSS SDK is never committed.** It is under the same license: binaries and headers in a git-ignored
  `external/DLSS`, fetched by a script, behind `OHAO_DLSS`. The supplement asks for attribution (the DLSS mark in
  credits) and a notice to NVIDIA before a commercial release. NRD is already fetched at configure time, never vendored.

## 5. What stays out

- **Upscaling** (DLSS SR, FSR): sub-project 16.
- **The visibility buffer:** ruling O3.
- **OpenPBR's coat, transmission, sheen and thin film:** OpenPBR step 2. It builds on F1, since there is one tracer
  to extend.
