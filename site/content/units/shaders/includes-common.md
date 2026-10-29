---
module: shaders
id: includes-common
title: Common includes
standard: v2
---

## Six headers, one preprocessor, no includer

`glslc` has no modules. `GL_GOOGLE_include_directive` splices text, and OHAO's
shader build hands the compiler two search roots — the `shaders/` tree and
`shaders/includes/` — so one header can be reached under more than one spelling
and more than once per translation unit.

{{cite shaders/CMakeLists.txt "-I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/includes"}}

That is why every file here opens with an `#ifndef OHAO_..._GLSL` guard. Today the
guards fire for nobody, because no stage shader includes any header in
`includes/common/` at all. The deferred lighting pass that used to reach them (through
`encoding.glsl` directly, and through the GLSL BRDF headers transitively) is Slang now,
`core/deferred_lighting.slang`, and Slang has modules: it imports the shared layout,
OpenPBR's module and the clustered light loop by name, and splices in no text.

{{cite shaders/core/deferred_lighting.slang "import includes.lighting.clustered_lights;"}}

`includes/common/` holds no constant the C++ side also needs. Those live in
`shaders/shared/layout.json` and reach GLSL and Slang through the generated
`shared/layout.glsl` and `shared/layout.slang`; the lighting pass imports the latter:

{{cite shaders/core/deferred_lighting.slang "import shared.layout;"}}

The absence of a module system on the GLSL side has a second consequence that shapes
everything below: nothing *obliges* a shader to use these headers. `common.glsl`
declares itself the bootstrap every shader should include first. No shader includes it.

{{cite shaders/includes/common/common.glsl "This file should be included first in all shaders"}}

## Headers with no consumer

`constants.glsl` is included by `common.glsl` and `math.glsl`; `math.glsl` by
`common.glsl` and `lighting/light_attenuation.glsl`. Nothing includes `common.glsl` or
`light_attenuation.glsl`, so no build parses any of them. The deferred frame shades through OpenPBR's
module (`material/openpbr.slang`), the realtime path tracer carries its own GGX
(`includes/material/ggx_aniso.glsl`) and its own constants, and neither reaches this
directory.

## The light struct is not here

No header in `includes/common/` declares a light. The raster light record the
deferred lighting pass and the clustering kernel read is `LightGpu`, four `vec4`s,
declared once in `shaders/shared/layout.json` and generated into GLSL, Slang and C++
by `tools/gen_layout.py`:

{{cite shaders/shared/layout.glsl "struct LightGpu {"}}

The C++ copy asserts every field's offset and the 64-byte size, and the three copies
come from one description, so the GLSL side of the boundary is checked too: editing
`layout.glsl` by hand makes `gen_layout.py --check` fail, and editing the JSON without
regenerating does the same.

{{cite ohao/gpu/shared_layout.hpp "static_assert(offsetof(LightGpu, params) == 48);"}}

`constants.glsl` still carries three light-type macros, `LIGHT_TYPE_DIRECTIONAL` 0,
`LIGHT_TYPE_POINT` 1 and `LIGHT_TYPE_SPOT` 2, matching the raster codes in
`LightGpu.position.w`. The lighting shader compares bare integers and uses none of
them.

{{cite shaders/includes/common/constants.glsl "#define LIGHT_TYPE_DIRECTIONAL 0"}}

## Octahedral normals: the shared copies ship none

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

That last step is where OHAO's version gets interesting. The decoder in
`encoding.glsl` undoes the remap on entry, so it expects $[0,1]$:

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The *encoder* sitting immediately below it does not apply the forward remap — it
returns $[-1,1]$ — so the two functions in the same file are not inverses. Neither
has a caller: nothing includes `encoding.glsl`. The GBuffer pass carries its own
private copy of the projection and applies the range remap itself, at the call site:

{{cite shaders/includes/scene/gbuffer_body.glsl "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

and every reader carries its own decoder with the matching remap: the lighting pass's
`decodeNormalOctahedron`, and the `decodeOctahedron` of SSAO, SSR and GI's denoiser.

{{cite shaders/core/deferred_lighting.slang "float3 decodeNormalOctahedron(float2 e)"}}

`math.glsl` supplies a third implementation, `octEncode`/`octDecode`, the only
self-consistent $[0,1]$ pair among the headers, with zero callers.

{{cite shaders/includes/common/math.glsl "vec2 octEncode(vec3 n) {"}}

:::why
The header comment justifies the packing by the GBuffer format, naming
`A2R10G10B10`. The attachment is actually `VK_FORMAT_R16G16B16A16_SFLOAT`
{{cite ohao/render/deferred/gbuffer_pass.cpp "VK_FORMAT_R16G16B16A16_SFLOAT,   // octahedral normal + roughness + emissive / -subsurface"}}
so the precision argument for octahedral encoding does not apply — fp16 would
hold a raw xyz normal comfortably. What the packing still buys is the *channel*:
two components for the normal leaves `.b` for roughness and `.a` for emissive
luminance in the same attachment. The rejected alternative — three components of
raw normal — costs another MRT attachment and its bandwidth on every GBuffer
write. The comment is stale about the reason; the decision is still right for a
different reason.
:::

## What the math and color headers actually deliver

`math.glsl` exports eighteen helpers and none reaches live code. `saturate` is called
eight times in `light_attenuation.glsl`, which no shader includes. Thirteen have no caller at all —
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
tree, and nothing calls either function. Neither header is even parsed: the shader
glob compiles only stage files, and no stage file includes either of them.

{{cite shaders/includes/common/encoding.glsl "// Used by: ssgi, volumetric_scatter"}}

There is no shared noise or hash header at all. The shaders that need
pseudo-randomness each roll their own: `pt_anyhit.rahit` an integer
bit-mixer (PCG), `sky.frag` and `dlss_tonemap.comp` float-valued hashes of the familiar
`fract`-of-products shape.

{{cite shaders/rt/pt_anyhit.rahit "uint pcg(uint v) {"}}

{{cite shaders/postprocess/sky.frag "p = fract(p * vec2(123.34, 456.21));"}}

## Contracts

- `gbuffer_body.glsl`'s private encoder returns $[-1,1]$ and the `×0.5+0.5` remap lives at its call site; every reader's decoder assumes $[0,1]$. Change either half alone and every consumer's lower-hemisphere normals flip. The copies in `encoding.glsl` and `math.glsl` are in no shader.
- The raster light record is `LightGpu` in `shaders/shared/layout.json`; a field changes there and is regenerated, never edited in `layout.glsl`, `layout.slang` or `shared_layout.hpp`.
- `light_attenuation.glsl` is the only file outside this directory that includes `math.glsl`, and nothing includes it; a new includer of either pulls in `constants.glsl`'s macros, whose values nothing checks against the C++.
- `constants.glsl`'s header says its values must match the C++ engine and points at `src/renderer/offscreen/offscreen_types.hpp`, a path that is not in the tree; nothing checks any of them.
{{cite shaders/includes/common/constants.glsl "// See: src/renderer/offscreen/offscreen_types.hpp"}}
