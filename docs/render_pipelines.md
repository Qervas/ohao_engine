# Render pipelines — one Vulkan host, four modes

## North star

One Vulkan foundation — device, upload, bindless textures, offscreen targets,
seed — with several image-formation paths on top of it sharing scene,
materials and acceleration structures. Switching mode does not rebuild the
scene.

```
Scene / cameras / lights
        │
 Vulkan foundation (device, upload, bindless, offscreen, seed)
        │
 ┌──────┼────────────┬──────────────┐
 ▼      ▼            ▼              ▼
Forward Deferred   RTRealtime    RTOffline
        (raster)   (path trace)  (path trace)
```

`RenderMode` (`ohao/gpu/vulkan/renderer.hpp`):

| Mode | Role |
|------|------|
| **Forward** | Legacy forward raster, 8-light limit. Kept for comparison |
| **Deferred** | G-buffer raster: CSM, SSAO, post-processing. The interactive default |
| **RTRealtime** | KHR path tracing tuned for interactive use — 1 spp, DLSS-RR / NRD denoised, ReSTIR GI |
| **RTOffline** | KHR path tracing tuned for reference stills — high spp, OIDN |

`isRTRenderMode` and `isRasterRenderMode` are the predicates to branch on
rather than comparing enumerators by hand.

## Hybrid

The hybrid path is not a fifth mode: it is **Deferred** with RT shadows and
one-bounce RT GI composited on top of the G-buffer. That is why the two
pipelines must agree about materials and the TLAS — they are looking at the
same surfaces in the same frame.

## Denoising

Runtime-switchable through `--denoise=` on every example:

| Value | Backend |
|-------|---------|
| `none` | raw path-traced output |
| `oidn` | Intel OpenImageDenoise 2.x — the offline default |
| `nrd` | NVIDIA NRD REBLUR_DIFFUSE_SPECULAR + cinematic post — the interactive one |
| `atrous` | built-in à-trous / SVGF, no external dependency |
| `dlss` | NVIDIA DLSS Ray Reconstruction (`dlssrr`, `dlssd` also accepted) |

`optix` is still parsed and falls back to OIDN with a warning; the OptiX
backend was removed in `0873766`.

## Art of the code (LOC law)

| Rule | Limit |
|------|-------|
| Soft target | ≤ **350** lines per `.hpp` / `.cpp` |
| Hard cap | **500** lines per TU |
| Ban | No new **1k+** files |
| Host | `renderer.cpp` — **wiring only** for new pipelines |

## Related

- `README.md` — build, run, and the path-tracing write-up
- `docs/bugs_solved/` — the failures worth keeping
- `site/` — the monograph, unit by unit
