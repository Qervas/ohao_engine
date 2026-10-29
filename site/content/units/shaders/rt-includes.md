---
module: shaders
id: rt-includes
title: RT shader includes
standard: v2
figures: [shaders-rt-includes-hitdist-smc]
---

## What a ray payload can afford to carry

Every shader in a ray tracing pipeline must declare the same `rayPayloadEXT`
struct byte-for-byte, so a field added for one stage is paid for by all of them.
The realtime path tracer's payload is five `vec3`s plus two floats and a `uint`, and
three of those vec3s carry material data: `color` (emissive),
`hitAlbedo`, and `attenuation`. Only the last is packed, and `pbr_unpack.glsl`
is the contract for reading it back. The closest-hit shader writes roughness,
metallic and a curvature proxy into its three floats:

{{cite shaders/rt/pt_closesthit.rchit "payload.attenuation = vec3(roughness, clamp(metallic, 0.0, 1.0), curv);"}}

The proxy is the summed pairwise divergence of the hit triangle's three vertex
normals, so it is constant across a triangle — the code's own comment calls it
per-pixel. The decode of the other two floats looks like it ought to be two
clamps. It is not: it also has to accept two obsolete encodings of the same
float.

## Two encodings no code produces

`unpackHitPbr` accepts two encodings of `.x` besides plain roughness. A *negative*
value means "metal", and ±10 on top is a *shape* flag — which can work only because
roughness never reaches 1, so a magnitude of 10 or more can only be the flag. The sign
rule is gated on `.y` being near zero, the signature of a producer that never filled
the metallic channel:

{{cite shaders/rt/includes/pbr_unpack.glsl "if (att.x < 0.0 && att.y < 1e-4) {"}}

Nothing in the tree writes either encoding. `payload.attenuation` is written in
exactly one place, from the per-material table at binding 10, and roughness is
clamped to at least 0.04 before the write, so `.x` is always positive and never
near 10:

{{cite shaders/rt/pt_closesthit.rchit "roughness = max(roughness, 0.04);"}}

The instance table carries no material word either. Its tenth row, `emitterLight`, is
zero on every instance except a light's emitter mesh, where it holds the mesh's
radiance (`.rgb`) and the light's index in the RT light list (`.w`):

{{cite shaders/includes/scene/scene_data.glsl "vec4 emitterLight;   // an emitter mesh's radiance (rgb) and light index (w)"}}

The reference path tracer reads `.w` on such an instance to weigh a bounce ray's hit
against light sampling:

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}

So the decoder's two extra branches are dead code that still compiles into every
realtime raygen; they are worth understanding before touching the helper, not because
anything can reach them.

## The decode that bypasses the helper

`unpackHitPbr` has four call sites, all in `pt_raygen_realtime.rgen`: the first hit
once, the roughness AOV once, and a secondary bounce twice — once in the specular
walk, once in the diffuse walk. The reference path tracer, `path_tracer.slang`, has
no packed payload: its closest hit hands the raygen every material input as its own
field, so it neither includes the helper nor needs it.

{{cite shaders/rt/pt_raygen_realtime.rgen "unpackHitPbr(payload.attenuation, _aovRough, _aovMetal);"}}

One further decode open-codes the old rules instead, and it is consequential: the
realtime profile's ReSTIR-GI secondary-vertex decode reimplements the *sign* rule
and drops the `.y` guard entirely:

{{cite shaders/rt/pt_raygen_realtime.rgen "bool  sIsMetal  = (sPacked < 0.0);"}}

Since the closest-hit never writes a negative roughness, `sIsMetal` is always
false, and it gates two things. The reservoir's secondary vertex gets a
dielectric F0 whatever the artist painted:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3  sF0 = sIsMetal ? sAlbedo : vec3(0.04);"}}

It is also the metal test on the diffuse weight — `kD = (1.0 - F) * (sIsMetal ?
0.0 : 1.0)`, written identically in that vertex's NEE branch and its env-IS
branch — so a metal there also keeps a full Lambertian lobe that should be zero.
Both halves err the same way: untinted specular, plus diffuse that should not
exist.

ReSTIR-GI is the default there (the block is the `else` of `if (restirGiLegacy)`
and the `PT_FLAG_RESTIRGI_*` bits are all opt-outs), so this is live: one bounce
into the realtime GI path, metals shade like plastic. Routing the block through
`unpackHitPbr` is the fix, because that function reads `.y`, which is where the
metallic actually lives. The dependency runs one way only: the block never calls
`unpackHitPbr`, so deleting the legacy branches inside the function would
neither break it nor repair it.

## Two identical files, one of which never compiles

`shaders/rt/includes/pbr_unpack.glsl` and `shaders/includes/pbr_unpack.glsl` are
byte-identical, and only the first is ever compiled. The realtime raygen writes
`#include "includes/pbr_unpack.glsl"`; glslc resolves a quoted include relative
to the including file before consulting `-I`, so from `shaders/rt/` the sibling
`includes/` wins. The root copy is reachable only via the `-I <shaders>` search
path — that is, only if the `rt/` copy were deleted:

{{cite shaders/CMakeLists.txt "COMMAND ${GLSLC} --target-env=vulkan1.3 -I${CMAKE_CURRENT_SOURCE_DIR}"}}

{{cite shaders/rt/pt_raygen_realtime.rgen "#include "includes/pbr_unpack.glsl""}}

The same raygen reaches `shaders/includes/` for its other headers —
`rt/mis.glsl`, `rt/env_sampling.glsl`, `rt/sampler_api.glsl`,
`rt/nrd_frontend.glsl`, `material/ggx_aniso.glsl` and `rt/pt_lights.glsl` directly,
plus the two sampler files `sampler_api.glsl` pulls in and the generated
`shared/layout.glsl` that `pt_lights.glsl` does — all resolved through the
`-I <shaders>` root, because `shaders/rt/includes/` has no `rt/` or `material/`
subdirectory to shadow them. `pbr_unpack.glsl` is the one name that exists in both
places, and the sibling copy takes it every time. Editing the root copy changes no
binary and warns about nothing.

## The build does not track either copy

The list that decides when a shader is recompiled is globbed from one directory:

{{cite shaders/CMakeLists.txt "includes/**/*.glsl"}}

That glob roots at `shaders/includes/` and matches only files one level deeper:
evaluated standalone it yields thirty `.glsl` files, all in a
`shaders/includes/<subdir>/`. Neither `shaders/rt/includes/*.glsl` nor the two
`.glsl` files directly in `shaders/includes/` appear, so they are absent from the
`DEPENDS` list of every compile command. Editing `pbr_unpack.glsl` alone rebuilds
nothing.

:::key
Both hazards here are silent. Edit the wrong copy and the compiler never sees
the change; edit the right copy and the build system never notices. Touch a
`.rgen` — or clear `build/shaders/` — after changing either file.
:::

## Normalising a hit distance so REBLUR can decode it

`nrd_frontend.glsl` is the front-end for NVIDIA's REBLUR denoiser. Its
`IN_DIFF_RADIANCE_HITDIST` and `IN_SPEC_RADIANCE_HITDIST` inputs are single RGBA
images: radiance in `.rgb`, and in `.a` a hit distance squeezed into [0,1].
REBLUR re-expands that alpha with its own normalisation, so the writer's
function and the denoiser's must be the *same* function or the decoded distance
is wrong. The normalisation is

$$\hat{d} \;=\; \mathrm{saturate}\!\left(\frac{d}{f}\right), \qquad f \;=\; \bigl(A + |z_{\text{view}}|\,B\bigr)\cdot\bigl(C + (1-C)\,\mathrm{smc}(r)\bigr)$$

where $d$ is the traced hit distance in world units, $z_{\text{view}}$ the
view-space depth of the primary hit, $r$ perceptual roughness, and
$\mathrm{smc}$ a lobe-spread term running from 0 at a mirror to 1 at a fully
rough surface. The $A + |z|B$ factor grows the scale with distance, so a
reflection seen from far away can still be tens of metres long without
saturating; the second factor stretches it by up to $C$ for sharp lobes, whose
hit distances are longest. OHAO ports this verbatim at $A=3$, $B=0.1$, $C=20$:

{{cite shaders/includes/rt/nrd_frontend.glsl "float f = (NRD_HIT_DIST_PARAMS.x + abs(viewZ) * NRD_HIT_DIST_PARAMS.y)"}}

Everything except $\mathrm{smc}$.

## Where the port diverges, and by how much

NRD v4.17 computes $\mathrm{smc}(r) = \bigl(1 - 2^{-200r^2}\bigr)\sqrt{r}$ — the
`_NRD_GetSpecMagicCurve` helper in `NRD.hlsli`, which CMake fetches rather than
vendors in-tree. OHAO substitutes plain $r^2$, and says so in the comment:

{{cite shaders/includes/rt/nrd_frontend.glsl "float smc = roughness * roughness; // lobe-spread proxy"}}

The two agree at $r = 0$ and $r = 1$, and cross once more at $r \approx 5 \times
10^{-5}$, where both sit below $10^{-8}$ and nothing depends on which is larger.
Above that crossing the proxy is much smaller all the way to $r = 1$, so the
second factor stays nearer $C$, $f$ comes out too large, and the
normalised distance in `.a` too small. Evaluating both closed forms at the
shipping constants, the overshoot in $f$ peaks at 2.63x near $r = 0.76$.

{{figure shaders-rt-includes-hitdist-smc "The lobe-spread term smc(r): NRD v4.17's magic curve against the r-squared proxy in nrd_frontend.glsl. Both curves are evaluated from the closed forms, not measured from a render; the annotated 2.63x is the resulting peak overshoot in the normalisation factor f at A=3, B=0.1, C=20."}}

Because REBLUR de-normalises with its own curve, it reconstructs a hit distance
up to ~2.6x shorter than the raygen traced across the mid-to-rough range. Hit
distance sizes REBLUR's specular reprojection and blur footprints, so the error
is systematic, not noise. Its perceptual cost is not measured here.

:::why
The A/B/C triple is a `const vec3` compiled into the shader, while the C++ side
exposes only A: `NrdReblurProfile` carries a `hitDistanceParamA` field and no B
or C, and `setReblurSettings` starts from `nrd::ReblurSettings`'s defaults and
overrides that one member. Both sides sit at A = 3.0 today, but nothing enforces
it: turning the `hitDistanceParamA` knob in C++ silently desynchronises the
front-end from the denoiser consuming it, and the GLSL constant has no way to
follow. Whether the alternative — uploading the triple alongside the other
per-frame raygen parameters — was ever weighed is not recorded anywhere in the
tree.
:::

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "s.hitDistanceParameters.A         = p.hitDistanceParamA;"}}

## The YCoCg round trip, and a clamp that earns its place

REBLUR takes radiance in YCoCg and returns it the same way, so
`nrd_frontend.glsl` carries both directions of NRD's transform
(Y = ¼R + ½G + ¼B, Co = ½R − ½B, Cg = −¼R + ½G − ¼B) — an exact algebraic
inverse, so an unfiltered round trip is lossless. The compositor unpacks before
remodulating by the demodulated albedo and F0 AOVs:

{{cite shaders/rt/nrd_compose.comp "vec3 diffRad    = nrdYCoCgToLinear(imageLoad(inDiffRad,    p).rgb);"}}

The clamp at the end of the inverse is not cosmetic:

{{cite shaders/includes/rt/nrd_frontend.glsl "return max(r, vec3(0.0));"}}

What comes back from REBLUR is not a round trip but a *filtered* YCoCg signal,
and a luma/chroma pair blended from neighbouring pixels can invert to a negative
channel. Without the clamp those negatives survive remodulation and land in the
HDR buffer.

`nrd_frontend.glsl` is included by `nrd_compose.comp` and by the realtime raygen,
and by nothing else:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec4 diffPacked = nrdPackRadianceHitDist(diffDemod,   diffHitDist, firstHitViewZ, firstHitRoughness);"}}

The reference path tracer is Slang and cannot include GLSL; it carries its own
copies of the two packing functions, with the same A, B and C written as literals:

{{cite shaders/rt/path_tracer.slang "float4 nrdPackRadianceHitDist(float3 radiance, float hitDist, float viewZ, float roughness)"}}

## The ray masks are not a header here

The masks a trace passes are generated constants, one source for C++, GLSL and Slang:
`shaders/shared/layout.json` names the instance masks and the ray masks, and
`tools/gen_layout.py` writes them into `shared/layout.glsl` and its Slang twin. The
realtime raygen reaches the GLSL file through `includes/rt/pt_lights.glsl`; the
reference imports the Slang module:

{{cite shaders/includes/rt/pt_lights.glsl "shared/layout.glsl"}}
{{cite shaders/rt/path_tracer.slang "import shared.layout;"}}

In the realtime raygen every trace passes `kRayMaskScene` except the camera ray and a
near-mirror first hit's specular ray, which pass `kRayMaskCamera` and are the only
rays that see a light's emitter mesh. In the reference the camera ray passes
`kRayMaskCamera`, shadow rays `kRayMaskScene`, and bounce rays `kRayMaskScene` plus
the emitter mask, so a bounce that meets a light's mesh is weighed against light
sampling (hybrid/visibility has the scheme).

## Contracts

- `pt_closesthit.rchit` must write `attenuation` as (roughness, metallic, curvature) in that order. `pt_miss.rmiss` declares the same payload and writes `hitDist = -1.0`, `color` and `envPdf`, but never `attenuation` — so `unpackHitPbr` is valid only inside the `payload.hitDist >= 0` branch.
- Edit `shaders/rt/includes/pbr_unpack.glsl`; the `shaders/includes/` copy is shadowed by include-resolution order and never compiled. Neither file is in the CMake `DEPENDS` list, so force a shader rebuild after touching it.
- `NRD_HIT_DIST_PARAMS` in GLSL, the literals in `path_tracer.slang`'s `nrdPackRadianceHitDist` and `nrd::ReblurSettings::hitDistanceParameters` in C++ must hold the same A, B, C. Only A is exposed through `NrdReblurProfile`; B and C must therefore stay at NRD's defaults of 0.1 and 20.
- The legacy branches in `unpackHitPbr` are unreachable on the shipping payload, and removing them changes nothing at any of its four call sites. What must not be skipped is the reverse: the ReSTIR-GI block in `pt_raygen_realtime.rgen` open-codes the sign rule and does *not* call `unpackHitPbr`, so deleting the branches will not repair it. Route that block through the function first.
