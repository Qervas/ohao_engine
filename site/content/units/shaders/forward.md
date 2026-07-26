---
module: shaders
id: forward
title: Forward shaders
standard: v2
---

## The path the renderer lands on when nothing else works

`VulkanRenderer::render()` tries the RT pipelines, then deferred, and if neither
is live it falls through to the raster pair in `shaders/core/`:

{{cite ohao/gpu/vulkan/renderer.cpp "    // Forward rendering path"}}

`m_renderMode` is constructed as `Forward`, and `setRenderMode` refuses to switch
when the requested backend was never created — it logs and returns, leaving the
mode untouched:

{{cite ohao/gpu/vulkan/renderer.cpp "Deferred rendering not available, staying in Forward mode"}}

No example in `examples/` ever asks for `RenderMode::Forward`; they all request
Deferred or an RT profile. These two shaders are never chosen — they are what you
get when the choice fails, which makes their correctness a reliability question
rather than a cleanup one.

## Geometry comes from a UBO, material comes from a matrix

`forward.vert` takes the model matrix from the push constant but reads view and
projection from the camera UBO at set 0, binding 0:

{{cite shaders/core/forward.vert "gl_Position = camera.proj * camera.view * worldPos;"}}

That decision splits the path in half. The UBO is rewritten every frame, so
vertices land where they should; nothing the pair reads from the push block after
the model matrix does.

Both stages declare the same 96-byte block: `mat4 model`, `vec3 baseColor`, then
`metallic`, `roughness`, `ao`, and eight trailing bytes that are inert in the
vertex stage —

{{cite shaders/core/forward.vert "    vec2 padding;"}}

— and carry two bindless texture indices, packed as float bit patterns, in the
fragment stage:

{{cite shaders/core/forward.frag "    float albedoTexIdx;   // uint32 packed as float (0xFFFFFFFF = no texture)"}}

The two agree with each other byte for byte: under std430 push rules `baseColor`
occupies 64–76, `metallic` 76, `roughness` 80, `ao` 84, and the indices sit at 88
and 92, exactly covering `vec2 padding`. What they disagree with is what the
engine pushes. `ObjectPushConstants` places a second matrix at byte 64:

{{cite ohao/gpu/vulkan/renderer.hpp "glm::mat4 viewProj;"}}

So `baseColor` is `viewProj`'s first column, `metallic` is `viewProj[0].w`,
`roughness` and `ao` are the first two floats of column 1, and the texture indices
are `viewProj[1].z` and `viewProj[1].w`. The "no texture" sentinel is
`0xFFFFFFFF`, a bit pattern an ordinary finite float never carries, so the guard
passes and the shader samples `textures[]` at whatever index a projection-matrix
element encodes — a descriptor read far past the end of the bound array.

:::key
`forward.vert` and `forward.frag` are a self-consistent pair that is inconsistent
with the buffer the engine pushes. Positions are right because the transform
comes from a UBO; every material parameter past byte 64 is a reinterpreted matrix
element. Repairing this means rewriting both GLSL blocks against
`ObjectPushConstants`, not patching one field.
:::

## Eight lights, and at most one of them casts

The light loop is bounded twice — by the uploaded `numLights` and by the
compile-time cap that has to match the C++ `LightUniformBuffer`:

{{cite shaders/includes/common/types.glsl "#define MAX_LIGHTS 8"}}

Eight is the budget for the whole scene, and authored lights have first claim on
it. `updateLightBuffer` walks the scene's `LightComponent`s into the array, then
appends the cached emissive-mesh proxies only while `numLights` is still below
`MAX_LIGHTS` — emitters get the remainder, never a reserved share. The cache they
come from holds at most one proxy per actor, built from the first material on it
that has an emissive texture:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "break;  // one light per actor"}}

Shadowing is thinner than the data model suggests. `params.z` is documented as a
shadow map *index*, but one shadow map is bound (set 0, binding 2, a single
`sampler2D`) and the C++ side hands that index out once:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "int shadowCasterIndex = -1;  // Index of first shadow-casting light"}}

The eligibility test reads the raw `LightType` enum (Sphere = 0, Directional = 1,
Spot = 2) and accepts 0 or 2, so an authored directional light never gets the
shadow map — only a sphere (point) or a spot light does. Of those two only the
spot light ends up with a projection, because `calculateLightSpaceMatrix`
switches on the *remapped* type the same loop just wrote into `position.w`, where
Sphere has become 1:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "int lightType = static_cast<int>(light.position.w);"}}

1 matches neither its directional branch nor its spot branch, so a sphere light
falls out the bottom and receives `glm::mat4(1.0f)`:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "// Point lights don't use a single matrix (need cube maps)"}}

`shadow_pcf.glsl` then reads raw world position as clip space. After the
`* 0.5 + 0.5` the bounds test passes only where world $x$ and $y$ lie in
$[-1, 1]$ and world $z$ in $[0, 1]$: every fragment outside a 2 × 2 × 1 box at
the origin returns unshadowed. The fallback sun synthesized for a scene with no
lights is the one directional light that receives the map, and — type 0 in
`position.w` — the one whose matrix is actually built.

`forward.frag` is the only file in the tree that calls
`calculateShadowForLightIndex`; the deferred path uses CSM or an RT shadow mask.
The kernel is a fixed 5×5 PCF tap with a slope-scaled bias,
`shadowBias * tan(acos(N·L))` clamped to 0.01.

:::why
The shadow helper takes a light *index*, not a `Light`, and reaches into the UBO
by the global name `lighting` for each field. The obvious alternative — pass the
128-byte `Light` by value — was rejected for a reason recorded at the function:

{{cite shaders/includes/shadow/shadow_pcf.glsl "CRITICAL: Pass light INDEX, not Light struct, to avoid GLSL struct copy corruption!"}}

The cost is the coupling it creates. The header reads `lighting` at global scope
on seven lines and declares it on none, so it only compiles inside a translation
unit that has already declared that UBO — which is why `forward.frag` declares
the block first and includes second. Swap the two lines and `glslc` stops with
`'lighting' : undeclared identifier`, reported from inside the header rather than
from the file that broke it. The instance name is part of the interface, and the
header says so itself:

{{cite shaders/includes/shadow/shadow_pcf.glsl "Caller must define LightUBO named 'lighting' before including this file!"}}

Rename `lighting` in `forward.frag` and a file you did not touch stops compiling.
The block name `LightUBO` beside it is inert: member access goes through the
instance, so the header never spells it outside that comment.
:::

## The falloff that is not inverse square

Point and spot lights here attenuate through `attenuationSmooth`, whose only call
sites outside its own header are in `forward.frag` — the header's `attenuationSpot`
and `attenuationPoint` wrappers have no callers at all:

{{cite shaders/includes/lighting/light_attenuation.glsl "float attenuation = 1.0 / (1.0 + (distance * distance) / (range * range));"}}

With $d$ the fragment-to-light distance and $r$ the light's range (packed into
`direction.w`), the full factor is

$$a(d) = \frac{1}{1 + (d/r)^2}\;\Big[\operatorname{sat}\!\big(1 - d/r\big)\Big]^2$$

The trailing squared term exists because the first factor alone is $1/2$ at
$d = r$: a light cut off at its range would leave a visible disc edge. Squaring
the clamped linear ramp drives both the value and its first derivative to zero at
$d = r$, so the light fades out instead of popping.

Note that $a$ depends only on the ratio $d/r$. It is not inverse-square — at
$d = 0$ it is exactly 1 for every light regardless of range — so `range` is not
merely a culling distance, it is a second brightness control: widening a light's
range brightens every point already inside it.

The deferred pipeline made the opposite call for the same `LightData`:

{{cite shaders/core/deferred_lighting.frag "float invSq = 1.0 / max(d2, 0.01);  // inverse-square (physical)"}}

Genuine $1/d^2$ with a squared quartic window. Same scene, same light components,
and the two raster paths disagree on falloff in shape, not just by a constant.
Unlike the push-constant drift, this gap would survive a fix to the push block.

## Tangent space without a tangent

`forward.vert` declares all eight vertex attributes the pipeline binds — including
the `vec4` tangent with handedness in `w` — and forwards none past position,
color, normal and UV0. The fragment shader rebuilds a basis from screen-space
derivatives instead:

{{cite shaders/core/forward.frag "vec3 T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);"}}

{{cite shaders/core/forward.frag "vec3 B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);"}}

Expand those against the chain rule. With $J$ the screen-to-UV Jacobian,
$\partial P/\partial x$ and $\partial P/\partial y$ are linear combinations of
$\partial P/\partial u$ and $\partial P/\partial v$ with $J$'s entries as
coefficients; substituting them into the two lines above cancels the cross terms
and leaves

$$\mathbf{T}_{\text{raw}} = \det J \;\frac{\partial P}{\partial u},
\qquad
\mathbf{B}_{\text{raw}} = \det J \;\frac{\partial P}{\partial v}$$

— the two surface tangents, each carrying the *same* factor $\det J$. `normalize`
discards the magnitude but not the sign, and $\det J$ is negative exactly when the
UV parameter frame and the screen-space derivative frame have opposite
orientation, which for a given facing is exactly the mirrored island. Both axes
flip together there. The frame's determinant survives the double flip, since
$(-\mathbf{T}) \times (-\mathbf{B}) = \mathbf{T} \times \mathbf{B}$, so
`mat3(T, B, normal)` still has the handedness the mesh asked for — but the
in-plane part of the perturbation, $x\mathbf{T} + y\mathbf{B}$, comes out rotated
by 180°. Mirrored islands do not come out right; they come out with their
tangent-space detail pointing backwards.

That is one of three uncorrected costs. The frame is also constant across a
triangle for planar UVs, losing the smooth interpolation the attribute would have
given; and `T` and `B` are never orthogonalised against the interpolated normal,
so on UV-skewed triangles the matrix handed to `mat3(T, B, normal)` is not
orthonormal.

`gbuffer.frag`, on the same geometry, reaches those two expressions verbatim —
but only as a fallback for a degenerate attribute. Its first choice is the vertex
tangent, Gram-Schmidted against the normal, with handedness taken from the
authored `w`:

{{cite shaders/core/gbuffer.frag "T = normalize(fragTangent.xyz - N * dot(N, fragTangent.xyz));"}}

{{cite shaders/core/gbuffer.frag "B = normalize(cross(N, T)) * fragTangent.w;"}}

None of this is what the current build puts on screen. The branch is entered on

{{cite shaders/core/forward.frag "uint normalIdx = floatBitsToUint(material.normalTexIdx);"}}

and `material.normalTexIdx` is `viewProj[1].w`. Reinterpreting any normalized
float's bits gives an index of at least 8 million, against an array whose
descriptor count defaults to

{{cite ohao/gpu/vulkan/bindless_texture_manager.hpp "uint32_t m_maxTextures{4096};"}}

So the comparison above is between two pieces of code, not two images.

## Display encoding has to happen inside the fragment shader

There is no post chain here — no bloom, no TAA, no exposure pass — and the color
attachment is unsigned normalized, not sRGB, so the hardware encodes nothing on
write:

{{cite ohao/gpu/vulkan/framebuffer.cpp "colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;"}}

`forward.frag` therefore ends by tonemapping with the Narkowicz ACES fit and
applying a gamma curve itself:

{{cite shaders/core/forward.frag "color = linearToSRGBFast(color);"}}

The ACES fit clamps to $[0,1]$, so highlights are hard clipped inside the shading
pass and the readback buffer is already display-referred — nothing downstream can
recover them. And the encode is the gamma-2.2 approximation,

{{cite shaders/includes/common/color.glsl "return pow(linear, vec3(1.0 / 2.2));"}}

not the piecewise sRGB transfer function the same header also provides, whose
shadow segment is a straight line of slope 12.92 below linear 0.0031308:

{{cite shaders/includes/common/color.glsl "return linear * 12.92;"}}

The error runs the opposite way from what "fast gamma" suggests. The two curves
cross at linear $\approx 0.125$, and *below* the crossing the approximation
returns the higher code value: linear 0.001 encodes as 0.043 against a true
0.013, the gap peaking near 0.034 — 8.5 codes at 8 bits — around linear 0.002.
Near-black comes out lifted, not crushed. Above the crossing the
approximation is the darker of the two, by at most 0.006. Acceptable in a
fallback viewport; wrong for anything compared against the offline tracer.

## The branch that is never compiled

`forward.frag` carries a complete Blinn-Phong alternative behind `USE_PBR`, with
its own include of `blinn_phong.glsl`. The flag defends itself:

{{cite shaders/core/forward.frag "#define USE_PBR 1  // Enable PBR by default"}}

and nothing overrides it, because the shader build passes no preprocessor
definitions at all:

{{cite shaders/CMakeLists.txt "COMMAND ${GLSLC} --target-env=vulkan1.3"}}

Every shader goes through that one invocation — two `-I` paths, no `-D` — and the
standalone `compile_shaders.sh` adds none either, so the `#else` branch and
`blinn_phong.glsl` never reach SPIR-V. Never compiled is not the same as
uncompilable: adding `-DUSE_PBR=0` to that exact command line builds
`forward.frag` and its Blinn-Phong include without a diagnostic. The branch is
not rotten, just unreachable.

Worth knowing before deleting it: `blinn_phong.glsl` carries its own copy of the
`attenuationSmooth` falloff under a different name,

{{cite shaders/includes/lighting/blinn_phong.glsl "float calculateAttenuation(float distance, float range) {"}}

its edge ramp spelled `clamp(..., 0.0, 1.0)` where the other header calls
`saturate`, which `math.glsl` defines as that clamp. The arithmetic is identical; the
names are not, so the two headers can be included in one translation unit without
a redefinition. The duplication is a maintenance cost, not a merge blocker — and
deleting the branch takes one of the two copies of the falloff with it.

## Contracts

- `forward.frag` must declare the `LightUBO` named `lighting` **before** including `shadow_pcf.glsl`, which resolves that name at global scope without declaring it; `types.glsl` must precede both, since the UBO body uses `Light` and `MAX_LIGHTS`.
- `MAX_LIGHTS` is 8 in `types.glsl` and must match `LightUniformBuffer` in `renderer.hpp`. Emissive mesh proxies consume slots from the same eight.
- Both GLSL blocks describe 96 bytes that stop matching the 240-byte `ObjectPushConstants` at byte 64. Every material parameter and bindless index here is undefined until both are rewritten; the vertex transform is unaffected because it reads the camera UBO.
- The attachment is `R8G8B8A8_UNORM` with no post pass, so `forward.frag` owns tonemapping and transfer-function encoding. Remove either line and linear values ship into an 8-bit buffer.
- At most one light per frame carries a non-negative `params.z`, against a single `sampler2D` at set 0, binding 2. Treat `shadowMapIndex` as a boolean until a shadow atlas exists.
- The caster test switches on the `LightType` enum while `calculateLightSpaceMatrix` switches on the remapped type in `position.w`. A sphere light passes the first and misses both branches of the second, so it holds the shadow slot with an identity matrix. Change one encoding without the other and shadows move between "wrong light" and "no projection".
