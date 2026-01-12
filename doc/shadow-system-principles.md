# Shadow System Design Principles

*Quick reference for building robust shadow mapping in Vulkan.*

---

## The Three Bugs That Break Shadow Mapping

### 1. Vulkan Depth Range Mismatch
**Symptom:** Shadow map has data but shadows don't appear.

**Cause:** GLM produces OpenGL clip space (Z: -1 to 1), Vulkan expects (Z: 0 to 1).

**Fix:**
```cpp
// After glm::ortho()
lightProj[2][2] = -1.0f / (farPlane - nearPlane);
lightProj[3][2] = -nearPlane / (farPlane - nearPlane);
lightProj[1][1] *= -1.0f;  // Y flip for Vulkan
```

### 2. GLSL Struct Copy Corruption
**Symptom:** Matrix values are garbage in shader.

**Cause:** Large struct copies corrupt data on some GPU drivers.

**Fix:**
```glsl
// BAD
UnifiedLight light = ubo.lights[i];
mat4 m = light.lightSpaceMatrix;  // Corrupted!

// GOOD
mat4 m = ubo.lights[i].lightSpaceMatrix;  // Direct access
```

### 3. Forgotten Uniform Updates
**Symptom:** Shadow map is empty (all 1.0).

**Cause:** Light-space matrix calculated but never uploaded to GPU.

**Fix:** Trace data flow: CPU → `uniformBuffer->write()` → Shader.

---

## Compile-Time Safety Principles

### 1. Single Source of Truth
```cpp
// ONE file defines constants
namespace ShaderBindings {
    constexpr int kMaxLights = 8;
    static_assert(kMaxLights == 8, "Update GLSL!");
}

// All code references it
array<Light, ShaderBindings::kMaxLights> lights;
```

### 2. Strong Typed Handles
```cpp
using LightHandle = StrongHandle<LightTag>;
using ShadowHandle = StrongHandle<ShadowTag>;

// Prevents: setLight(shadowIndex)  // Won't compile!
```

### 3. Layout Verification
```cpp
static_assert(sizeof(UnifiedLight) == 128);
static_assert(offsetof(UnifiedLight, matrix) == 64);
```

### 4. Bounds-Checked Access
```cpp
auto& light = checkedAccess(lights, handle, "context");
// Throws: "context: Out of range (id=99, size=8)"
```

### 5. Optional for Exhaustion
```cpp
std::optional<Tile> allocateTile();  // Forces caller to handle failure
```

---

## Debugging Checklist

- [ ] Force known output in shadow depth shader → Does pipeline work?
- [ ] Sample shadow map at fixed coords → Is data being written?
- [ ] Print light-space matrix → Are values reasonable?
- [ ] Check uniform buffer writes → Is data reaching GPU?
- [ ] Verify projection matrix Z range → OpenGL vs Vulkan?
- [ ] Test with hardcoded shadow coords → Is sampling correct?

---

## Architecture Summary

```
shader_bindings.hpp     ← Constants (binding indices, array sizes)
        ↓
descriptor_builder.hpp  ← Type-safe descriptor definitions
        ↓
csm_manager.hpp        ← Cascaded shadows (directional light)
shadow_atlas.hpp       ← Atlas shadows (point/spot lights)
        ↓
pcss.glsl              ← Soft shadow sampling
```

---

## Quick Reference: Coordinate Spaces

| Space | Range | Notes |
|-------|-------|-------|
| World | Infinite | Scene coordinates |
| Light View | Camera-relative | Looking from light |
| Light Clip | Vulkan: Z [0,1] | After projection |
| Shadow UV | [0,1] | Texture sampling |

Transform chain:
```
worldPos → lightView → lightProj → perspectiveDivide → uvTransform
```

---

*When shadows don't work, it's always one of: wrong matrix, wrong depth range, or data not reaching the GPU.*
