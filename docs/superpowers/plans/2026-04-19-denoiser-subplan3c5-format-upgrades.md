# Denoiser Sub-plan 3.C.5: Format Upgrades Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote 3.B roughness AOV from R8 UNORM → R16F and 3.C diffuse+specular radiance AOVs from RGBA16F → RGBA32F; remove the now-unnecessary `min(..., 60000.0)` clamp at raygen exit; upgrade the 3 affected readback helper signatures from raw byte / raw half-float vectors to native `std::vector<float>` for cleaner debug consumers.

**Architecture:** Pure format + signature change. Zero math changes. Two sub-commits: (T1) GPU-side format promotion — descriptor image formats + shader binding decl qualifiers + clamp removal; (T2) CPU-side readback signature upgrade — helper return types + byte counts + env_demo dump decoders. T1 leaves `--dump-*` helpers returning garbage until T2 patches them; Cornell smoke still passes between commits because beauty doesn't depend on AOV readback.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · `R16_SFLOAT` + `R32G32B32A32_SFLOAT` storage images · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-19-denoiser-subplan3c5-format-upgrades-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.cpp` | 3 format field changes in `createImages()` (roughness + diffuse + specular blocks) — image + view formats. |
| `shaders/rt/pt_raygen.rgen` | Binding 21 decl `r8` → `r16f`. Bindings 22+23 decls `rgba16f` → `rgba32f`. Remove 2 `min(..., 60000.0)` lines at exit. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Same format / clamp edits. |
| `ohao/gpu/vulkan/renderer.hpp` | 3 readback signatures: `std::vector<uint8_t>&` / `std::vector<uint16_t>&` → `std::vector<float>&`. |
| `ohao/gpu/vulkan/renderer.cpp` | 3 readback impls: staging byte counts, output vector sizing, R16F→float decode for roughness. |
| `examples/env_demo.cpp` | Roughness dump decoder: uint8 direct → float × 255 clamp. Diffuse/specular dump decoder: half2float → direct float read. `half2float` still used by MV dump (stays). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C.5 entry. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-3c5-formats -b denoiser-3c5-format-upgrades HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-3c5-formats`.

If the fresh worktree's `build/` is empty, configure + bootstrap deps from master:
```bash
cd /home/frankyin/Desktop/Github/ohao-3c5-formats
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -10
# If build/_deps/glm-src/ is empty:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

For OptiX support (optional):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: GPU-side format promotion (images + shaders + clamp removal)

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp`
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen`
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 1.1: Change roughness AOV format in `path_tracer.cpp`

Find the 3.B roughness block in `PathTracer::createImages()` (search `m_roughnessAOVImage` near the `// ---- Feature 3.B: Roughness AOV` comment). The block creates image + view with `VK_FORMAT_R8_UNORM`.

Change BOTH occurrences (image create + view create):

```cpp
imageInfo.format = VK_FORMAT_R8_UNORM;
```

to:

```cpp
imageInfo.format = VK_FORMAT_R16_SFLOAT;
```

and:

```cpp
viewInfo.format = VK_FORMAT_R8_UNORM;
```

to:

```cpp
viewInfo.format = VK_FORMAT_R16_SFLOAT;
```

Update the block's header comment from `// ---- Feature 3.B: Roughness AOV (R8 UNORM) ----` to `// ---- Feature 3.B: Roughness AOV (R16F, promoted 3.C.5) ----`.

### Step 1.2: Change diffuse + specular radiance formats in `path_tracer.cpp`

Find the 3.C diffuse block (`// ---- Feature 3.C: Diffuse radiance (RGBA16F) ----`) and the 3.C specular block below it.

In BOTH blocks, change both `imageInfo.format` and `viewInfo.format` from:

```cpp
VK_FORMAT_R16G16B16A16_SFLOAT
```

to:

```cpp
VK_FORMAT_R32G32B32A32_SFLOAT
```

Update the block header comments from `(RGBA16F)` to `(RGBA32F, promoted 3.C.5)`.

### Step 1.3: Update shader binding decls + remove clamps in `pt_raygen.rgen`

Find the binding decls block (around line 31–33, after the binding 20 depth decl).

Change:
```glsl
layout(set = 0, binding = 21, r8)   uniform image2D roughnessAOV;
layout(set = 0, binding = 22, rgba16f) uniform image2D diffuseRadiance;
layout(set = 0, binding = 23, rgba16f) uniform image2D specularRadiance;
```

to:
```glsl
layout(set = 0, binding = 21, r16f)  uniform image2D roughnessAOV;
layout(set = 0, binding = 22, rgba32f) uniform image2D diffuseRadiance;
layout(set = 0, binding = 23, rgba32f) uniform image2D specularRadiance;
```

Find the raygen exit write block (the `// Sub-plan 3.C: demodulate + write` block just before the final `imageStore(outputImage, ...)`). The block currently reads:

```glsl
    // Sub-plan 3.C: demodulate + write diffuse/specular radiance AOVs
    vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
    vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
    // Clamp to RGBA16F safe range (max 65504; leave headroom for NRD internal ops)
    diffDemod = min(diffDemod, vec3(60000.0));
    specDemod = min(specDemod, vec3(60000.0));
    imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
    imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

DELETE the three lines (comment + 2 `min()` calls):

```glsl
    // Clamp to RGBA16F safe range (max 65504; leave headroom for NRD internal ops)
    diffDemod = min(diffDemod, vec3(60000.0));
    specDemod = min(specDemod, vec3(60000.0));
```

Result:

```glsl
    // Sub-plan 3.C: demodulate + write diffuse/specular radiance AOVs
    vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
    vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
    imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
    imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

### Step 1.4: Mirror 1.3 edits to `pt_raygen_offline.rgen`

`pt_raygen_offline.rgen` is a verbatim copy of pt_raygen.rgen except for the top 3–4 line comment. Apply 1.3 identically.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only the top comment block differs.

### Step 1.5: Mirror 1.3 edits to `pt_raygen_realtime.rgen`

Apply the binding decl format changes (r8→r16f, rgba16f→rgba32f) and the clamp removal at the analogous sites. Realtime's exit-write block has the same structure.

### Step 1.6: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-3c5-formats
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile (`r16f` + `rgba32f` both valid storage-image qualifiers on Vulkan 1.3). Clean full-app link.

### Step 1.7: Cornell smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t1_3c5_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: `Saved: /tmp/t1_3c5_cornell.png`. No Vulkan validation errors.

**NOTE:** At this point, `--dump-roughness`, `--dump-diffuse`, `--dump-specular` will produce garbled output (byte-count mismatch between staging buffer and actual image format). That's expected — Task 2 fixes it. Do NOT run env_demo with those flags yet.

Beauty output should be bit-identical to pre-3.C.5 (format change has no math impact on the `radiance` value).

### Step 1.8: Commit

```bash
git add ohao/render/rt/path_tracer.cpp \
        shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): promote roughness→R16F and radiance→RGBA32F (Sub-plan 3.C.5)

Binding 21 roughness AOV: VK_FORMAT_R8_UNORM → VK_FORMAT_R16_SFLOAT
(GLSL r8 → r16f). Bindings 22+23 diffuse/specular radiance:
VK_FORMAT_R16G16B16A16_SFLOAT → VK_FORMAT_R32G32B32A32_SFLOAT
(GLSL rgba16f → rgba32f).

Clamp removal: min(..., vec3(60000.0)) at raygen exit is no longer
needed — 32F max is 3.4e38. Deleted the clamp lines in all three
raygen shaders.

Memory: +35 MB at 1080p (trivial).

Readback helpers (CPU side) not yet updated — --dump-* debug flags
produce garbled output until the next commit. Beauty path unaffected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B` — verify `Claude Opus 4.7 (1M context)` matches the recent 3.C commits.

---

## Task 2: CPU-side readback signature upgrade + env_demo dumps + verification

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 2.1: Update readback declarations in `renderer.hpp`

Find the 3 readback decls (search `readbackRoughnessAOV`, `readbackDiffuseRadiance`, `readbackSpecularRadiance`). Currently:

```cpp
    bool readbackRoughnessAOV(std::vector<uint8_t>& roughData, uint32_t& width, uint32_t& height);
    bool readbackDiffuseRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height);
    bool readbackSpecularRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height);
```

Change the three signatures to native float:

```cpp
    // Debug: readback R16F roughness AOV as native float (decoded from half in-helper).
    bool readbackRoughnessAOV(std::vector<float>& roughData, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F diffuse radiance AOV (4 floats per pixel, native).
    bool readbackDiffuseRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F specular radiance AOV (4 floats per pixel, native).
    bool readbackSpecularRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);
```

### Step 2.2: Add a static half→float decoder helper near the top of `renderer.cpp`

Near other utility helpers (or in an anonymous namespace at top of the file if the pattern already exists — search `namespace {` or `static inline float`), add:

```cpp
namespace {
// IEEE 754 binary16 → binary32 (for R16F AOV readback).
inline float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            exp = 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 0x1f) {
        f = (sign << 31) | (0xff << 23) | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}
}  // namespace
```

If an anonymous namespace already exists, add the helper there. If not, wrap as shown.

### Step 2.3: Rewrite `readbackRoughnessAOV` in `renderer.cpp`

Find the existing `readbackRoughnessAOV` impl. The body uses staging size `w * h * 1` (R8) and writes directly into `std::vector<uint8_t>`. Change:

1. Byte count:
   ```cpp
   const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height; // R8 = 1 byte
   ```
   to:
   ```cpp
   const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 2; // R16F = 2 bytes
   ```

2. Output vector resize — currently:
   ```cpp
   roughData.resize(static_cast<size_t>(width) * height);
   ```
   unchanged (1 float per pixel).

3. The memcpy+decode step. The current code does:
   ```cpp
   std::memcpy(roughData.data(), mapped, byteCount);
   ```
   which writes R8 bytes directly. Now we have R16F halfs. Replace with a decode loop:
   ```cpp
   // R16F → float decode
   const uint16_t* halfs = reinterpret_cast<const uint16_t*>(mapped);
   for (uint32_t i = 0; i < width * height; i++) {
       roughData[i] = half_to_float(halfs[i]);
   }
   ```

### Step 2.4: Rewrite `readbackDiffuseRadiance` + `readbackSpecularRadiance` in `renderer.cpp`

Find both impls. Change each of:

1. Byte count — currently:
   ```cpp
   const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 8; // RGBA16F = 8 bytes/pixel
   ```
   to:
   ```cpp
   const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes/pixel
   ```

2. Output vector resize — currently `halfData.resize(width * height * 4)` (4 halfs). Keep the same `* 4` — now it's 4 floats per pixel:
   ```cpp
   data.resize(static_cast<size_t>(width) * height * 4);
   ```
   (parameter name also changed from `halfData` to `data` per the header).

3. The memcpy — native float copy, no decode needed:
   ```cpp
   std::memcpy(data.data(), mapped, byteCount);
   ```
   (already a direct memcpy — just change the variable name in both impls to match the new parameter name `data`).

Verify that both impls compile: `data.data()` returns `float*`, byteCount matches the RGBA32F layout, readback is semantically correct.

### Step 2.5: Update `env_demo.cpp` roughness dump block

Find the `--dump-roughness` block (search `dumpRoughnessPath`). Currently:

```cpp
    if (!dumpRoughnessPath.empty()) {
        std::vector<uint8_t> roughData;
        uint32_t rw = 0, rh = 0;
        if (!renderer.readbackRoughnessAOV(roughData, rw, rh)) {
            std::cerr << "[Roughness dump] readback failed\n";
        } else {
            stbi_write_png(dumpRoughnessPath.c_str(), rw, rh, 1, roughData.data(), rw);
            std::cout << "Saved roughness debug: " << dumpRoughnessPath << std::endl;
        }
    }
```

Replace the vector type and encode:

```cpp
    if (!dumpRoughnessPath.empty()) {
        std::vector<float> roughData;
        uint32_t rw = 0, rh = 0;
        if (!renderer.readbackRoughnessAOV(roughData, rw, rh)) {
            std::cerr << "[Roughness dump] readback failed\n";
        } else {
            // R16F → 8-bit grayscale PNG. Roughness is [0, 1], clamp + scale.
            std::vector<uint8_t> gray(static_cast<size_t>(rw) * rh, 0);
            for (uint32_t i = 0; i < rw * rh; i++) {
                float r = std::max(0.0f, std::min(1.0f, roughData[i]));
                gray[i] = static_cast<uint8_t>(r * 255.0f);
            }
            stbi_write_png(dumpRoughnessPath.c_str(), rw, rh, 1, gray.data(), rw);
            std::cout << "Saved roughness debug: " << dumpRoughnessPath << std::endl;
        }
    }
```

### Step 2.6: Update `env_demo.cpp` diffuse + specular dump blocks

Find the `dumpRGBA16FStream` lambda added during 3.C/T5 (search `dumpRGBA16FStream`). Currently it takes `const std::vector<uint16_t>& halfData` and calls `half2float(halfData[i*4+k])`. Change the signature + body to work on native `float` input:

Replace the existing lambda:

```cpp
    auto dumpRGBA16FStream = [&](const std::string& path, const std::vector<uint16_t>& halfData,
                                  uint32_t w, uint32_t h) { ... };
```

With (rename to `dumpRGBA32FStream` for clarity):

```cpp
    auto dumpRGBA32FStream = [&](const std::string& path, const std::vector<float>& data,
                                  uint32_t w, uint32_t h) {
        // Decode RGBA32F → Reinhard-tonemapped 8-bit RGB PNG.
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        float maxC = 0.0f;
        for (uint32_t i = 0; i < w * h; i++) {
            float r = data[i * 4 + 0];
            float g = data[i * 4 + 1];
            float b = data[i * 4 + 2];
            maxC = std::max({maxC, r, g, b});
            float rT = r / (r + 1.0f);
            float gT = g / (g + 1.0f);
            float bT = b / (b + 1.0f);
            rgb[i * 3 + 0] = static_cast<uint8_t>(std::max(0, std::min(255, int(rT * 255.0f))));
            rgb[i * 3 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, int(gT * 255.0f))));
            rgb[i * 3 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, int(bT * 255.0f))));
        }
        stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
        std::cout << "Saved " << path << " (max channel = " << maxC << ")" << std::endl;
    };
```

Find the `--dump-diffuse` + `--dump-specular` blocks (search `dumpDiffusePath`, `dumpSpecularPath`). Update the vector type declarations and rename the lambda call:

```cpp
    if (!dumpDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDiffuseRadiance(data, dw, dh)) {
            std::cerr << "[Diffuse dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpDiffusePath, data, dw, dh);
        }
    }

    if (!dumpSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackSpecularRadiance(data, sw, sh)) {
            std::cerr << "[Specular dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpSpecularPath, data, sw, sh);
        }
    }
```

The existing `half2float` lambda (hoisted in 3.C/T5 for MV dump use) stays unchanged — MV is still RG16F.

### Step 2.7: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-3c5-formats
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

### Step 2.8: Helmet scene dump — visual + max channel check

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t2_3c5_helmet.png 64 --denoise=none \
    --dump-roughness=/tmp/t2_3c5_rough.png \
    --dump-diffuse=/tmp/t2_3c5_diffuse.png \
    --dump-specular=/tmp/t2_3c5_specular.png 2>&1 | tail -8
```

Expected stdout lines:
```
Saved roughness debug: /tmp/t2_3c5_rough.png
Saved /tmp/t2_3c5_diffuse.png (max channel = X.X)
Saved /tmp/t2_3c5_specular.png (max channel = Y.Y)
```

Record the diffuse + specular max channel values. Specular is likely to exceed the previous 34048 cap now that the clamp is gone — that's the point.

### Step 2.9: Visual spot-check via Read tool

Use the Read tool on all three PNGs to confirm:
- **Roughness**: grayscale helmet silhouette, visor dark (glossy), matte plates gray, sky white (miss sentinel 1.0). Should look visually identical to the 3.C roughness dump at 8-bit display resolution.
- **Diffuse**: emissive panels visible (green/cyan), matte plate ambient noise, visor near-black. Same basic look as 3.C.
- **Specular**: visor + dome reflection of env; helmet body glossy. Reinhard may display *darker* than 3.C if max channel is significantly higher (tonemap compresses against the new peak).

If any image is uniform black/white or visibly mangled, investigate — either the readback byte count is off or the decoder has a bug.

### Step 2.10: Regression smoke — beauty unchanged

Compare beauty PNG against a pre-3.C.5 baseline:
```bash
# From the worktree root
./build/cornell_box /tmp/t2_3c5_cornell.png 16 --denoise=none 2>&1 | tail -3
```

Beauty path should be bit-identical to the pre-branch state. A pixel-level hash comparison would be overkill here — visual parity is the realistic bar (format-only change has no effect on `radiance` accumulation math).

### Step 2.11: Append verification log entry

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-19: Format upgrades — roughness R16F + radiance RGBA32F (Sub-plan 3.C.5)

Format promotions for quality-first NRD consumption:
- Binding 21 roughness: R8_UNORM → R16_SFLOAT (+2 MB at 1080p).
- Bindings 22+23 radiance: RGBA16F → RGBA32F (+33 MB at 1080p combined).
- Raygen clamp `min(..., 60000.0)` removed from diffuse+specular exit writes.

Readback helper signatures upgraded to native `std::vector<float>` for
cleaner debug consumers. env_demo dump decoders simplified (no more
half2float dance for diffuse/specular; MV still uses it).

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Roughness dump:** visually identical to pre-3.C.5 at 8-bit display.
  R16F AOV has finer precision at low-roughness (visor/glossy) regions
  that the PNG quantization hides; NRD will consume the full precision.
- **Diffuse dump:** unchanged in appearance (demodulation math unchanged).
  Max channel: <fill from stdout>.
- **Specular dump:** max channel: <fill from stdout>. If > 60000, this
  is the clamp removal taking effect on genuine HDR spikes. Reinhard
  display may show relative darkening vs pre-3.C.5.
- **Regression:** beauty output unchanged (format-only change — no
  radiance math modified).

Sub-plan 3.C.5 complete. Next: 3.C.6 (demod AOV exposure — write raw
radiance, expose firstHitDiffAlbedo / firstHitSpecColor at new bindings
so NRD remodulates downstream).
```

Fill `<fill from stdout>` with the actual max channel values from Step 2.8.

### Step 2.12: Commit

```bash
git add ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): upgrade readback helpers to native float + update dumps (3.C.5)

readbackRoughnessAOV / readbackDiffuseRadiance / readbackSpecularRadiance
now return std::vector<float> instead of raw uint8 / half-float bytes.
Roughness readback internally decodes R16F → float via a static
half_to_float helper. Diffuse/specular readbacks are direct memcpy
(RGBA32F is native IEEE single-precision).

env_demo dumps updated: roughness decoder works on native floats
clamped to [0,1]; diffuse/specular use a renamed dumpRGBA32FStream
lambda (no half2float). MV dump unchanged (still RG16F).

Verified on helmet + env_studio: roughness visually identical, diffuse
max = X.X, specular max = Y.Y (clamp removal visible on specular).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Fill the X.X / Y.Y values from Step 2.8. Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §1 roughness R8→R16F | T1 (steps 1.1, 1.3, 1.4, 1.5) |
| §1 radiance RGBA16F→RGBA32F | T1 (steps 1.2, 1.3, 1.4, 1.5) |
| §1 clamp removal | T1 (steps 1.3, 1.4, 1.5) |
| §2 readback signature cleanup | T2 (steps 2.1, 2.3, 2.4) |
| §4.3 R16F half-float decode in helper | T2 (steps 2.2, 2.3) |
| §4.4 env_demo dump decoder changes | T2 (steps 2.5, 2.6) |
| §5 verification (helmet dumps, beauty regression) | T2 (steps 2.8, 2.9, 2.10) |
| §10 verification log update | T2 (step 2.11) |

**Placeholder scan:** Only `<fill from stdout>` in the verification log template + `X.X`/`Y.Y` in the T2 commit message — intentional, filled at execution time.

**Type consistency:**
- `std::vector<float>&` consistently used across all 3 readback signatures + env_demo callers.
- `VK_FORMAT_R16_SFLOAT` + `VK_FORMAT_R32G32B32A32_SFLOAT` ↔ GLSL `r16f` + `rgba32f` match.
- `half_to_float` function name used consistently in T2 steps 2.2 and 2.3.
- `dumpRGBA32FStream` lambda renamed consistently in T2 steps 2.6 and diffuse/specular call sites.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-19-denoiser-subplan3c5-format-upgrades.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Same pattern that shipped 3.A + 3.B + 3.C cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
