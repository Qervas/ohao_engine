# Module C Task 4: Hybrid RT Pipeline

## Goal

Merge the deferred rasterization pipeline with RT to get the best of both:
rasterization for primary visibility (fast), RT for shadows/reflections/GI (correct).

## Build Order

### Task 4a: RT Shadows
- Rasterize GBuffer (existing)
- For each pixel, read world position + normal from GBuffer
- Trace 1 shadow ray per light toward the light source
- Output: shadow mask texture (0 = shadowed, 1 = lit)
- Deferred lighting reads shadow mask instead of CSM shadow map
- **Test**: compare CSM vs RT shadows — RT has correct soft shadows, no shadow map artifacts

### Task 4b: RT Reflections
- For glossy pixels (roughness < 0.3), trace 1 reflection ray
- Direction = reflect(view, normal) + roughness jitter
- Output: reflection color texture
- Composite into deferred lighting output
- Skip matte pixels entirely (save rays)
- **Test**: metallic surfaces show correct environment reflections

### Task 4c: RT Ambient Occlusion / GI
- For each pixel, trace 1 short-range ray in cosine-weighted hemisphere
- If hits nearby geometry → occluded (AO) + sample that surface's color (GI)
- Output: AO factor + indirect color
- Denoise and blend into final image
- **Test**: corners get darker (AO), colored walls bleed onto nearby surfaces (GI)

### Task 4d: Denoise + Composite
- Apply bilateral/A-Trous denoiser to each RT buffer separately
- Shadow mask: simple blur (binary signal)
- Reflections: edge-aware blur guided by roughness
- GI: aggressive blur (low frequency signal)
- Composite: final = direct_light * shadow + reflections + GI + emissive
