# How I Fixed Shadow Mapping in Vulkan: A Deep Dive

*A journey from "why is my shadow map all white?" to production-quality shadows with compile-time safety.*

---

## The Problem

I had a Vulkan renderer that should have been casting shadows. The shadow pipeline was set up, shaders were compiling, but no shadows appeared. The shadow map was returning all 1.0 (white) - meaning either nothing was being rendered to it, or the coordinates were completely wrong.

This is a common frustration in graphics programming: **everything looks right, but nothing works**.

---

## The Debugging Journey

### Step 1: Verify the Shadow Pipeline Works

First question: Is the shadow depth pass even running?

I added a debug output to the fragment shader that forced a specific color:
```glsl
// In shadow_depth.vert - force vertices to valid clip space
gl_Position = vec4(0.0, 0.0, 0.5, 1.0);
```

Result: Cyan color appeared in debug output. The pipeline works.

**Lesson: Isolate components. Prove each piece works independently.**

### Step 2: Check If Shadow Uniforms Are Being Updated

Searched the codebase for where `updateShadowUniforms` was called. Answer: **nowhere**.

The light-space matrix was being calculated but never uploaded to the GPU.

```cpp
// Added this call in the render loop
shadowRenderer->updateShadowUniforms(frameIndex, lightSpaceMatrix);
```

**Lesson: Follow the data path from CPU to GPU. Every uniform needs to be written.**

### Step 3: The Vulkan Depth Range Problem

This was the critical bug. After fixing the uniform upload, shadows still didn't work. Debug sampling showed the shadow map had depth values, but shadow calculations failed.

The issue: **GLM produces OpenGL clip space, but Vulkan expects different depth range.**

| | OpenGL | Vulkan |
|---|--------|--------|
| NDC X, Y | [-1, 1] | [-1, 1] |
| NDC Z | **[-1, 1]** | **[0, 1]** |
| Y direction | Up | **Down** |

GLM's `glm::ortho()` produces a projection matrix for OpenGL's Z range. In Vulkan, this causes all geometry to be clipped because Z values fall outside [0, 1].

**The Fix:**
```cpp
glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);

// Convert from OpenGL Z range [-1, 1] to Vulkan Z range [0, 1]
lightProj[2][2] = -1.0f / (farPlane - nearPlane);
lightProj[3][2] = -nearPlane / (farPlane - nearPlane);

// Flip Y for Vulkan's coordinate system
lightProj[1][1] *= -1.0f;
```

**Lesson: Know your API's coordinate conventions. OpenGL tutorials won't work directly in Vulkan.**

### Step 4: GLSL Struct Copy Corruption

Even after fixing the projection, shadows were inconsistent. Found another subtle bug:

```glsl
// BAD - copying struct corrupts data on some drivers
UnifiedLight light = ubo.lights[i];
vec4 pos = light.lightSpaceMatrix * vec4(worldPos, 1.0);  // Garbage!

// GOOD - access UBO members directly
vec4 pos = ubo.lights[i].lightSpaceMatrix * vec4(worldPos, 1.0);  // Works!
```

Some GPU drivers don't handle large struct copies correctly in GLSL. The 128-byte `UnifiedLight` struct with a `mat4` inside was getting corrupted during the copy.

**Lesson: Be wary of struct copies in shaders. Access UBO members directly when possible.**

---

## The Architecture: Compile-Time Safety

After fixing the immediate bugs, I rebuilt the shadow system with a principle:

> **If an error can be caught at compile time, it must be caught at compile time.**

### Principle 1: Single Source of Truth for Constants

Before:
```cpp
// In descriptor.cpp
bindings[1].descriptorCount = 4;  // Magic number

// In shader
#define MAX_SHADOW_MAPS 4  // Duplicated, can drift
```

After:
```cpp
// shader_bindings.hpp - THE source of truth
namespace ShaderBindings {
    constexpr int32_t kMaxShadowMaps = 4;

    static_assert(kMaxShadowMaps == 4,
        "Update GLSL if changed!");
}

// All code references this
bindings[1].descriptorCount = ShaderBindings::kMaxShadowMaps;
```

### Principle 2: Strong Typed Handles

Before:
```cpp
void setLight(uint32_t lightIndex);
void setShadowMap(uint32_t shadowMapIndex);

// Easy to mix up!
setLight(shadowMapIndex);  // Compiles but wrong
```

After:
```cpp
template<typename Tag, typename T = uint32_t>
struct StrongHandle {
    T id;

    // Prevent conversion from other handle types
    template<typename OtherTag>
    StrongHandle(const StrongHandle<OtherTag>&) = delete;
};

using LightHandle = StrongHandle<LightHandleTag>;
using ShadowMapHandle = StrongHandle<ShadowMapHandleTag>;

void setLight(LightHandle h);
void setShadowMap(ShadowMapHandle h);

// Now this fails to compile!
setLight(shadowMapHandle);  // Error: cannot convert
```

### Principle 3: Bounds-Checked Access with Context

```cpp
template<typename Handle, typename Container>
auto& checkedAccess(Container& c, Handle h, const char* context) {
    if (!h.isValid()) {
        throw std::out_of_range(
            std::string(context) + ": Invalid handle (id=" +
            std::to_string(h.id) + ")");
    }
    if (h.id >= c.size()) {
        throw std::out_of_range(
            std::string(context) + ": Out of range (id=" +
            std::to_string(h.id) + ", size=" +
            std::to_string(c.size()) + ")");
    }
    return c[h.id];
}

// Usage
auto& light = checkedAccess(lights, handle, "LightingSystem::updateLight");
// Error message: "LightingSystem::updateLight: Out of range (id=99, size=8)"
```

### Principle 4: Layout Verification with static_assert

```cpp
struct UnifiedLight {
    glm::vec3 position;      // offset 0
    float type;              // offset 12
    glm::vec3 color;         // offset 16
    float intensity;         // offset 28
    // ... etc
    glm::mat4 lightSpaceMatrix;  // offset 64
};

// Verify struct matches GPU expectations
static_assert(sizeof(UnifiedLight) == 128,
    "UnifiedLight must be 128 bytes for GPU alignment");
static_assert(offsetof(UnifiedLight, lightSpaceMatrix) == 64,
    "lightSpaceMatrix must be at offset 64");
```

### Principle 5: Optional Return for Resource Exhaustion

```cpp
// BAD - silent failure or crash
AtlasTile* allocateTile() {
    if (full) return nullptr;  // Caller might not check
    // ...
}

// GOOD - caller must handle
std::optional<AtlasAllocation> allocateTile() {
    if (full) return std::nullopt;
    // ...
}

// Usage - compiler reminds you to handle failure
if (auto tile = atlas.allocateTile()) {
    // Use tile
} else {
    // Handle full atlas gracefully
}
```

---

## The Final Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    SHADOW SYSTEM                             │
├─────────────────────────────────────────────────────────────┤
│  Directional Light          │    Local Lights (up to 16)    │
│  ┌────────────────────┐     │    ┌─────────────────────┐    │
│  │ CSM (4 cascades)   │     │    │   Shadow Atlas      │    │
│  │ 2048×2048 each     │     │    │   4096×4096         │    │
│  └────────────────────┘     │    │   (16 × 1024 tiles) │    │
│           │                 │    └─────────────────────┘    │
│           ▼                 │             │                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              PCSS Soft Shadows                        │   │
│  │  • Contact-hardening (sharp near, soft far)          │   │
│  │  • 16-sample blocker search + 25-sample PCF          │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### Files Created

| File | Purpose |
|------|---------|
| `shader_bindings.hpp` | Single source of truth for constants |
| `ohao_vk_descriptor_builder.hpp` | Type-safe descriptor definitions |
| `csm_manager.hpp/cpp` | Cascaded shadow maps for directional light |
| `shadow_atlas.hpp/cpp` | Shadow atlas for point/spot lights |
| `pcss.glsl` | Soft shadow algorithm |
| `compile_time_tests.hpp` | 40+ static_assert validations |

---

## Key Takeaways

### For Vulkan Developers

1. **GLM is OpenGL-centric.** Always convert projection matrices for Vulkan's depth range and Y flip.

2. **Don't copy large structs in GLSL.** Access UBO members directly to avoid driver-specific corruption.

3. **Verify data flow.** CPU → Uniform Buffer → Shader. Every link must work.

### For Graphics Programmers

1. **Debug visually.** Force known outputs to prove each stage works.

2. **Isolate problems.** Is it the shadow map? The sampling? The matrix? Test each independently.

3. **Know your coordinate spaces.** World → View → Clip → NDC → Screen. Track your data through each transform.

### For Software Engineers

1. **Compile-time errors > Runtime errors > Silent failures.** Push validation as early as possible.

2. **Make illegal states unrepresentable.** Strong types prevent mixing IDs. Optional returns force handling failures.

3. **Single source of truth.** One definition, referenced everywhere, validated automatically.

---

## The Result

From "shadow map is all white" to production-quality shadows with:
- Cascaded shadow maps for large scenes
- Shadow atlas for multiple local lights
- PCSS soft shadows with contact hardening
- Type-safe architecture that catches errors at compile time

The sphere finally casts its shadow.

---

*Graphics programming is 90% debugging coordinate spaces and data flow. The other 10% is wondering why the screen is black.*
