# Module B: Lighting System for Path Tracer

## Problem

The path tracer has one hardcoded sphere light passed via push constants. No multiple lights, no light types, no environment lighting, no emissive mesh illumination. Every scene looks like it's lit by a single bare bulb in a void.

## Lighting Pipeline Overview

```
Scene Lights (actors)  ──┐
                         ├──► Light Buffer (SSBO) ──► NEE Multi-Light Sampling
Environment Map (HDR)  ──┤                                    │
                         │                              Shadow Ray
Emissive Meshes ────────┘                                    │
                                                       BRDF Evaluation
                                                             │
                                                       Radiance += contribution
```

The pipeline has three light source categories, built in order:

1. **Analytic lights** — explicit lights in the scene graph (sphere, directional, spot, area)
2. **Environment map** — HDR image lighting the entire scene (sky, IBL)
3. **Emissive mesh lights** — any triangle with emissive texture becomes a light source

NEE selects from ALL sources per bounce using power-weighted random selection.

## Light Types

| Type | Geometry | NEE Sampling | Use Case |
|------|----------|-------------|----------|
| **Sphere** | Point + radius | Random point on sphere surface | Bulbs, orbs, soft point lights |
| **Directional** | Infinite disk | Cone-sampled direction + angular radius | Sun, moon |
| **Spot** | Point + direction + cone | Sphere sample + cone falloff | Flashlights, stage lights |
| **Area rect** | Quad (2 edges) | Random point on parallelogram | Windows, screens, panels |
| **Environment** | HDR equirect/cubemap | Importance-sampled direction (alias table) | Sky, outdoor, studio |
| **Emissive mesh** | Triangle | Random point on triangle (barycentric) | Neon signs, lava, screens |

## Architecture

### Light Buffer (GPU-side SSBO, binding 12)

```glsl
struct GPULight {
    vec4 positionAndType;    // xyz=position, w=type (0=sphere,1=dir,2=spot,3=area)
    vec4 colorAndIntensity;  // rgb=color, w=intensity
    vec4 dirAndParam;        // xyz=direction, w=param (radius/innerAngle)
    vec4 extra;              // spot: w=outerAngle | area: xyz=edge1
    vec4 extra2;             // area: xyz=edge2, w=area
};

layout(set = 0, binding = 12) readonly buffer LightBuffer {
    uint lightCount;
    GPULight lights[];
} lightBuf;
```

### NEE Multi-Light Strategy

```
At each bounce:
  1. Choose light category: analytic vs environment vs emissive (power-weighted)
  2. Within category:
     - Analytic: pick one light (power-weighted), sample its geometry
     - Environment: importance-sample the HDR map
     - Emissive: pick random emissive triangle, sample barycentric point
  3. Trace shadow ray
  4. Evaluate BRDF, weight by 1/PDF
  5. Add to radiance

Cost: 1 shadow ray per bounce (same as now), but covers all light sources.
```

### Light Attenuation

```glsl
// Physical inverse-square with range cutoff
float attenuation(float dist, float range) {
    float d2 = dist * dist;
    float falloff = 1.0 / (d2 + 0.0001);
    // Smooth range cutoff (no hard edge)
    float window = max(1.0 - pow(dist / range, 4.0), 0.0);
    return falloff * window * window;
}
```

Directional lights have no attenuation. Spot lights multiply by angular falloff.

### Environment Map

- Load HDR equirectangular image (`.hdr` / `.exr`)
- Build importance sampling alias table on CPU at load time
- Upload as bindless texture + alias table buffer
- Miss shader returns environment color instead of black
- NEE can importance-sample the environment for direct lighting

### Emissive Mesh Lights

- After scene buffer upload, scan all materials for emissive textures
- Build an emissive triangle list: `{triangleID, emissivePower}`
- Upload as SSBO
- NEE samples random emissive triangle, evaluates emission at sample point
- Connects Module A emissive textures to the lighting system

### Scene Integration

```cpp
// Analytic lights via scene graph
auto sun = scene->createActor("Sun");
auto lc = sun->addComponent<LightComponent>();
lc->setType(LightType::Directional);
lc->setColor({1.0, 0.95, 0.85});
lc->setIntensity(5.0);
lc->setDirection({-0.5, -1.0, -0.3});

auto spot = scene->createActor("Spotlight");
auto lc2 = spot->addComponent<LightComponent>();
lc2->setType(LightType::Spot);
lc2->setInnerAngle(15.0f);  // degrees
lc2->setOuterAngle(30.0f);
spot->getTransform()->setPosition({0, 4, 2});

// Environment map
scene->setEnvironmentMap("studio_hdr.hdr");
```

## Build Order

### Task 1: Light buffer + scene integration
- Define GPULight struct (C++ + GLSL, matching layout)
- LightComponent: add LightType enum, radius, direction, cone angles, range
- Collect all LightComponent actors → GPU SSBO at scene buffer upload
- Path tracer descriptor: add binding 12 (light buffer)
- Remove hardcoded light from renderer.cpp render() call
- Update examples to create lights via scene graph
- **Test**: Cornell box with 3 colored sphere lights (red, green, blue)

### Task 2: Multi-light NEE + light types
- NEE reads from light buffer instead of push constants
- Random light selection (uniform first, power-weighted later)
- Sphere light sampling (existing, move to GLSL function)
- Directional light sampling (parallel rays, angular radius for soft sun)
- Spot light sampling (cone falloff between inner/outer angle)
- Light attenuation (inverse-square + range window)
- **Test**: Scene with sun + 2 spot lights + sphere light — all contributing

### Task 3: Area rectangle lights
- Area light defined by position + 2 edge vectors
- Random point sampling on parallelogram
- PDF = 1/area, geometric term includes light normal
- **Test**: Window lighting a dark room, glowing screen panel

### Task 4: Environment map (HDR/IBL)
- Load `.hdr` equirectangular via stb_image
- Upload as bindless texture
- Miss shader: sample environment instead of returning black
- Importance sampling: build 2D alias table on CPU
- NEE: sample environment as a light source alongside analytic lights
- **Test**: DamagedHelmet in outdoor HDR environment (no Cornell box)

### Task 5: Emissive mesh lights
- Scan scene for triangles with emissive materials
- Build emissive triangle list buffer (triangleID, emissivePower, area)
- NEE: sample random emissive triangle, evaluate emission
- Connects Module A emissive textures to actual scene illumination
- **Test**: Scene with neon signs / glowing objects lighting nearby surfaces

## Files Modified

| File | Changes |
|------|---------|
| `shaders/rt/pt_raygen.rgen` | Multi-light NEE, light buffer read, per-type sampling, env map |
| `shaders/rt/pt_miss.rmiss` | Return environment color instead of black |
| `ohao/render/rt/path_tracer.hpp/cpp` | Light buffer descriptor (binding 12), env map descriptor, remove push constant light |
| `ohao/gpu/vulkan/offscreen_scene_render.cpp` | Collect LightComponents → GPU light buffer, emissive triangle scan |
| `ohao/gpu/vulkan/renderer.cpp` | Remove hardcoded light, pass scene lights |
| `ohao/scene/component/light_component.hpp/cpp` | LightType enum, direction, cone angles, range |
| `examples/cornell_box.cpp` | Create lights via scene graph |
| `examples/model_viewer.cpp` | Auto-create key/fill/rim lights |

## Dependencies

- Module A (complete) — emissive textures needed for Task 5
- No external dependencies for Tasks 1-3
- Task 4 needs stb_image for HDR loading (already available)
