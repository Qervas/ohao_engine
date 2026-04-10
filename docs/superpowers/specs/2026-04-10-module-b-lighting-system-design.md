# Module B: Lighting System for Path Tracer

## Problem

The path tracer has one hardcoded sphere light passed via push constants. No multiple lights, no light types, no scene-defined lights, no environment lighting. Every scene looks like it's lit by a single bare bulb.

## Design

### Light Types

| Type | Geometry | NEE Sampling | Use Case |
|------|----------|-------------|----------|
| **Sphere** | Point + radius | Random point on sphere surface | Bulbs, orbs, soft point lights |
| **Directional** | Infinite plane | Cone-sampled direction | Sun, moon |
| **Spot** | Point + direction + cone | Cone-weighted sampling | Flashlights, stage lights |
| **Area rect** | Quad (4 corners) | Random point on quad | Windows, screens, panels |
| **Environment** | HDR cubemap/equirect | Importance-sampled direction | Sky, outdoor scenes |
| **Emissive mesh** | Any triangle | Random point on triangle | Neon signs, lava, screens |

### Phase 1: Multiple scene lights + light types (this spec)

- Sphere, directional, spot, area rect
- Light buffer (SSBO) replaces push constant light
- NEE randomly selects one light per bounce, weighted by power
- Scene graph integration — lights are actors with LightComponent

### Phase 2: Environment + emissive mesh (future)

- HDR skybox loading + importance sampling
- Emissive triangle light extraction from scene geometry

## Architecture

### Light Buffer (GPU-side SSBO)

```glsl
struct GPULight {
    vec4 positionAndType;    // xyz=position, w=type (0=sphere,1=dir,2=spot,3=area)
    vec4 colorAndIntensity;  // rgb=color, w=intensity
    vec4 params;             // sphere: w=radius | dir: xyz=direction | spot: xyz=dir,w=angle | area: unused
    vec4 params2;            // area: xyz=edge1 | spot: w=outerAngle | others: unused
    vec4 params3;            // area: xyz=edge2, w=area | others: unused
};
```

One buffer, up to 64 lights. Binding 12 in the path tracer descriptor set.

### NEE Multi-Light Strategy

At each bounce:
1. Pick one light randomly (uniform, or power-weighted for lower variance)
2. Sample a point on that light's geometry
3. Trace shadow ray
4. Weight by `numLights / selectionPDF`

This is unbiased and costs one shadow ray per bounce regardless of light count.

### Scene Integration

```cpp
// In test scene / example:
auto light = scene->createActor("KeyLight");
light->addComponent<LightComponent>();
light->getComponent<LightComponent>()->setType(LightType::Sphere);
light->getComponent<LightComponent>()->setColor({1, 0.95, 0.9});
light->getComponent<LightComponent>()->setIntensity(30.0f);
light->getComponent<LightComponent>()->setRadius(1.0f);
light->getTransform()->setPosition({0, 4, 0});
```

The renderer collects all LightComponents at scene buffer upload time and builds the GPU light buffer.

### Light Sampling Functions (GLSL)

```glsl
// Sample a point on a light, return: position, normal, PDF, emission
void sampleLight(GPULight light, vec2 rng, out vec3 lightPos, out vec3 lightNormal,
                 out float pdf, out vec3 Le);

// Sphere: random point on surface
// Directional: position = hitPos + dir * farDist, pdf = 1
// Spot: same as sphere but modulate by cone falloff
// Area: random point on quad, pdf = 1/area
```

## Build Order

### Task 1: Light buffer + multi-light NEE
- Define GPULight struct (C++ + GLSL)
- Collect lights from scene into SSBO at buffer upload time
- Replace push constant light with SSBO read in raygen shader
- Random light selection in NEE
- **Test**: Cornell box with 3 different colored lights

### Task 2: Directional + spot lights
- Add directional light sampling (parallel rays, no position)
- Add spot light sampling (cone falloff)
- **Test**: Outdoor scene with sun + spot lights

### Task 3: Area rectangle lights
- Quad light sampling (random point on parallelogram)
- **Test**: Window light, screen light

### Task 4: Scene integration
- Remove hardcoded light from offscreen_renderer.cpp
- Build light buffer from LightComponent actors
- Examples updated to create lights via scene graph
- **Test**: model_viewer auto-creates lights based on model bounds

## Files Modified

| File | Changes |
|------|---------|
| `shaders/rt/pt_raygen.rgen` | Multi-light NEE, light buffer read, per-type sampling |
| `ohao/render/rt/path_tracer.hpp/cpp` | Light buffer descriptor (binding 12), remove push constant light |
| `ohao/gpu/vulkan/offscreen_scene_render.cpp` | Collect LightComponents → GPU light buffer |
| `ohao/gpu/vulkan/offscreen_renderer.cpp` | Remove hardcoded light params from render() call |
| `ohao/scene/component/light_component.hpp` | Add LightType enum, radius, cone angle |
| `examples/cornell_box.cpp` | Create lights via scene graph |
| `examples/model_viewer.cpp` | Auto-create lights based on scene |
