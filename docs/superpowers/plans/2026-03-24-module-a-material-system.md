# Module A: PBR Material System — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full PBR material pipeline — per-pixel normal mapping, roughness, metallic, emissive — in the path tracer.

**Architecture:** Expand material buffer to 2 vec4s per material (scalars + texture layer indices). Add 3 new texture arrays (normal, roughness+metallic, emissive) alongside existing diffuse array. Add tangent buffer for TBN normal mapping. Extract all PBR maps from GLTF loader.

**Tech Stack:** GLSL 460 + GL_EXT_ray_tracing, Vulkan 1.3, tinygltf, stb_image

**Spec:** `docs/superpowers/specs/2026-03-24-module-a-material-system-design.md`

**Test method:** Render 1080p/512spp previews after each task. Compare before/after visually.

---

## Task 1: Expand Material Buffer to 2 vec4s + Per-Material Roughness/Metallic

Currently roughness is hardcoded to 0.75 in the closest-hit shader. This task passes real per-material roughness and metallic values through the pipeline.

**Files:**
- Modify: `src/engine/asset/model_gltf.cpp` — extract metallicFactor per material
- Modify: `src/engine/asset/model.hpp` — add metallicPerMaterial vector or encode in materialColors
- Modify: `src/renderer/offscreen/offscreen_scene_render.cpp` — upload 2 vec4s per material
- Modify: `shaders/rt/pt_closesthit.rchit` — read roughness+metallic from material buffer
- Modify: `src/renderer/rt/path_tracer.cpp` — update descriptor for larger material buffer

- [ ] **Step 1: GLTF loader — extract metallicFactor**

In `model_gltf.cpp`, the material loop already extracts `roughnessFactor` into `materialColors[].w`. Add metallic extraction. Store as a parallel vector `materialMetallic` in model.hpp, or encode into materialColors by switching to 2-vec4 layout.

In `model.hpp`, add:
```cpp
std::vector<float> materialMetallic;  // per-material metallic factor
```

In `model_gltf.cpp` material loop, after pushing to materialColors:
```cpp
materialMetallic.push_back(static_cast<float>(pbr.metallicFactor));
```

Also handle specular-glossiness extension: metallic = 0 for spec-gloss materials.
Also handle OBJ loader: push default metallic (0.0) per material.

- [ ] **Step 2: Upload 2 vec4s per material in scene buffer**

In `offscreen_scene_render.cpp`, where `allMatColors` is built, change from 1 vec4 to 2 vec4s per material:

```cpp
// vec4[0] = (r, g, b, diffuseTexLayer)  — same as before
// vec4[1] = (roughness, metallic, -1.0, -1.0)  — new PBR params
allMatColors.push_back(glm::vec4(color.r, color.g, color.b, texLayer));
allMatColors.push_back(glm::vec4(roughness, metallic, -1.0f, -1.0f));
```

Update buffer size calculation accordingly.

- [ ] **Step 3: Update closest-hit shader to read real roughness/metallic**

In `pt_closesthit.rchit`, replace the hardcoded roughness:

```glsl
// Old:
payload.attenuation = vec3(0.75, 0.0, 0.0);

// New:
vec4 matParams = matColorBuf.matColors[matID * 2u + 1u];
float roughness = matParams.x;
float metallic = matParams.y;
// Pack: negative roughness = metallic
payload.attenuation = vec3(metallic > 0.5 ? -(roughness + 0.001) : roughness, 0.0, 0.0);
```

Also update matColor read to use `matID * 2u + 0u`:
```glsl
vec4 matColor = matColorBuf.matColors[matID * 2u + 0u];
```

- [ ] **Step 4: Update path_tracer.cpp descriptor range**

The material color buffer descriptor range needs to account for 2x the data:
```cpp
materialColorInfo.range = materialColorCount * 2 * sizeof(glm::vec4);
```

- [ ] **Step 5: Build + render preview**

```bash
cmake --build build --target shaders && cmake --build build --config Release -j8
./build/Release/renderer_test.exe preview.png
```

Verify: different roughness values visible on different materials (glossy vs matte).

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(renderer): Per-material roughness/metallic in path tracer"
```

---

## Task 2: Normal Mapping — Tangent Extraction + TBN + Normal Map Sampling

The highest-impact visual upgrade. Adds surface detail (skin pores, fabric weave, metal scratches) without extra geometry.

**Files:**
- Modify: `src/engine/asset/model_gltf.cpp` — extract tangent attribute + normal textures
- Modify: `src/engine/asset/model.hpp` — add tangent data, normalTextures vector
- Modify: `src/engine/asset/model.cpp` — OBJ tangent generation from UV gradients
- Modify: `src/renderer/offscreen/offscreen_scene_render.cpp` — upload tangent buffer + normal texture array
- Modify: `src/renderer/rt/path_tracer.hpp` — new buffer/image members
- Modify: `src/renderer/rt/path_tracer.cpp` — new descriptors (binding 12 normal array, binding 15 tangent buffer)
- Modify: `shaders/rt/pt_closesthit.rchit` — TBN matrix, normal map sampling

- [ ] **Step 1: Extract tangent vectors from GLTF**

In `model_gltf.cpp`, in the vertex attribute loop where positions/normals/UVs are extracted, also check for `TANGENT` attribute:

```cpp
if (gltfModel.accessors[primitive.attributes.at("TANGENT")]) {
    // Extract vec4 tangent (xyz=tangent, w=handedness)
}
```

Store tangent as `glm::vec4` in each Vertex (add to Vertex struct if not present), or in a parallel `std::vector<glm::vec4> tangents` on Model.

- [ ] **Step 2: Generate tangents for models without them**

In `model.cpp` after OBJ load, and in `model_gltf.cpp` after GLTF load if tangents are missing:

```cpp
void Model::generateTangents() {
    tangents.resize(vertices.size(), glm::vec4(1,0,0,1));
    // For each triangle: compute tangent from edge/UV deltas
    // Accumulate per-vertex, normalize at end
}
```

- [ ] **Step 3: Extract normal textures from GLTF**

In `model_gltf.cpp` material loop, after extracting diffuse texture:

```cpp
if (gltfMat.normalTexture.index >= 0) {
    // Load image pixels same as diffuse
    // Store in model.normalTextures[]
    // Set materialNormalTexIndex[matIdx] = layer index
}
```

Add to `model.hpp`:
```cpp
std::vector<TextureData> normalTextures;
std::vector<int> materialNormalTexIndex;
```

- [ ] **Step 4: Upload tangent buffer (binding 15)**

In `offscreen_scene_render.cpp`, create tangent buffer similar to normal buffer:

```cpp
// Collect all tangents into contiguous buffer
std::vector<glm::vec4> allTangents;
for each actor's model:
    for each vertex: allTangents.push_back(tangent);
// Create VkBuffer, upload
```

Add `m_rtTangentBuffer` to offscreen_renderer.hpp.

- [ ] **Step 5: Build normal texture array (binding 12)**

In `offscreen_scene_render.cpp`, create a second texture array for normals:

```cpp
// Same process as diffuse array but:
// - Format: VK_FORMAT_R8G8B8A8_UNORM (not SRGB — normals are linear)
// - Source: model.normalTextures
```

Add `m_rtNormalTextureArray`, `m_rtNormalTextureArrayView` to offscreen_renderer.hpp.

- [ ] **Step 6: Add descriptors for binding 12 + 15 in path_tracer.cpp**

Update descriptor set layout to include:
- Binding 12: COMBINED_IMAGE_SAMPLER (normal texture array)
- Binding 15: STORAGE_BUFFER (tangent buffer)

Update descriptor pool sizes. Update descriptor writes in render().

- [ ] **Step 7: Update material buffer with normal texture layer index**

In the 2-vec4 material encoding from Task 1:
```
vec4[matID * 2 + 1] = (roughness, metallic, normalTexLayer, emissiveTexLayer)
```

Set `normalTexLayer` from `materialNormalTexIndex`.

- [ ] **Step 8: Closest-hit shader — TBN normal mapping**

Add to `pt_closesthit.rchit`:

```glsl
layout(set = 0, binding = 12) uniform sampler2DArray normalArray;
layout(set = 0, binding = 15) readonly buffer TangentBuffer { vec4 tangents[]; } tangentBuf;

// In main():
// Interpolate tangent
vec4 t0 = tangentBuf.tangents[i0];
vec4 t1 = tangentBuf.tangents[i1];
vec4 t2 = tangentBuf.tangents[i2];
vec4 interpTangent = w * t0 + u * t1 + v * t2;

vec3 T = normalize(mat3(gl_ObjectToWorldEXT) * interpTangent.xyz);
vec3 B = cross(worldNormal, T) * interpTangent.w;
mat3 TBN = mat3(T, B, worldNormal);

// Sample normal map
float normalTexLayer = matParams.z;  // from vec4[matID*2+1]
if (normalTexLayer >= 0.0) {
    vec3 mapNormal = texture(normalArray, vec3(texUV, normalTexLayer)).rgb;
    mapNormal = mapNormal * 2.0 - 1.0;  // [0,1] → [-1,1]
    worldNormal = normalize(TBN * mapNormal);
}
```

- [ ] **Step 9: Build + render preview**

Compare with/without normal maps. Skin detail, fabric weave should be visible.

- [ ] **Step 10: Commit**

```bash
git add -A && git commit -m "feat(renderer): Normal mapping with TBN in path tracer"
```

---

## Task 3: Roughness/Metallic Texture Maps

Per-pixel roughness and metallic from GLTF's metallicRoughnessTexture.

**Files:**
- Modify: `src/engine/asset/model_gltf.cpp` — extract metallicRoughnessTexture, unpack G/B channels
- Modify: `src/engine/asset/model.hpp` — add roughMetalTextures vector
- Modify: `src/renderer/offscreen/offscreen_scene_render.cpp` — build roughness+metallic texture array
- Modify: `src/renderer/rt/path_tracer.cpp` — binding 13 descriptor
- Modify: `shaders/rt/pt_closesthit.rchit` — sample per-pixel roughness/metallic

- [ ] **Step 1: Extract metallicRoughnessTexture from GLTF**

GLTF convention: `metallicRoughnessTexture` is a single image where G=roughness, B=metallic.

In `model_gltf.cpp`:
```cpp
if (pbr.metallicRoughnessTexture.index >= 0) {
    // Load full RGBA image
    // Create R8G8 texture: R=image.G (roughness), G=image.B (metallic)
    // Store in model.roughMetalTextures[]
}
```

Add to `model.hpp`:
```cpp
std::vector<TextureData> roughMetalTextures;
std::vector<int> materialRoughMetalTexIndex;
```

- [ ] **Step 2: Build roughness+metallic texture array (binding 13)**

Format: `VK_FORMAT_R8G8_UNORM` (2 channels: R=roughness, G=metallic).
Same process as normal array.

- [ ] **Step 3: Add binding 13 descriptor in path_tracer.cpp**

- [ ] **Step 4: Closest-hit shader — sample per-pixel roughness/metallic**

```glsl
layout(set = 0, binding = 13) uniform sampler2DArray roughMetalArray;

// Check if this material has a roughness/metallic texture
// (encode layer index in material buffer, e.g. a spare channel)
vec2 rm = texture(roughMetalArray, vec3(texUV, rmTexLayer)).rg;
float roughness = rm.r;
float metallic = rm.g;
```

Fall back to scalar values from material buffer if no texture.

- [ ] **Step 5: Build + render + commit**

---

## Task 4: Emissive Maps

Self-glowing surfaces from emissive texture maps.

**Files:**
- Modify: `src/engine/asset/model_gltf.cpp` — extract emissiveTexture + emissiveFactor
- Modify: `src/engine/asset/model.hpp` — add emissiveTextures vector
- Modify: `src/renderer/offscreen/offscreen_scene_render.cpp` — build emissive texture array
- Modify: `src/renderer/rt/path_tracer.cpp` — binding 14 descriptor
- Modify: `shaders/rt/pt_closesthit.rchit` — sample emissive, pass to raygen
- Modify: `shaders/rt/pt_raygen.rgen` — add emissive contribution at hit points

- [ ] **Step 1: Extract emissive data from GLTF**

```cpp
if (gltfMat.emissiveTexture.index >= 0) {
    // Load emissive texture → model.emissiveTextures[]
}
// Also extract emissiveFactor (vec3) as scalar fallback
```

- [ ] **Step 2: Build emissive texture array (binding 14) + descriptor**

- [ ] **Step 3: Closest-hit shader — sample emissive**

```glsl
vec3 emissive = vec3(0.0);
if (emissiveTexLayer >= 0.0) {
    emissive = texture(emissiveArray, vec3(texUV, emissiveTexLayer)).rgb;
}
emissive *= emissiveFactor;  // scale by material factor
payload.color = emissive;  // signal emissive to raygen
```

- [ ] **Step 4: Raygen shader — handle emissive surfaces**

```glsl
if (length(payload.color) > 0.0) {
    // This surface emits light
    radiance += throughput * payload.color;
    // Don't break — continue bouncing for indirect
}
```

- [ ] **Step 5: Build + render + commit**

Verify: glowing elements on sci-fi models (e.g., suit lights, visor).
