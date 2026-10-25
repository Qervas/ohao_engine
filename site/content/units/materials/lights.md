---
module: materials
id: lights
title: Lights dual packing
standard: v2
figures: [materials-lights-packing]
---

## The same integer means two different lights

A scene author sets one thing: a `Light` component whose `LightType` runs Point = 0,
Directional = 1, Spot = 2, AreaRect = 3.

{{cite ohao/ecs/components.hpp "enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };"}}

Render extraction resolves each one against its world transform into a `LightDesc`
whose `LightKind` repeats those four and adds a fifth, `EmissiveProxy`:

{{cite ohao/render/scene/render_scene_types.hpp "enum class LightKind : uint32_t { Point, Directional, Spot, AreaRect, EmissiveProxy };"}}

Neither light buffer ends up holding only the authored lights. Model upload also
derives one point-like stand-in per model whose material carries an emissive texture
(`EmissiveProxyLight`, resolved to `EmissiveProxy`), and the light table writes it into
both buffers after the real lights — the ray-traced side stamps it in as a sphere at the
mesh centre, the deferred side as a point light — so every count on this page is scene
lights plus emissive stand-ins:

{{cite ohao/render/scene/light_table.cpp "gl.positionAndType = glm::vec4(d.position, 0.0f);  // sphere type"}}
{{cite ohao/render/scene/light_table.cpp "light.position = glm::vec4(d.position, 1.0f);  // type 1 = point light"}}

The path tracer takes the kind unmodified — the enum value is cast straight to float and
dropped into `positionAndType.w`:

{{cite ohao/render/scene/light_table.cpp "gl.positionAndType = glm::vec4(d.position, static_cast<float>(static_cast<uint32_t>(d.kind)));"}}

The deferred pipeline does not. Its `LightData` uses the classic raster convention,
0 = directional, 1 = point, 2 = spot:

{{cite ohao/render/scene/render_scene_types.hpp "alignas(16) glm::vec4 position;      // xyz = position, w = type (0=dir, 1=point, 2=spot)"}}

so `LightTable::set` swaps 0 and 1 on the way in. The remap is a single nested ternary:

{{cite ohao/render/scene/light_table.cpp "const int deferredType = (d.kind == LightKind::Point) ? 1 : (d.kind == LightKind::Directional) ? 0 : 2;"}}

AreaRect = 3 has no raster equivalent, so the else-arm sends it to 2, and the deferred
lighting loop treats the rectangle as a cone — attenuated by the component's cone angles,
30° and 45° on a default-constructed `Light`, which nobody authoring an area light
ever set:

{{cite shaders/core/deferred_lighting.frag "attenuation *= calculateSpotAttenuation(L, normalize(light.direction.xyz),"}}

Those two numbers are plain member initialisers on the component, and they travel
unchanged through `LightDesc` (same defaults) into `params.x` and `params.y` as cosines:

{{cite ohao/ecs/components.hpp "float innerConeDeg{30.0f};"}}

The renderer decides what an unlit scene looks like. With no light in the raster list —
no authored light and no emissive stand-in — `setFrameLights` lights the frame with a warm
fallback sun whose intensity is $\pi$, cancelling the diffuse BRDF's $1/\pi$ so that a
white Lambertian surface facing it reflects its colour at unit scale:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "sun.color = glm::vec4(1.0f, 0.98f, 0.95f, glm::pi<float>());"}}

{{figure materials-lights-packing "Byte layout of both light buffers and the type-code crossing between them. Offsets and struct sizes are read from gpu_light.hpp, shaders/shared/layout.json and light_table.cpp; the diagram is conceptual, not a memory dump."}}

:::key
`positionAndType.w` carries the `LightKind` enum; `LightData.position.w` does not.
Read either code with the other table in hand and every point light becomes a sun.
Area lights exist only on the ray-traced side.
:::

## Eighty bytes and sixty-four

The ray-traced pack is the small one. `GPULight` is five `vec4`s, std430, with a static
assert pinning the size so a stray member cannot silently desynchronise it from the GLSL
mirror:

{{cite ohao/render/rt/gpu_light.hpp "// 5 vec4s = 80 bytes per light, std430 layout"}}

`LightData` is 128 bytes on the host, and 64 of those are a per-light shadow matrix that
no shader reads: the light table fills it only for the one light it picks as shadow-map
caster (the first Point or Spot) and writes identity into every other slot, and the GPU
copy drops it. `LightClusters` uploads each light as a 64-byte `LightGpu`, the four
`vec4`s alone:

{{cite ohao/render/scene/render_scene_types.hpp "alignas(16) glm::mat4 lightSpaceMatrix; // Transform to light space for shadow mapping (64 bytes)"}}
{{cite shaders/shared/layout.json "{"name": "LightGpu", "doc": "one raster light of the light table (core/deferred_lighting.frag, compute/light_cluster.slang)","}}

Both buffers are capped, and the caps are different kinds of number. The raster table
holds `MAX_LIGHTS = 65536`, and both loops that fill it — authored lights first, then the
emissive stand-ins — `break` once it is full, with a warning logged once. It is a storage
buffer per frame slot, and the lighting pass reads only the lights in a pixel's froxel
(the clustered-lighting unit), so the count costs per pixel what reaches the pixel, not
what the scene holds:

{{cite ohao/render/scene/render_scene_types.hpp "constexpr uint32_t MAX_LIGHTS = 65536;"}}

The RT side has a real cap too. The device buffer is sized as the 16-byte header plus
`MAX_GPU_LIGHTS = 64` lights, and a longer list is truncated with a one-time log, so the
estimator's light count never exceeds what the buffer holds:

{{cite ohao/render/rt/gpu_light.hpp "static constexpr uint32_t MAX_GPU_LIGHTS = 64;"}}
{{cite ohao/render/scene/light_table.cpp "gpuLights.resize(MAX_GPU_LIGHTS);"}}

:::why
The obvious alternative is one union struct feeding both pipelines, and the byte counts say
why it was not taken: a shared layout drags the 64-byte shadow matrix into a buffer the
raygen re-reads once per next-event sample, and drags area edges and a sampling radius into
the record every lit pixel reads once per light in its froxel, and the clustering kernel
once per light per froxel. Two packs cost a remap function. One pack costs bandwidth in the
hot loop of both pipelines.
:::

## The sixteen bytes std430 hands over

`GPULight` has `vec4` alignment, so std430 rounds the array base of any buffer that begins
with a `uint` counter up to 16 regardless. The light table spends three of those four words —
the light count, the bindless environment-map index at offset 4, an HDRI intensity scale at
offset 8 — and fills the last with an all-ones constant rather than leaving it as padding:

{{cite ohao/render/scene/light_table.cpp "const uint32_t header[4] = {static_cast<uint32_t>(m_rt.size()), m_envMapTexIdx,"}}

The environment-map slot defaults to the no-texture sentinel, `0xFFFFFFFF` — the value the
miss shader tests for "no HDRI":

{{cite ohao/render/scene/light_table.hpp "uint32_t m_envMapTexIdx{kNoTexture};"}}

Binding 11 is declared four times across the ray-tracing stages, in two shapes. All three
raygen shaders name only `lightCount` and let std430 alignment skip the rest of the header.
`pt_miss.rmiss` spells the header out in full — `lightCount`, `envMapTexIdx`, `envIntensity`,
then a `uint _pad` it never reads — because it is the stage that samples the environment, and
it needs the third word as much as the second: the index selects the HDRI, the scale
multiplies it.

{{cite shaders/rt/pt_miss.rmiss "float envIntensity;   // HDRI scale"}}
{{cite shaders/rt/pt_miss.rmiss "payload.color = texture(textures[nonuniformEXT(lightBuf.envMapTexIdx)], uv).rgb * envS;"}}

That side channel is owned by the table, not by the upload. The index and the scale are held
as members, `setEnvMapTextureIndex` and `setEnvIntensity` mark the table dirty only when the
value actually changes, and the header is rewritten whole with the stored values whenever
anything is dirty. A light edit therefore re-uploads the header with the sky still in it, and
there is no re-stamping step to forget.

## Radiance divided by a sphere the light may not be

`dirAndParam.w` is overloaded: the packer fills it with the sphere radius, then, two lines
later, overwrites it with the inner cone angle in degrees when the kind is Spot.

{{cite ohao/render/scene/light_table.cpp "float dirParam = d.radius;"}}

The bounce-0 next-event-estimation block of the default `pt_raygen.rgen` converts intensity
to radiance *before* it branches on type, using the surface area of a sphere of that radius:

{{cite shaders/rt/pt_raygen.rgen "// ==== Analytic direct NEE at bounce 0 ===="}}

Write $c$ and $I$ for the packed colour and intensity, $r=\max(\texttt{dirAndParam.w},0.01)$,
$d$ for the distance from the shading point to the sampled light point, $\theta_\ell$ for the
angle at the light between its normal and the incoming direction, $s$ for the clamped spot
cone falloff, and $A$ for the rectangle area in `extra2.w`. The block forms
$L_e = cI/\max(4\pi r^2, 0.01)$ once, then each branch multiplies by its own geometric
weight, giving

$$L_e w = \frac{cI\cos\theta_\ell}{d^2},\quad \frac{cI}{4\pi r^2},\quad \frac{cI s^2}{d^2},\quad \frac{cI A\cos\theta_\ell}{4\pi r^2 d^2}$$

for sphere, directional, spot and area respectively. The $4\pi r^2$ reappears inside the
sphere and spot weights and cancels, which is why growing a sphere light softens its shadow
without brightening it. It does not cancel for the other two: a directional light's
brightness and a rect light's brightness both scale as $1/r^2$ in a radius that means
nothing for either shape. The component's default radius is 0.1, which makes the divisor
$4\pi r^2 \approx 0.126$ — so out of the box both shapes are quietly *brightened* roughly
eightfold relative to the intensity that was authored:

{{cite ohao/ecs/components.hpp "float radius{0.1f};"}}

The spot branch is worse than a scale factor. It reads `dirAndParam.w` twice in the same
block — once through `cos(radians(...))` as an angle, which is correct, and once unconverted
as the radius of the sphere it jitters the sampled light point over. The C++ struct is the
only place in the tree where both meanings are written down. `pt_raygen.rgen` and
`pt_raygen_offline.rgen` copy the comment but keep only the `sphere:radius` half;
`pt_raygen_realtime.rgen` carries no member comments at all. Anyone reading a shader alone
sees one meaning:

{{cite ohao/render/rt/gpu_light.hpp "glm::vec4 dirAndParam;        // xyz=direction, w=param (sphere:radius, spot:innerAngle)"}}
{{cite shaders/rt/pt_raygen.rgen "vec4 dirAndParam;        // xyz=direction, w=param (sphere:radius)"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "vec4 dirAndParam;"}}

A default-constructed spot, inner cone 30°, therefore has its sample point scattered over a
sphere of radius 30 world units, so its direction and its shadow ray are close to
meaningless. Sphere, directional and area lights do not share the fault — but every raygen
does. The same clamp-then-jitter pairing sits in each next-event block of all three: three
in `pt_raygen.rgen`, three in `pt_raygen_offline.rgen`, four in `pt_raygen_realtime.rgen`.

## One light per shadow ray, and the factor that pays for it

Each next-event sample picks a single light uniformly rather than looping over all of them.
With $N_\ell$ entries in the buffer — scene lights plus the synthesised emissive ones — the
selection density is $p(\ell)=1/N_\ell$, and an unbiased estimator divides by it, so the
contribution is multiplied by the light count:

$$\hat L_d = \frac{L_e\,w\,f_r\cos\theta_s}{p(\ell)} = N_\ell\,L_e\,w\,f_r\cos\theta_s$$

where $f_r$ is the Cook-Torrance value evaluated inline in the same block and $\theta_s$ is
the angle at the shading point. That `* float(lightBuf.lightCount)` is the visible half of
the division:

{{cite shaders/rt/pt_raygen.rgen "vec3 directContribution = Le * (diff + spec) * NdotL * weight * float(lightBuf.lightCount);"}}

The whole decode-and-sample block appears three times in `pt_raygen.rgen` — once at the
primary hit and once inside each of the two indirect bounce paths, where the identical
expression is pre-multiplied by that path's specular or diffuse throughput. It was then
copied wholesale into the two profile raygens: three more blocks in `pt_raygen_offline.rgen`,
four in `pt_raygen_realtime.rgen`. Neither file is dead — both are bound by a live render
profile:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_offline.rgen.spv"}}
{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}

The ten copies are not clones — the realtime sphere branch has since been replaced with
solid-angle sampling, and the offline block's primary hit wraps the diffuse `NdotL` with a
subsurface term the specular half does not get — but the header decode, the type dispatch
and the `dirAndParam.w` overload are the same in all of them. A light-layout change lands in
ten places across three files.

The shadow ray uses the sampled point's distance, not the light's radius, as `Tmax`, and the
three positional branches shorten it by 0.02 so the ray stops just short of the light's own
geometry; the origin is pushed 0.01 along the shading normal and `Tmin` is 0.001:

{{cite shaders/rt/pt_raygen.rgen "hitPos + N * 0.01, 0.001, L, shadowDist, 0);"}}

Directional lights get a hard-coded `Tmax` of 10000 instead, which is a scene-scale
assumption rather than a computed bound.

## The constants that name only one table

`constants.glsl` defines three light-type macros, and they are the raster codes:
directional 0, point 1, spot 2. The deferred lighting shader reaches the header through
its BRDF includes but compares `position.w` against bare integers, and no ray-tracing
stage includes it.

{{cite shaders/includes/common/constants.glsl "#define LIGHT_TYPE_POINT       1"}}

The macros are not merely unused, they are a trap: `LIGHT_TYPE_POINT` is 1, and 1 is
*directional* in the `GPULight` table this page opened with. Including the header into a
ray-tracing stage to avoid retyping three constants inverts the two most common light
types in the engine.

## Contracts

- `positionAndType.w` is the raw `LightKind`; `LightData.position.w` is the remapped raster code. Anything reading a light type must know which buffer it came from.
- `dirAndParam.w` means radius for sphere/directional/area and inner cone angle in degrees for spot. Every spot branch in all three raygens reads both meanings; only the angle reading is correct.
- Both buffers are filled from authored `Light`s *and* from emissive stand-ins. `lightCount` — and therefore the estimator's $N_\ell$ — counts both, and the raster table's capacity is shared between them.
- The raster table holds 65,536 lights and the RT SSBO 64; both truncate with a one-time log. Past the eighth shadow caster a raster light renders unshadowed (no RT channel), and a froxel keeps at most 256 lights, the overflow reported as `lightsOverflow` in `render.stats`.
- The environment-map index and intensity scale are members of the light table, and the 16-byte header is rewritten whole whenever any of them, or the light list, changes. Code that writes binding 11 by any other route would have to reproduce that header.
- The estimator multiplies by `lightCount` because it selects one light uniformly. Replacing uniform selection with importance sampling means replacing that factor with the real inverse density.
