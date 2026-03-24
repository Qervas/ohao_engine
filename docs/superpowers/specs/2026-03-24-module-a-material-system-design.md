# Module A: PBR Material System for Path Tracer

## Problem

The path tracer only samples diffuse textures. Roughness is hardcoded to 0.75, metallic is always 0, normal maps are ignored, emissive maps are unused. Every model looks like matte plastic.

## Design

### Texture Arrays (4 separate sampler2DArray bindings)

| Binding | Map Type | Format | Channel Layout |
|---------|----------|--------|----------------|
| 11 | Diffuse | R8G8B8A8_SRGB | RGB=color, A=opacity |
| 12 | Normal | R8G8B8A8_UNORM | RGB=tangent-space normal |
| 13 | Roughness+Metallic | R8G8_UNORM | R=roughness, G=metallic |
| 14 | Emissive | R8G8B8A8_SRGB | RGB=emissive color |

All arrays are 1024x1024 per layer (matching existing diffuse array). Each material indexes independently into each array (-1 = no texture for that slot).

### Material Buffer (expanded)

Current: 1 vec4 per material `(baseColor.rgb, texLayer)`
New: 2 vec4s per material:

```
vec4[matID * 2 + 0] = (baseColor.r, baseColor.g, baseColor.b, diffuseTexLayer)
vec4[matID * 2 + 1] = (roughness, metallic, normalTexLayer, emissiveTexLayer)
```

Texture layer index: >= 0 means sample from array, < 0 means use scalar value.

### Normal Mapping via TBN Matrix

Requires per-vertex tangent vectors. Two sources:
1. **GLTF tangent attribute** — extract if present (vec4, w=handedness)
2. **Computed from UV gradients** — generate if tangents missing, using triangle edge/UV deltas

In closest-hit shader:
```
T = normalize(worldMatrix * tangent.xyz)
B = cross(N, T) * tangent.w  // handedness
N_perturbed = normalize(TBN * (normalMapSample * 2.0 - 1.0))
```

### Payload Changes

Current payload.attenuation is vec3 with packed roughness in .x.
New encoding — use all 3 channels:

```
payload.attenuation.x = roughness (0-1)
payload.attenuation.y = metallic (0-1)
payload.attenuation.z = emissive luminance (0 = not emissive)
```

Add `payload.hitEmissive` (vec3) for emissive color if needed, or pack into existing `payload.color`.

### GLTF Loader Extraction

From `pbrMetallicRoughness`:
- `baseColorTexture` → diffuse array (existing)
- `metallicRoughnessTexture` → unpack G=roughness, B=metallic into R8G8 texture
- `metallicFactor`, `roughnessFactor` → scalar fallbacks

From material:
- `normalTexture` → normal array
- `emissiveTexture` → emissive array
- `emissiveFactor` → scalar fallback

From `KHR_materials_pbrSpecularGlossiness` (CC3 models):
- `diffuseTexture` → diffuse array (existing)
- `glossinessFactor` → convert to roughness = 1 - glossiness

### OBJ Loader

Auto-discover from textures/ folder:
- `{name}_diff_*` → diffuse (existing)
- `{name}_norm_*` → normal
- `{name}_spec_*` → roughness/metallic (specular → rough approximation)

### Tangent Generation (fallback)

For models without tangent attributes:
```cpp
for each triangle (v0, v1, v2):
    edge1 = v1.pos - v0.pos
    edge2 = v2.pos - v0.pos
    dUV1 = v1.uv - v0.uv
    dUV2 = v2.uv - v0.uv
    tangent = normalize((edge1 * dUV2.y - edge2 * dUV1.y) / det)
    // accumulate per-vertex, normalize at end
```

Store in new tangent buffer (binding 15, vec4[], w=handedness).

## Build Order

Each step is independently testable with a render.

### Step 1: Per-material roughness/metallic scalars
- Fix hardcoded 0.75 in closest-hit shader
- Pass roughness from matColors[].w through payload
- Extract metallicFactor from GLTF, encode in material buffer
- **Test**: render with varying roughness — glossy vs matte surfaces visible

### Step 2: Normal mapping
- Extract normal textures from GLTF (normalTexture)
- Extract tangent vectors from GLTF (or generate from UVs)
- Add tangent buffer (binding 15)
- Add normal texture array (binding 12)
- Build TBN in closest-hit, perturb normal
- **Test**: render model with/without normal map — surface detail visible

### Step 3: Roughness/metallic texture maps
- Extract metallicRoughnessTexture from GLTF
- Unpack G=roughness, B=metallic into R8G8 texture
- Add roughness+metallic array (binding 13)
- Sample per-pixel in closest-hit
- **Test**: render metal vs plastic on same model

### Step 4: Emissive maps
- Extract emissiveTexture from GLTF
- Add emissive array (binding 14)
- In raygen, add emissive contribution when payload signals it
- **Test**: render model with glowing elements (e.g., sci-fi suit lights)

## Files Modified

| File | Changes |
|------|---------|
| `shaders/rt/pt_closesthit.rchit` | Add bindings 12-15, TBN normal mapping, PBR texture sampling |
| `shaders/rt/pt_raygen.rgen` | Decode per-pixel roughness/metallic, emissive handling |
| `src/engine/asset/model_gltf.cpp` | Extract normal/roughness/metallic/emissive textures + tangents |
| `src/engine/asset/model.hpp` | Add normalTextures, roughMetalTextures, emissiveTextures, tangent data |
| `src/engine/asset/model.cpp` | OBJ normal/spec texture auto-discovery |
| `src/renderer/rt/path_tracer.hpp` | New buffer members for 3 extra texture arrays + tangent buffer |
| `src/renderer/rt/path_tracer.cpp` | New descriptor bindings 12-15, layout updates |
| `src/renderer/offscreen/offscreen_scene_render.cpp` | Build 3 new texture arrays, tangent buffer, expanded material buffer |

## Verification

After each step, render the realistic_female.glb and scifi-girl OBJ in the Cornell box:
- Step 1: Visible roughness difference between skin/cloth/metal
- Step 2: Skin pores, fabric weave, surface detail from normal maps
- Step 3: Per-pixel material variation (worn edges, scratches)
- Step 4: Glowing elements on sci-fi models
