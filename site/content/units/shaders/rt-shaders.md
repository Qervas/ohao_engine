---
module: shaders
id: rt-shaders
title: RT program family
standard: v2
figures: [shaders-rt-shaders-dualray]
---

## One raygen, one miss, one hit group

Both render profiles build their RT pipeline from the same function,
`PathTracer::createRTPipeline`, and it is three shader groups either way: one
general group for the raygen, one for the miss shader, one triangles hit group
holding closest-hit plus — nominally — any-hit.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "groups[2].anyHitShader = hasAnyHit ? 3 : VK_SHADER_UNUSED_KHR;"}}

Every trace in the family passes SBT offset 0, stride 0, miss index 0, so there is
no per-material hit record and the table is three handles wide. Bouncing is a loop
in the raygen; closest-hit and miss never trace.

The two profiles are two program families. `PathTracerShaderSet` names four SPIR-V
files and their entry points; its default is the reference path tracer, one Slang
file, `rt/path_tracer.slang`, whose four entry points compile to four `.spv` files:

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* closestHitEntry{"closestHit"};"}}

The offline profile takes that default. The realtime profile names the GLSL family
— `pt_raygen_realtime.rgen`, `pt_miss.rmiss`, `pt_closesthit.rchit`,
`pt_anyhit.rahit`, each entered at `main`. `RTProfileRendererBase::init` hands the
set to the tracer before `PathTracer::init` runs, and the pipeline takes each
stage's entry name from it:

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}
{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pName = m_shaderSet.raygenEntry;"}}

Both families bind to the same descriptor set layout. The reference reads two
things the GLSL family does not: the bindless textures from the raygen (binding 12,
for the environment it samples itself) and OpenPBR's energy tables at binding 38.

{{cite shaders/rt/path_tracer.slang "[[vk::binding(38, 0)]] StructuredBuffer<float> openpbrTables;"}}

## The payload is the calling convention

A raygen drives everything, so the single payload is the only channel between
programs. The two families use it differently.

The GLSL family's `RayPayload` is reused for material fetch, emissive transport,
environment radiance and shadow visibility. Visibility has no shadow payload and no
shadow miss shader: the raygen stores a positive sentinel in `payload.hitDist`,
traces with terminate-on-first-hit plus skip-closest-hit, then asks whether the
miss shader ran — it alone can write a negative distance.

{{cite shaders/rt/pt_miss.rmiss "payload.hitDist = -1.0;  // signal miss"}}

The same invocation fills `payload.color` with environment radiance, so the
realtime raygen's env-MIS gets occlusion and incident radiance from one ray instead
of two. The cost is global mutable state: anything still needed after a shadow
trace must be latched into a local first, or the failure is a quiet darkening, not
a crash.

:::why
With no environment map bound the GLSL miss shader returns black, not a cheap
constant sky ambient. The shader names the rejected alternative: in a closed room,
GI rays leaking through wall seams pick that fake sky up and carry spurious light
and grain back inside.
:::

{{cite shaders/rt/pt_miss.rmiss "payload.color = vec3(0.0);"}}

The reference's `PathPayload` carries a hit's place, its geometric and shading
normals and its resolved OpenPBR inputs, or a light's index, or nothing. Its miss
shader only clears the hit flag; the raygen looks the environment up itself.
Visibility uses the same convention as the GLSL family — a flag set before the
trace that only the miss shader clears:

{{cite shaders/rt/path_tracer.slang "p.flags = kHit;   // the closest hit is skipped: only the miss shader clears it"}}
{{cite shaders/rt/path_tracer.slang "payload.flags = 0u;"}}

## An any-hit shader that runs only when a material asks

Each family's any-hit is an alpha test — read the material's alpha row,
interpolate the UV, multiply the base colour texture's alpha in, and drop the
intersection below the cutoff (alpha-tested materials) or by a stochastic test
against the alpha (blended ones).

{{cite shaders/rt/pt_anyhit.rahit "if (alpha < alphaRow.y) ignoreIntersectionEXT;"}}
{{cite shaders/rt/path_tracer.slang "if (alpha < row3.y) IgnoreHit();"}}

Most scenes never run it. Both BLAS builders in `rt_acceleration_structure.cpp` stamp
`VK_GEOMETRY_OPAQUE_BIT_KHR` on the geometry, and the raygens trace with
the opaque ray flag, which suppresses any-hit, unless the scene's light header says
an instance has an alpha material. The GLSL raygen does it through a macro, the
Slang one through a function:

{{cite shaders/rt/pt_raygen_realtime.rgen "#define OHAO_RAY_OPAQUE (lightBuf.alphaRays == 1u ? gl_RayFlagsNoneEXT : gl_RayFlagsOpaqueEXT)"}}
{{cite shaders/rt/path_tracer.slang "uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }"}}

Every trace site goes through them — fourteen in the realtime raygen, two in the
reference (`trace` and `visible`). `RenderScene` sets the TLAS instance of an
alpha-tested or blended material to `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR`,
which overrides the BLAS bit for that instance only, and an instance whose materials
are all blended gets its own mask that camera rays alone see. A scene with no alpha
material traces exactly as before alpha modes existed. The hybrid shadow trace has no
hit shaders at all: it is a compute pass of inline ray queries that ends on the first
hit, and for a non-opaque candidate it asks `rt/alpha_test.slang` whether the triangle
counts before committing it.

{{cite shaders/rt/shadow_trace.slang "RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;"}}

Its visibility is the query's committed status — lit when nothing was committed:

{{cite shaders/rt/shadow_trace.slang "return rand01(pixelKey, l, s, 2u) < T ? 1.0 : 0.0;"}}

:::key
Occlusion is signalled by the *absence* of a hit — the path tracer's miss shader
running, the shadow query committing nothing — not by any hit shader. An alpha cutout
works by making a hit disappear (`IgnoreHit`, or a candidate the query never
commits), which is why it needs the ray to be non-opaque in the first place.
:::

## What the closest hits decode, and the 24-bit fuse

Both closest hits resolve a triangle through the instance table. The TLAS
instance's custom index is the `RenderScene` slot, and the shaders use it to fetch
the instance, its mesh, and the mesh-local indices into the shared vertex and index
arenas:

{{cite shaders/rt/pt_closesthit.rchit "InstanceGpu inst = instances[gl_InstanceCustomIndexEXT];"}}
{{cite shaders/rt/path_tracer.slang "asuint(instances[slot * stride + 8u])"}}

The host stages that field when it builds each TLAS instance:

{{cite ohao/render/scene/render_scene.cpp "vi.instanceCustomIndex = slot & 0xFFFFFFu;   // 24 bits"}}

The field is 24 bits wide in `VkAccelerationStructureInstanceKHR`, so the scheme wraps
past 16,777,216 instance slots — masked rather than checked, and the symptom would be a
hit reading another instance's mesh and material. The fuse is on instances, not
triangles, which keeps it far out of reach of any scene the engine builds today.

The reference's closest hit hands the raygen every OpenPBR input already resolved:
base colour times its texture, roughness and metalness times the packed map,
specular weight and colour times theirs, emission colour times luminance times the
emissive texture. A normal map is read in the vertex tangents' frame, as the
G-buffer reads it:

{{cite shaders/rt/path_tracer.slang "// A normal map in the vertex tangents' frame, as the G-buffer reads it."}}

A hit on a light's emitter mesh returns no material: it flags the payload and
carries the light's index, which the host stored in the instance's tenth row:

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}

The GLSL closest hit is older. Its normal mapping builds a Frisvad basis from the
shading normal alone, ignoring the vertex tangents:

{{cite shaders/rt/pt_closesthit.rchit "float a = 1.0 / (1.0 + worldNormal.z);"}}

That frame is unrelated to the UV parameterisation, so on the realtime profile
directional detail authored against the UV layout — brushed metal, mortar bevels,
stitching — is rotated by an arbitrary amount about the normal. Bump-like isotropic
detail survives.

Its payload's third slot is not what its name suggests: `attenuation.z` carries a
curvature proxy, the summed pairwise divergence of the triangle's three vertex
normals, meant for a subsurface term.

{{cite shaders/rt/pt_closesthit.rchit "payload.attenuation = vec3(roughness, clamp(metallic, 0.0, 1.0), curv);"}}

No raygen reads it: the realtime raygen has no subsurface term, so the closest hit
computes and ships a value nobody consumes.

The `.x` / `.y` half goes through `unpackHitPbr` in `includes/pbr_unpack.glsl`.
It carries two dormant legacy branches: a negative-`.x` "binary metal" path,
and a subtract-10 shift for magnitudes at or above 10. Current closest-hit writes
a positive roughness already floored at 0.04, so neither can fire.

{{cite shaders/includes/pbr_unpack.glsl "if (att.x < 0.0 && att.y < 1e-4) {"}}

The realtime raygen re-implements the same unpack inline for its secondary
reservoir vertex, dormant branch included, instead of calling the helper:

{{cite shaders/rt/pt_raygen_realtime.rgen "if (sRough >= 10.0) sRough -= 10.0;"}}

## One buffer, three declarations

Binding 11 is declared three times across `shaders/rt/`, in three shapes. The
realtime raygen sees a four-word header (light count, environment map index,
intensity, the alpha-rays flag) followed by the `GPULight` array; the GLSL miss
shader sees the same header with its last word as padding and no array:

{{cite shaders/rt/pt_miss.rmiss "uint envMapTexIdx;    // bindless index of HDR env map (0xFFFFFFFF = none)"}}

The reference reads it as rows of four floats — row 0 the header, five rows a light
after it — and decodes each light by hand:

{{cite shaders/rt/path_tracer.slang "[[vk::binding(11, 0)]] StructuredBuffer<float4> lightData;    // header, then 5 rows a light"}}

std430 puts the vec4-based array at offset 16 regardless, so the three spare header
words are a hand-maintained union across three shader declarations and the C++ file
that fills them, with nothing enforcing agreement.

{{cite ohao/render/scene/light_table.hpp "static constexpr VkDeviceSize kHeaderBytes = 16;"}}

## Two ways to split a path for a denoiser

From the primary hit `pt_raygen_realtime.rgen` does not stochastically choose a
lobe. It fires two independent chains — a GGX VNDF specular sample and a cosine
hemisphere diffuse sample — attributing everything each collects to `specContrib`
or `diffContrib`. The specular chain's bounce-0 weight is the VNDF estimator, the
Fresnel term times the height-correlated Smith ratio:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

The diffuse chain's bounce-0 throughput is exact: for a Lambertian lobe under
cosine sampling, f·cosθ/pdf collapses to `albedo`. Neither restores the lobe
weight of the other, so a dielectric launches both chains from the same hit, each
at its own lobe's weight. Past bounce 0 the two loops read as copies but are not. A
specular bounce inside the specular chain scales throughput by
`mix(1, albedo, metallic)`; the same event inside the diffuse chain scales by
`albedo * (1 - metallic)` — zero for a pure metal, which kills that chain outright.

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput *= mix(vec3(1.0), bAlbedo, bMetallic);"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "diffThroughput *= bAlbedo * (1.0 - bMetallic);"}}

Hit distances follow REBLUR's conventions: the specular one accumulates along the
chain until a diffuse bounce breaks it, the diffuse one latches the first secondary
hit only.

{{cite shaders/rt/pt_raygen_realtime.rgen "if (payload.hitDist >= 0.0 && specChainActive) {"}}

The reference traces one path a sample. The OpenPBR base samples the next
direction and reports the lobe it drew from; the first bounce's lobe decides which
channel everything after it is attributed to:

{{cite shaders/rt/path_tracer.slang "const BsdfSample s = base.sample(wo, float3(rng.next(), rng.next(), rng.next()), tables);"}}
{{cite shaders/rt/path_tracer.slang "if (depth == 0u) firstLobe = s.lobe;"}}

{{figure shaders-rt-shaders-dualray "Conceptual data flow of the realtime raygen's dual-ray split and the NRD round trip, drawn from the shader source — not a captured render and not a timing measurement. The dashed path is the remodulation performed by nrd_compose.comp."}}

## The packing that keeps a sign bit alive

Binding 26 is `R10G10B10A2_UNORM`, NRD's `IN_NORMAL_ROUGHNESS` layout, and both
raygens port NRD's encoder verbatim: rotated octahedral normal in x and y,
roughness in z, and the normal's z-sign smuggled into the *sign of the roughness*.
Zero roughness has no sign, so the encoder floors it at 1.5/512 ≈ 0.0029 first:

{{cite shaders/rt/pt_raygen_realtime.rgen "roughness = max(roughness, 1.5 / 512.0);"}}

In this engine that floor can never bind. The GLSL closest hit clamps roughness to
0.04 before writing the payload and `unpackHitPbr` clamps again at 0.01; the
reference clamps its first hit's roughness at 0.01 before packing. What reaches the
encoder is more than three times the floor: the line is correct, but not
load-bearing.

{{cite shaders/rt/pt_closesthit.rchit "roughness = max(roughness, 0.04);"}}
{{cite shaders/rt/path_tracer.slang "firstRough = clamp(hit.specularRoughness, 0.01, 1.0);"}}

The remaining two bits carry NRD's material ID, left at slot 0.

## Where the demodulation round trip closes

`nrd_compose.comp` is thirty-five lines and one line of arithmetic, applied after
unpacking REBLUR's YCoCg output back to linear:

{{cite shaders/rt/nrd_compose.comp "vec3 composed = diffRad * diffAlbedo + specRad * specColor;"}}

That multiply is the second half of a round trip whose first half — dividing
radiance by the diffuse albedo AOV before packing — lives in the raygens:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 diffDemod = diffOut / max(firstHitDiffAlbedo, vec3(0.03));"}}
{{cite shaders/rt/path_tracer.slang "const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));"}}

Only the diffuse channel is divided. Specular is packed raw. The realtime raygen's
specular factor is `mix(F0, 1, metallic)` — unity for a metal, F0 for a
dielectric — matched to a chain whose throughput carries a metal's albedo:

{{cite shaders/rt/pt_raygen_realtime.rgen "firstHitSpecColor  = mix(F0, vec3(1.0), metallic);"}}

The reference's is the base's specular albedo toward the viewer, while its specular
radiance already carries the BSDF's Fresnel; its NRD composite applies the specular
albedo twice (see the NRD unit).

The radiance AOVs are folded under the accumulate-AOVs flag into a `1/(n+1)`
running mean, so an N-spp render hands the denoiser its average rather than its
last sample — a macro in GLSL, a `lerp` in the reference:

{{cite shaders/rt/pt_raygen_realtime.rgen "_ohaoAovCur = mix(_ohaoAovPrev, _ohaoAovCur, 1.0 / _ohaoAovN);"}}

## The hybrid pair, and what it is not

`rt/shadow_trace.slang` (with its filter, `rt/shadow_filter.slang`) and
`rt/gi_trace.slang` (with its denoiser, `rt/gi_denoise.slang`) are a different
family — they read the deferred GBuffer instead of tracing primary rays. Both are on by default. The `{false}` member initializers in
`deferred_renderer.hpp` are only the pre-init value; `DeferredRenderer::init` flips
each flag to `true` as soon as the corresponding technique's own `init()` succeeds
(for both, only on a device with ray queries). The CSM fallback runs when RT
shadows do not trace a frame — initialisation failed, no TLAS, or no light casts
shadows — and the no-GI branch when RT GI is not available.

{{cite ohao/render/deferred/deferred_renderer.cpp "m_useRTShadows = true;"}}

Neither trace is a ray tracing pipeline at all: each is a compute shader with
inline ray queries, no shader binding table, and its lights arrive in a storage
buffer the C++ side writes per frame slot. The shadow trace ranks the frame's
shadow-casting lights (up to eight) per pixel by importance and traces one ray to
each of the four most important; the filter then turns those bits into one 8-bit
visibility channel a light (see the hybrid shadow technique). The GI trace shoots
cosine-weighted rays one bounce deep — two a pixel at half resolution in realtime,
32 at full resolution in reference — and lights each hit with every light,
attenuated as the lighting pass attenuates them, one of them, chosen in proportion
to what it gives (among eight candidates drawn by power past 16 lights), behind a
shadow ray from the secondary hit. It takes the hit surface's
normal as the negated ray direction: a one-bounce estimate, not a path tracer (see the
hybrid GI technique).

{{cite shaders/rt/gi_trace.slang "return (albedo * (1.0 / kPi) * directAt(at, -dir, key, s) + hm.emission) * T;"}}

The albedo is the hit material's OpenPBR directional albedo toward the ray, the material
resolved at the hit's uv by `hitMaterial` (`rt/alpha_test.slang`) and its energy tables at
binding 11; the hit's own emission is added unlit.

{{cite shaders/rt/gi_trace.slang "const float3 albedo = OpenPBRBase(hm.inputs).albedo(float3(0.0, 0.0, 1.0), tables);"}}

There is no miss shader in either. The query's committed status is the whole answer:
a shadow ray that commits nothing is lit, and a GI ray that commits nothing samples
the environment map in its direction, or returns black when none is bound.

{{cite shaders/rt/gi_trace.slang "if (pc.flags.y == 0u) return float3(0.0);"}}

Both draw their random numbers from an integer PCG hash, keyed by the render seed
and the frame index since the last temporal reset as well as the pixel and sample,
so their noise changes every frame and repeats exactly for the same seed and frame:

{{cite shaders/rt/shadow_trace.slang "return pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y))));"}}

Both trace with ray mask `kRayMaskScene`: every visible scene instance, never a
light's emitter mesh or a first-person gun (camera and mirror rays only, `kRayMaskCamera`)
nor a hidden instance (mask 0). Whatever triangle the GI trace's query commits, it
resolves that instance's material unconditionally; the mask is why an emitter's instance,
which carries no surface material (its `emitterLight` holds its radiance and light index), is never read as a surface.

{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceEmitter) != 0   ? gpu::shared::kTlasMaskEmitter"}}
{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceViewModel) != 0 ? gpu::shared::kTlasMaskViewModel"}}
{{cite shaders/rt/gi_trace.slang "const HitMaterial hm = hitMaterial(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),"}}

## The rest of shaders/rt, and which of it runs

Nineteen files sit in `shaders/rt/`. Past the two path tracer families and the hybrid passes above, four
`cinematic_*.comp` passes run after NRD compose — bright-pass extract, a 13-tap
downsampling blur that builds the bloom mip chain, a gather-based DoF, and the
composite that applies Khronos PBR Neutral and writes binding 32 — each built and dispatched
through `nrd_cinematic.cpp`.

Two à-trous kernels remain, and the plain one is the one that never runs.
`rt_atrous.comp` is a 5×5 B3-spline à-trous over the LDR beauty whose per-tap
weight multiplies three
edge-stopping terms in source order: colour similarity first, then normal, then
linear depth. Colour similarity is the term doing most of the work — it is what
averages away Monte Carlo noise; the normal and depth guides only stop the kernel
crossing geometric edges.

Nothing loaded it, and it has been deleted. `AtrousDenoiser` builds its two
pipelines from
`rt_svgf_temporal.comp` and `rt_svgf_atrous.comp` instead — the variance-guided
pair, which is what `DenoiseMode::Atrous` actually dispatches — and the
`RTDenoiser` class that would have driven the plain kernel was a header with no
translation unit, `ohao/render/rt/denoiser.hpp`, since deleted.

{{cite ohao/render/rt/denoise/atrous_denoise.cpp "rt_rt_svgf_atrous.comp.spv"}}

## Contracts

- In the GLSL family, latch any payload field you still need before tracing a shadow ray: the miss shader overwrites `color`, `hitDist` and `envPdf` on every ray, visibility rays included.
- Binding 11's header sits at fixed byte offsets 0 / 4 / 8 / 12 with `GPULight[]` at 16, declared in three shaders (the reference as raw `float4` rows, five a light) and written in one C++ file. Change it in one place and a shader reads a light's position as an env-map index.
- `instanceCustomIndex` is the `RenderScene` slot, masked to 24 bits; past 2²⁴ slots a hit reads another instance.
- Alpha-tested and blended geometry reaches a ray only while the scene has an instance with such a material: `OHAO_RAY_OPAQUE` and `traceFlags()` drop the opaque flag only then, and the TLAS instance carries `FORCE_NO_OPAQUE` over the BLAS's `VK_GEOMETRY_OPAQUE_BIT_KHR`. The hybrid ray queries decide through `rt/alpha_test.slang`.
- A profile's shader set names files *and* entry points: a GLSL stage is `main`, a Slang stage its function name. A set that names a Slang file with `main` fails pipeline creation.
- Bindings 24/25 are demodulation factors matched to what the raygen already did — diffuse pre-divided, specular not. In the realtime raygen, demodulate the specular channel without changing `firstHitSpecColor`, or vice versa, and metals double-darken while dielectrics lose their Fresnel.
