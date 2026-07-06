# OHAO Engine

A solo Vulkan 1.3 renderer with a hybrid pipeline: KHR ray tracing for path-traced reference and indirect lighting, deferred raster for real-time. Pure C++, no editor framework.

<p align="center">
  <a href="docs/media/helmet_orbit.mp4">
    <img src="docs/media/helmet_orbit.gif" width="640" alt="OHAO Engine — real-time path-traced helmet orbiting in an outdoor HDRI scene" />
  </a>
</p>

<p align="center"><sub><em>Real-time path tracing at ~67 fps — DLSS Ray Reconstruction · ReSTIR GI · cinematic grade. A full 360° orbit, rendered live — click for the <a href="docs/media/helmet_orbit.mp4">▶ full-quality MP4</a>.</em></sub></p>

## What it is

I started this in November 2024 to learn how a modern hybrid renderer is wired end to end. 18 months later it's 52K lines of C++ and 14K lines of GLSL across 121 shaders, with two pipelines that share scene, materials, and acceleration structures. No engine SDK, no editor host. Path tracer is for ground truth. Deferred is for interactive iteration. The hybrid mode runs RT shadows and 1-bounce RT GI on top of the deferred G-buffer.

## Headline numbers

| Metric | Value |
|---|---|
| Commits | 501 (Nov 2024 - Apr 2026, solo) |
| C++ source | 52,594 LOC across `ohao/` |
| GLSL shaders | 14,738 LOC across 121 files |
| Render code | 23,780 LOC, 89 files |
| GPU/Vulkan layer | 9,021 LOC, 19 files |
| Physics (Jolt) | 10,989 LOC, 51 files |
| Scene graph | 7,042 LOC, 35 files |
| Denoise backends | 4 (Intel OIDN, NVIDIA OptiX, NVIDIA NRD, NVIDIA DLSS-RR) |

## Real-time path tracing — and the firefly that taught me the most

The most recent stretch pushed the interactive viewer from noisy preview to real-time path tracing at 67 fps (1 spp, 720p), denoised with NVIDIA DLSS Ray Reconstruction and stabilized with ReSTIR GI. The fix I got the most out of, though, was the smallest one.

![Outdoor HDRI scene, graded](docs/images/hero_outdoor_graded.jpg)

### The 1-spp firefly storm — importance sampling + MIS

**Symptom.** At one sample per pixel, metal and ground sparkled: bright pixels flashing at random every frame. Denoising couldn't touch it — the denoiser was being fed garbage.

**Cause.** The real-time integrator reached the bright HDRI environment *only* through random BSDF bounces. A rare ray that happened to land on the sun returned `radiance / (tiny pdf)` — a colossal spike. That's unbounded Monte-Carlo variance, by construction. The offline path already importance-sampled the environment; the real-time fork I'd split off didn't.

**Fix.** Importance-sample the environment by its own luminance (marginal + conditional CDF), shadow-trace, and MIS-weight against BSDF sampling with the balance heuristic:

```glsl
// shaders/rt/pt_raygen_realtime.rgen
sampleEnvMap(u1, u2, ..., out envDir, out envPdf);   // sample the sky where it's bright
if (dot(N, envDir) > 0.0 && envPdf > 0.0) {
    traceRayEXT(...);                                 // visibility toward the sampled dir
    if (payload.hitDist < 0.0) {                      // ray escaped -> it saw the env
        float w = misBalanceHeuristic(envPdf, bsdfPdfAtEnv);
        radiance += envRadiance * (diff + spec) * NdotL * w / envPdf;   // /envPdf cancels the spike
    }
}
```

**Lesson.** Variance isn't noise you denoise away — it's a sampling problem you solve at the source. A denoiser can only reconstruct a signal whose variance is already low. Every later win was the same move: cut variance *before* the denoiser, not after.

### The flicker that wasn't where it looked

Chrome on a test helmet flickered frame to frame. I built two plausible fixes — feeding DLSS the specular hit-distance guide, then swapping the glossy sampler for GGX VNDF importance sampling (Heitz 2018) — measured both with a drift-controlled A/B, and both came back *not* fixing it. The control gave it away: a diffuse wall that should read a temporal std of ~3 measured ~27. The ReSTIR-GI reservoir was still boiling, and the metal was just reflecting it — and a reflection is view-dependent, so the denoiser can't reproject the boil away the way it does on the walls.

| scene | metal flicker (temporal std) | fps (1 spp) |
|---|---|---|
| enclosed room | ~15 | ~35 |
| open HDRI scene | **0.69** | **67** |

The fix was the scene, not the sampler: an open environment reflects a stable sky instead of boiling GI, and one importance-sampled HDRI replaces four noisy area lights — ~20× less flicker and ~2× faster. Same lesson: ask *what* the pixel reflects before *how* the ray is aimed.

<p>
  <img src="docs/images/flicker_map.jpg" width="49%" alt="per-pixel temporal variance map — metal bright, diffuse walls dark">
  <img src="docs/images/indoor_before.jpg" width="49%" alt="the enclosed room the flicker came from">
</p>

*Left: a per-pixel temporal-standard-deviation map — bright = flickering. The metal glows; the diffuse walls are black, which is what pointed at the reflection. Right: the enclosed room the flicker came from.*

I also tried **ReSTIR DI** for the direct lighting and reverted it — the weight calculation introduced more noise than it removed. Measuring that honestly and backing it out was the right call; ReSTIR GI on the indirect bounce stayed, and does the real variance reduction (3.2× less boil, unbiased).

## Architecture

Two pipelines, one scene. Both consume the same bindless texture array, the same material SSBO, the same TLAS. Switch at runtime via `--denoise=` and the example app argv.

```
                         scene + materials + lights
                                    |
                  +-----------------+-----------------+
                  |                                   |
            Path Tracer (rt/)                Deferred (deferred/)
            VK_KHR_ray_tracing_pipeline      G-buffer + lighting
            NEE + MIS + env-map IS           CSM, SSAO, SSR, SSS
            Sobol + Owen scramble            TAA, bloom, ACES
            OIDN / OptiX / NRD denoise       (RT shadow + RT GI plug in here)
                  |                                   |
                  +----------------+------------------+
                                   |
                            output (PNG / swapchain)
```

### Path tracing

Lives in `ohao/render/rt/`. Vulkan KHR ray tracing pipeline. Files are split: `path_tracer.cpp` for the orchestrator, `path_tracer_descriptors.cpp` for the 30-binding descriptor layout, `path_tracer_images.cpp` for the AOV images, `path_tracer_pipeline.cpp` for the SBT, `path_tracer_render.cpp` for dispatch.

- Next-event estimation with multi-light sampling from a GPU light SSBO
- MIS with both balance heuristic and power heuristic (beta=2), see `shaders/includes/rt/mis.glsl`
- Environment map importance sampling: marginal + conditional CDF binary search in `shaders/includes/rt/env_sampling.glsl`, CDFs built CPU-side in `env_cdf.cpp`
- Sobol QMC sequence with Owen scrambling (`owen_scramble.cpp`, `sobol_generator.cpp`)
- Adaptive sampling: per-pixel variance estimated from a 3x3 neighborhood in `pt_raygen.rgen`, sample budget steered by noise level
- Cook-Torrance GGX BRDF, bindless PBR textures (diffuse, normal, rough/metal, emissive)
- Alpha transparency via any-hit shader for foliage and hair cards
- Animated geometry: `animated_rt_manager.cpp` does GPU skinning into a vertex buffer that feeds BLAS rebuilds

### Denoising (three backends, runtime switchable)

| Backend | Files | Use |
|---|---|---|
| Intel OIDN 2.x | `oidn_denoise.cpp/.hpp` | CPU post-process for offline reference |
| NVIDIA OptiX 9.1 | `optix_denoise.cpp/.hpp` | GPU denoise via CUDA interop, optional |
| NVIDIA NRD 4.17 | `nrd_denoise.cpp`, `nrd_compose.cpp`, `nrd_tonemap.cpp` | Realtime REBLUR diffuse + specular for the interactive viewer |

OptiX is optional. If the SDK isn't found, the OptiX backend compiles as a no-op stub and `--denoise=optix` falls back to OIDN at runtime.

### Deferred raster

`ohao/render/deferred/`. G-buffer with position, normal, albedo, rough/metal, emissive. Lighting pass uses the same bindless texture array as RT.

- Cascaded shadow maps (CSM) with skinned variant for animated meshes
- SSAO, SSR (screen-space reflections), SSS (subsurface scattering)
- TAA with Halton jitter, bloom, ACES tonemapping
- HDR environment reflection, Fresnel-weighted

### Hybrid composite

`render/rt/rt_shadow_technique.*` and `render/rt/rt_gi_technique.*` plug into the deferred G-buffer. RT shadows are traced from the G-buffer's reconstructed world position. RT GI is 1-bounce indirect with temporal blending (see header comment in `rt_gi_technique.hpp`). Both run on the same TLAS the path tracer uses.

### Acceleration structures

`rt_acceleration_structure.cpp` handles BLAS/TLAS lifecycle. Animated meshes go through `animated_rt_manager.cpp` which skins on the GPU then rebuilds the BLAS, so RT GI and RT shadows stay correct under animation. The path tracer currently uses static BLAS for animated meshes (known gap, called out in `CLAUDE.md`).

### Subsystem map

```
ohao/
  core/         413 LOC   logging, events, commands
  gpu/vulkan/  9,021 LOC  device, memory, descriptors, dispatch
  render/     23,780 LOC  rt/, deferred/, graph/, ibl/, particles/, picking/, async/
  scene/       7,042 LOC  actor, component, asset (gltf/obj/fbx via Assimp)
  physics/    10,989 LOC  Jolt 5.1 backend behind IPhysicsBackend plugin
  animation/     966 LOC  skeleton, clips, controller, GPU skinning
  audio/         383 LOC  miniaudio backend
shaders/      14,738 LOC  121 files, mostly rt/, core/, postprocess/, compute/
```

## Build

```bash
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build -j8
cmake --build build --target shaders   # shaders only
```

Requires CMake 3.20+, Vulkan SDK 1.3+ with RT extensions, a C++17 compiler. Tested on Linux with GCC/Clang and on Windows with MSVC.

## Run

```bash
./build/cornell_box       output.png 1024                 # 1024 spp path-traced reference
./build/cornell_box       output.png 1   deferred         # deferred + RT hybrid
./build/model_viewer      model.glb  output.png 256       # GLB in Cornell box, OIDN denoised
./build/model_viewer      model.fbx  output.png 1 deferred # FBX with skinned animation
./build/env_demo          model.glb  env.hdr output.png 256
./build/interactive       model.glb  env.hdr              # GLFW viewer, ~75 fps
./build/turntable         model.glb  mirror 256 480       # turntable video frames
./build/renderer_test                                      # smoke test
```

All examples accept `--denoise=oidn|optix|nrd|none`. The interactive viewer uses NRD's REBLUR_DIFFUSE_SPECULAR for realtime denoising.

## Dependencies

- Vulkan SDK 1.3+ with RT extensions
- GLFW 3.x (interactive viewer only)
- Intel OpenImageDenoise 2.x
- NVIDIA OptiX SDK 9.1 (optional, requires CUDA Toolkit). Set `OPTIX_ROOT` or install under `$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64/`. CMake auto-detects.
- NVIDIA NRD (RayTracingDenoiser) v4.17, fetched via FetchContent. Pure Vulkan, no CUDA. Opt out with `-DOHAO_NRD=OFF`.
- Jolt Physics 5.1.0, Assimp 5.4.3, tinygltf, stb, glm, VMA, nlohmann/json (all FetchContent)

## Project status

`CHANGELOG.md` and `devlog/` track the progression. Most recent milestone: Sub-plan 4.F (Apr 2026) shipped the NRD quality pass with env composite in tonemap, view-change bootstrap, multi-spp AOV accumulation, and Halton jitter. `--denoise=nrd` is shippable-realtime quality. The newest work (see [Real-time path tracing](#real-time-path-tracing--and-the-firefly-that-taught-me-the-most) above) adds `--denoise=dlssrr` (DLSS Ray Reconstruction), ReSTIR GI, DLSS upscaling, and an outdoor HDRI showcase scene — closing two of the gaps I'd previously called out. ReSTIR DI was tried and reverted (added more noise than it removed). The remaining path-tracer gap in `CLAUDE.md` is RT BLAS rebuild for skinned meshes.

For deeper docs see `docs/INDEX.md`, `docs/render.md`, `docs/architecture/`, and the per-bug write-ups in `docs/bugs_solved/`.

## Known build gotchas

These bit me, listed so they don't bite you:

- Linux: `#ifdef _WIN32` guards around Vulkan external memory extensions
- GCC: `-Wl,--start-group ... --end-group` for circular static lib deps
- GCC strict mode: missing `<cstring>` and `<algorithm>` includes in some upstream headers
- stbi duplicate symbols: `--allow-multiple-definition` on Linux, `/FORCE:MULTIPLE` on MSVC
- Shutdown order matters: Scene before VulkanRenderer, PathTracer before VkDevice

## License

MIT, see [LICENSE](LICENSE). Built by Frank Yin ([@Qervas](https://github.com/Qervas)).
