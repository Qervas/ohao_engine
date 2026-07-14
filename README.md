# OHAO Engine

A solo Vulkan 1.3 renderer with a hybrid pipeline: KHR ray tracing for path-traced reference and indirect lighting, deferred raster for real-time. Pure C++, no editor framework.

![OHAO Engine hero](assets/hero.png)

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
| Denoise backends | 3 (Intel OIDN, NVIDIA OptiX, NVIDIA NRD) |

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

Requires CMake 3.20+, Vulkan SDK 1.3+ with RT extensions, a **C++20** compiler. Tested on Linux with GCC/Clang and on Windows with MSVC.

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

`CHANGELOG.md` and `devlog/` track the progression. Most recent milestone: Sub-plan 4.F (Apr 2026) shipped the NRD quality pass with env composite in tonemap, view-change bootstrap, multi-spp AOV accumulation, and Halton jitter. `--denoise=nrd` is shippable-realtime quality. Open gaps that I've called out honestly in `CLAUDE.md`: ReSTIR DI, DLSS Ray Reconstruction, and RT BLAS rebuild for skinned meshes in the path tracer.

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
