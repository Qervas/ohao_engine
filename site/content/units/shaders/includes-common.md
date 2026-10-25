---
module: shaders
id: includes-common
title: Common includes
standard: v2
figures: [shaders-includes-common-oct-path]
---

## Eight headers, one preprocessor, no module system

`glslc` has no modules. `GL_GOOGLE_include_directive` splices text, and OHAO's
shader build hands the compiler two search roots — the `shaders/` tree and
`shaders/includes/` — so one header can be reached under more than one spelling
and more than once per translation unit.

{{cite shaders/CMakeLists.txt "-I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/includes"}}

That is why every file here opens with an `#ifndef OHAO_..._GLSL` guard. No
shader in the tree currently reaches the same header twice, so today the guards
fire for nobody — but the graph is one include away from needing them:
`math.glsl` is pulled in by both `brdf_common.glsl` and `light_attenuation.glsl`,
so a shader that took the BRDF and the attenuation header together would reach it
twice, and without the guard `saturate` would be redefined and the shader would
fail to compile. Macros behave differently. `types.glsl` and `constants.glsl`
both `#define MAX_LIGHTS 8`, and `glslc` rejects a redefinition only when the
substitution *differs* — so the `#ifndef MAX_LIGHTS` wrapper in `constants.glsl`
is not what keeps copies compatible; agreeing on `8` is. `deferred_lighting.frag`
proves it, spelling the macro out unconditionally after `brdf_ggx.glsl` has
already dragged `constants.glsl` in, and compiling anyway. What breaks the build
is a copy drifting to `16`.

{{cite shaders/core/deferred_lighting.frag "#define MAX_LIGHTS 8"}}

The absence of a module system has a second consequence that shapes everything
below: nothing *obliges* a shader to use these headers. `common.glsl` declares
itself the bootstrap every shader should include first. No shader includes it.

{{cite shaders/includes/common/common.glsl "This file should be included first in all shaders"}}

## The transitive chain deferred lighting depends on without naming

`constants.glsl` looks like a bag of macros nobody needs. It has exactly one live
consumer, three includes down. `brdf_ggx.glsl` — the BRDF `deferred_lighting.frag`
calls — spends `EPSILON` at six sites. Three are genuine
division guards, `max(denom, EPSILON)` in the GGX distribution, in the
Schlick-GGX geometry term and in the height-correlated Smith visibility:

{{cite shaders/includes/brdf/brdf_ggx.glsl "return a2 / max(denom, EPSILON);"}}

The other three are `max(dot(N, V), EPSILON)` lower bounds on a cosine. Nothing
is divided there; they only keep a grazing `NdotV` or `NdotL` off exact zero
before it is fed to the three that are.

`brdf_ggx.glsl` never includes `constants.glsl`. It includes `brdf_common.glsl`,
one hop away, which includes `math.glsl` — for `saturate` and `INV_PI`, both of
which it genuinely uses — and `math.glsl` includes `constants.glsl`, annotating
the line with the reason.

{{cite shaders/includes/brdf/brdf_common.glsl "includes/common/math.glsl"}}

Every link on that chain is declared, then, except the last consumer:
`brdf_ggx.glsl` reads `EPSILON`, `PI` and `INV_PI` while naming no file that
defines any of them. Dropping `math.glsl` from `brdf_common.glsl` because "it
only uses saturate, let's inline it" is a hard compile error, and it fires at the
edit site first — `saturate` in both Fresnel functions, `INV_PI` in both diffuse
lobes — before the cascade reaches `brdf_ggx.glsl` at all. An undefined macro in
GLSL stays an identifier rather than becoming zero, so there is no `inf` to
observe: the build simply stops.

## The Light struct is the one hard ABI here

`types.glsl` is not a utility header; it is a GPU-side mirror of a C++ struct.
Its `Light` is four `vec4`s plus a `mat4` — 128 bytes in std140 — matching the
`LightData` the renderer uploads every frame.

{{cite shaders/includes/common/types.glsl "mat4 lightSpaceMatrix;  // Transform to light space"}}

No shader includes it. The one raster stage that reads that buffer,
`deferred_lighting.frag`, declares its own field-for-field copy of the struct
inline, so this header is a second copy of the ABI with no consumer, and the copy
that matters lives in a shader body.

{{cite shaders/core/deferred_lighting.frag "Light lights[MAX_LIGHTS];"}}

The C++ side pins the size at compile time; the GLSL side has no equivalent
check, so the assert protects only half the boundary.

{{cite ohao/render/scene/render_scene_types.hpp "OHAO_ASSERT_GPU_LAYOUT(LightData, layout::kLightDataBytes);"}}

The headers that would consume `types.glsl` carry a naming contract only a
comment enforces: `shadow_pcf.glsl` and `blinn_phong.glsl` read the light UBO by
the bare global name `lighting` without ever declaring it, so an includer has to
declare the block under that exact name *above* the include line.
`deferred_lighting.frag` happens to name its block `lighting`, but includes
neither header; `light_attenuation.glsl` takes everything by parameter.

{{cite shaders/includes/shadow/shadow_pcf.glsl "IMPORTANT: Caller must define LightUBO named 'lighting' before including this file!"}}

## Octahedral normals: three encoders, two decoders, one of each ships

A unit normal has two degrees of freedom but three components. Octahedral
mapping trades a little angular precision for a channel: project the normal onto
the L1 unit ball,

$$p = \frac{(n_x, n_y)}{|n_x| + |n_y| + |n_z|}$$

which is a bijection from the upper hemisphere onto the square $|p_x|+|p_y|\le 1$.
The lower hemisphere ($n_z < 0$) is folded outward into the four corner triangles,

$$p' = \big(1 - |(p_y, p_x)|\big) \odot \operatorname{sign}(p)$$

where $\odot$ is componentwise multiply and $\operatorname{sign}$ maps zero to
$+1$. The result covers $[-1,1]^2$; storing it in an unsigned target needs one
more step, $p \mapsto 0.5p + 0.5$.

That last step is where OHAO's version gets interesting. The shared decoder
undoes the remap on entry, so it expects $[0,1]$:

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The shared *encoder* sitting immediately below it does not apply the forward
remap — it returns $[-1,1]$ — so the two functions in the same file are not
inverses. They never have to be, because the shared encoder has no callers at
all. The GBuffer pass carries its own private copy of the projection and applies
the range remap itself, at the call site:

{{cite shaders/core/gbuffer.frag "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

`math.glsl` then supplies a *third* implementation, `octEncode`/`octDecode`,
which is the only self-consistent $[0,1]$ pair in the tree and has zero callers.

{{cite shaders/includes/common/math.glsl "vec2 octEncode(vec3 n) {"}}

{{figure shaders-includes-common-oct-path "Producer/consumer split for the octahedral normal channel. Conceptual — a data-flow map read from the include graph and GBuffer1 layout, not a captured render or a measurement."}}

:::why
The header comment justifies the packing by the GBuffer format, naming
`A2R10G10B10`. The attachment is actually `VK_FORMAT_R16G16B16A16_SFLOAT`
{{cite ohao/render/deferred/gbuffer_pass.cpp "attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}
so the precision argument for octahedral encoding does not apply — fp16 would
hold a raw xyz normal comfortably. What the packing still buys is the *channel*:
two components for the normal leaves `.b` for roughness and `.a` for emissive
luminance in the same attachment. The rejected alternative — three components of
raw normal — costs a fifth MRT attachment and its bandwidth on every GBuffer
write. The comment is stale about the reason; the decision is still right for a
different reason.
:::

## What the math and color headers actually deliver

`math.glsl` exports eighteen helpers. One of them reaches live code: `saturate`,
at the two Fresnel call sites in `brdf_common.glsl` (its other eight call sites
sit in `light_attenuation.glsl`, which no shader includes). The other seventeen have no live caller. Thirteen have no caller at all —
`remap`, `remapClamped`, `linearstep`, `fastAtan2`, `lengthSquared`,
`safeNormalize`, `reject`, `getRotationMatrix`, `smoothHermite`,
`smootherHermite`, `gain`, `packNormal`, `unpackNormal` — and so do `octEncode`
and `octDecode` above. The last two, `project` and `bias`, are called only from
inside this same file, by the already-dead `reject` and `gain`. Nothing is
shadowed by an inlined copy either: the only `safeNormalize` in the engine is an
unrelated C++ function in the physics module.

`color.glsl` is in the same position wholesale: no shader includes it. Its
`luminance` and `tonemapACES` have live counterparts, but they are private copies
— `tonemapping.frag` defines its own ACES fit rather than including this one.

{{cite shaders/includes/common/color.glsl "float luminance(vec3 color) {"}}

{{cite shaders/postprocess/tonemapping.frag "vec3 ACESFilm(vec3 x) {"}}

## Reconstruction and depth linearization: one live overload

`reconstruction.glsl` has no live entry point left and no includer. Its last caller
was the GLSL SSAO shader, which called the `mat4` overload of `reconstructViewPos`
twice; the Slang SSAO that replaced it reads world positions from the G-buffer
instead. The compact overload that takes `(1/proj[0][0], 1/proj[1][1], near, far)`
to save 48 bytes of push constants never had a caller, nor did
`reconstructWorldPos` and `reconstructWorldDir`. The header's usage comments name consumers —
`volumetric_scatter`, `cloud.comp`, `water.frag` — that do not exist anywhere in
`shaders/`.

{{cite shaders/includes/common/reconstruction.glsl "// Used by: cloud.comp, water.frag (ray origin/direction from screen pixel)"}}

Both depth linearizers in `encoding.glsl` are in the same position: their
comments name `ssgi`, `volumetric_scatter` and `dof_coc`, none of which is in the
tree, and nothing calls either function. They are not entirely unchecked: the
whole of `encoding.glsl` is spliced into `deferred_lighting.frag`, its one remaining
includer, so its uncalled functions are still parsed on every shader build — they
simply never reach a call site. `reconstruction.glsl` is not even parsed: the shader
glob compiles only stage files, and no stage file includes it.

{{cite shaders/includes/common/encoding.glsl "// Used by: ssgi, volumetric_scatter"}}

There is no shared noise or hash header at all. The shaders that need
pseudo-randomness each roll their own: `particle_emit.comp` an integer
bit-mixer, `sky.frag` and `dlss_tonemap.comp` float-valued hashes of the familiar
`fract`-of-products shape.

{{cite shaders/particles/particle_emit.comp "uint hash(uint x) {"}}

{{cite shaders/postprocess/sky.frag "p = fract(p * vec2(123.34, 456.21));"}}

## Contracts

- `gbuffer.frag`'s private encoder returns $[-1,1]$ and the `×0.5+0.5` remap lives at its call site; `decodeNormalOctahedron` assumes $[0,1]$. Change either half alone and every consumer's lower-hemisphere normals flip.
- `types.glsl`'s `Light` must stay field-for-field and 128 bytes aligned with C++ `LightData`. Only the C++ side is statically asserted.
- `deferred_lighting.frag`'s inline `Light` is the struct that must stay field-for-field with C++ `LightData`; `types.glsl` is a duplicate no shader compiles. Any shader that adopts `shadow_pcf.glsl` or `blinn_phong.glsl` must declare its light UBO as `lighting` above the include, because both read that global without declaring it.
- `brdf_ggx.glsl` gets `EPSILON` transitively through `brdf_common.glsl` → `math.glsl` → `constants.glsl` and declares none of it. That chain is required by the deferred lighting pass.
- `constants.glsl`'s `MAX_LIGHTS 8` must match `render_scene_types.hpp`; its comment points at `src/renderer/offscreen/offscreen_types.hpp`, a path that is not in the tree.
{{cite shaders/includes/common/constants.glsl "// See: src/renderer/offscreen/offscreen_types.hpp"}}
