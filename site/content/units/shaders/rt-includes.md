---
module: shaders
id: rt-includes
title: RT shader includes
standard: v2
figures: [shaders-rt-includes-hitdist-smc]
---

## What is left to include

The path tracer includes nothing. `rt/path_tracer.slang` is one Slang file whose four
entry points share their declarations by being in the same file — the ray payload
among them, declared once rather than matched byte-for-byte across stage files — and
it reaches shared code by importing two Slang modules, the generated layout and the
OpenPBR base:

{{cite shaders/rt/path_tracer.slang "import shared.layout;"}}

{{cite shaders/rt/path_tracer.slang "struct PathPayload"}}

What remains under `shaders/includes/` is three GLSL headers and one Slang module.
`includes/rt/nrd_frontend.glsl` is NRD's front and back end, and its one includer is
the compose pass:

{{cite shaders/rt/nrd_compose.comp "#include "includes/rt/nrd_frontend.glsl""}}

`includes/scene/scene_data.glsl` declares the scene tables the raster vertex stages
read (the G-buffer's and the cascade depth pass's), and `includes/scene/gbuffer_body.glsl`
is the G-buffer fragment body that `gbuffer.frag` and `gbuffer_mask.frag` share.
`includes/lighting/clustered_lights.slang` is the light loop (shaders/includes-lighting).

## The instance row a hit reads

The instance table carries no material word. Its tenth row, `emitterLight`, is zero
on every instance except a light's emitter mesh, where it holds the mesh's radiance
(`.rgb`) and the light's index in the RT light list (`.w`):

{{cite shaders/includes/scene/scene_data.glsl "vec4 emitterLight;   // an emitter mesh's radiance (rgb) and light index (w)"}}

The path tracer's closest hit reads that row by its position in the instance table and
returns the index on an emitter hit, so the raygen can weigh a bounce ray's hit
against light sampling:

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}

## Normalising a hit distance so REBLUR can decode it

REBLUR's `IN_DIFF_RADIANCE_HITDIST` and `IN_SPEC_RADIANCE_HITDIST` inputs are single
RGBA images: radiance in `.rgb`, and in `.a` a hit distance squeezed into [0,1].
REBLUR re-expands that alpha with its own normalisation, so the writer's function and
the denoiser's must be the *same* function or the decoded distance is wrong. The
normalisation is

$$\hat{d} \;=\; \mathrm{saturate}\!\left(\frac{d}{f}\right), \qquad f \;=\; \bigl(A + |z_{\text{view}}|\,B\bigr)\cdot\bigl(C + (1-C)\,\mathrm{smc}(r)\bigr)$$

where $d$ is the traced hit distance in world units, $z_{\text{view}}$ the view-space
depth of the primary hit, $r$ perceptual roughness, and $\mathrm{smc}$ a lobe-spread
term running from 0 at a mirror to 1 at a fully rough surface. The $A + |z|B$ factor
grows the scale with distance, so a reflection seen from far away can still be tens of
metres long without saturating; the second factor stretches it by up to $C$ for sharp
lobes, whose hit distances are longest. The writer is the path tracer's raygen, which
carries its own copy of the packing — Slang cannot include the GLSL header — with
$A=3$, $B=0.1$, $C=20$ written as literals:

{{cite shaders/rt/path_tracer.slang "const float f = (3.0 + abs(viewZ) * 0.1) * lerp(20.0, 1.0, smc);"}}

`nrd_frontend.glsl` holds the same function with the triple as a constant,
`NRD_HIT_DIST_PARAMS`; its packing half has no caller (the compose pass uses only the
inverse transform below):

{{cite shaders/includes/rt/nrd_frontend.glsl "const vec3 NRD_HIT_DIST_PARAMS = vec3(3.0, 0.1, 20.0);"}}

Everything matches NRD except $\mathrm{smc}$.

## Where the port diverges, and by how much

NRD v4.17 computes $\mathrm{smc}(r) = \bigl(1 - 2^{-200r^2}\bigr)\sqrt{r}$ — the
`_NRD_GetSpecMagicCurve` helper in `NRD.hlsli`, which CMake fetches rather than
vendors in-tree. OHAO substitutes plain $r^2$, in both copies:

{{cite shaders/rt/path_tracer.slang "const float smc = saturate(roughness * roughness);"}}

{{cite shaders/includes/rt/nrd_frontend.glsl "float smc = roughness * roughness; // lobe-spread proxy"}}

The two agree at $r = 0$ and $r = 1$, and cross once more at $r \approx 5 \times
10^{-5}$, where both sit below $10^{-8}$ and nothing depends on which is larger.
Above that crossing the proxy is much smaller all the way to $r = 1$, so the second
factor stays nearer $C$, $f$ comes out too large, and the normalised distance in `.a`
too small. Evaluating both closed forms at the shipping constants, the overshoot in
$f$ peaks at 2.63x near $r = 0.76$.

{{figure shaders-rt-includes-hitdist-smc "The lobe-spread term smc(r): NRD v4.17's magic curve against the r-squared proxy the path tracer's packing uses. Both curves are evaluated from the closed forms, not measured from a render; the annotated 2.63x is the resulting peak overshoot in the normalisation factor f at A=3, B=0.1, C=20."}}

Because REBLUR de-normalises with its own curve, it reconstructs a hit distance up to
~2.6x shorter than the raygen traced across the mid-to-rough range. Hit distance sizes
REBLUR's specular reprojection and blur footprints, so the error is systematic, not
noise. Its perceptual cost is not measured here.

:::why
The A/B/C triple is compiled into the shader, while the C++ side exposes only A:
`NrdReblurProfile` carries a `hitDistanceParamA` field and no B or C, and
`setReblurSettings` starts from `nrd::ReblurSettings`'s defaults and overrides that one
member. Both sides sit at A = 3.0 today, but nothing enforces it: turning the
`hitDistanceParamA` knob in C++ silently desynchronises the raygen's packing from the
denoiser consuming it. Uploading the triple with the other per-frame parameters would
close that; the push-constant block has one free lane (`tuning.w`), not three.
:::

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "s.hitDistanceParameters.A         = p.hitDistanceParamA;"}}

## The YCoCg round trip, and a clamp that earns its place

REBLUR takes radiance in YCoCg and returns it the same way. The raygen packs into
YCoCg (Y = ¼R + ½G + ¼B, Co = ½R − ½B, Cg = −¼R + ½G − ¼B), and `nrd_frontend.glsl`
carries the exact algebraic inverse, so an unfiltered round trip is lossless. The
compositor unpacks before remodulating by the demodulated albedo and F0 AOVs:

{{cite shaders/rt/nrd_compose.comp "vec3 diffRad    = nrdYCoCgToLinear(imageLoad(inDiffRad,    p).rgb);"}}

The clamp at the end of the inverse is not cosmetic:

{{cite shaders/includes/rt/nrd_frontend.glsl "return max(r, vec3(0.0));"}}

What comes back from REBLUR is not a round trip but a *filtered* YCoCg signal, and a
luma/chroma pair blended from neighbouring pixels can invert to a negative channel.
Without the clamp those negatives survive remodulation and land in the HDR buffer.

## The ray masks are not a header here

The masks a trace passes are generated constants, one source for C++, GLSL and Slang:
`shaders/shared/layout.json` names the instance masks and the ray masks, and
`tools/gen_layout.py` writes them into `shared/layout.glsl` and its Slang twin, which
the path tracer imports. The camera ray passes `kRayMaskCamera`, shadow rays
`kRayMaskScene`, and bounce rays `kRayMaskScene` plus the emitter mask, so a bounce
that meets a light's mesh is weighed against light sampling (hybrid/visibility has the
scheme):

{{cite shaders/rt/path_tracer.slang "const PathPayload hit = trace(origin, dir, depth == 0u ? kRayMaskCamera : (kRayMaskScene | kTlasMaskEmitter), 1.0e4);"}}

## The build tracks every header

The list that decides when a GLSL shader is recompiled is globbed from one pattern:

{{cite shaders/CMakeLists.txt "includes/**/*.glsl"}}

It matches only files at least one directory below `shaders/includes/`. All three GLSL
headers sit there, so editing any of them recompiles every GLSL stage. The Slang
modules are tracked separately: every Slang target depends on every `.slang` file.

## Contracts

- The path tracer's NRD packing (`nrdPackRadianceHitDist` in `path_tracer.slang`), `NRD_HIT_DIST_PARAMS` in `nrd_frontend.glsl` and `nrd::ReblurSettings::hitDistanceParameters` in C++ must hold the same A, B, C. Only A is exposed through `NrdReblurProfile`; B and C must therefore stay at NRD's defaults of 0.1 and 20.
- The compose pass's `nrdYCoCgToLinear` must invert the raygen's YCoCg exactly; the two live in different languages and nothing checks them against each other.
- A GLSL header must sit at least one directory below `shaders/includes/`, or the dependency glob misses it and editing it rebuilds nothing.
- `emitterLight` is zero on every instance but a light's emitter mesh; nothing may read it as a surface property.
