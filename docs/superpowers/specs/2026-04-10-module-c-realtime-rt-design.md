# Module C: Real-Time Ray Tracing

## Problem

The path tracer produces beautiful images but takes 15-30 seconds per frame (1024 spp at 1080p). Interactive applications need 60fps — that's 16ms per frame, or roughly 1-4 spp. At 1 spp, the image is pure noise. We need denoising and temporal accumulation to make 1-4 spp look clean.

## Current State

```
1024 spp → 30 seconds → clean image
   4 spp → 0.1 seconds → pure noise
   1 spp → 0.03 seconds → unusable
```

## Target

```
1-4 spp + denoiser → 16ms → clean enough for interactive use
Camera moves → temporal reprojection → no ghosting
Static camera → progressive accumulation → converges to ground truth
```

## Architecture

Two modes of operation:

### Interactive Mode (camera moving, game running)
```
Frame N:
  1. Rasterize GBuffer (position, normal, albedo, roughness, motion vectors)
  2. RT shadows (1 spp) → noisy shadow mask
  3. RT reflections (1 spp on glossy surfaces only)
  4. RT GI (1 spp diffuse indirect)
  5. Temporal accumulation (reproject from frame N-1, blend)
  6. Spatial denoiser (edge-aware blur)
  7. Composite: direct light + denoised RT shadows/reflections/GI
  8. Post-processing (bloom, tonemap)
```

### Offline Mode (static camera, quality render)
```
Same as current: accumulate N spp, tonemap, done.
Already works — just needs a mode switch.
```

## Build Order

### Task 1: Low-spp path tracer + progressive mode switch
- Add render mode: `Interactive` vs `Offline`
- Interactive: 1-4 spp per frame, output noisy image
- Offline: accumulate spp over time (current behavior)
- Measure: frames per second at 1 spp
- **Test**: interactive mode renders at 30+ fps (noisy but fast)

### Task 2: Temporal accumulation with reprojection
- Store previous frame's color buffer
- Compute motion vectors (current vs previous frame MVP)
- Reproject previous frame to current camera
- Blend: `color = lerp(reprojected, current, 0.1)` (90% history, 10% new)
- Disocclusion detection: reject history where motion vectors are invalid
- **Test**: static camera converges to clean image over time, moving camera stays responsive

### Task 3: Spatial denoiser (A-Trous wavelet)
- Edge-aware wavelet filter using GBuffer normals + depth
- 5 iterations at increasing kernel size (à trous)
- Preserves edges (normal/depth discontinuities) while blurring noise
- Applied after temporal accumulation
- Implemented as compute shader passes
- **Test**: 4 spp + temporal + spatial looks nearly as clean as 256 spp raw

### Task 4: Hybrid RT passes (optional, future)
- RT shadows: 1 spp shadow ray per light → denoised shadow mask
- RT reflections: 1 spp on glossy pixels only (roughness < 0.3)
- RT ambient occlusion: 1 spp short-range rays
- Each pass denoised separately, composited with rasterized GBuffer
- This is the CoD/Cyberpunk architecture

## Key Insight

Tasks 1-3 upgrade the existing path tracer to interactive quality. Task 4 is a fundamentally different architecture (hybrid). We should do 1-3 first — they make the current path tracer usable at interactive rates without rewriting everything.

## Files Modified

| File | Changes |
|------|---------|
| `ohao/render/rt/path_tracer.hpp/cpp` | Interactive mode, spp control, history buffer |
| `shaders/rt/pt_raygen.rgen` | Temporal reprojection, motion vectors |
| `shaders/compute/denoise_atrous.comp` | A-Trous wavelet denoiser (new) |
| `ohao/render/deferred/denoiser_pass.hpp/cpp` | Denoiser compute pass (new) |
| `ohao/gpu/vulkan/offscreen_renderer.cpp` | Mode switch, frame timing |
| `examples/interactive.cpp` | Interactive viewer with camera controls (new) |

## Performance Budget (target)

| Pass | Budget |
|------|--------|
| GBuffer rasterize | 1ms |
| Path trace 1 spp | 3ms |
| Temporal accumulation | 0.5ms |
| Spatial denoise (5 iterations) | 2ms |
| Post-processing | 1ms |
| **Total** | **~8ms (120fps)** |
