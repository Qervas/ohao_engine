---
module: shaders
id: rt-shaders
title: RT program family
standard: v2
---

## One raygen, one miss, one hit group

Both render profiles build their RT pipeline from the same function,
`PathTracer::createRTPipeline`, and it is three shader groups either way: one
general group for the raygen, one for the miss shader, one triangles hit group
holding closest-hit plus — nominally — any-hit.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "groups[2].anyHitShader = hasAnyHit ? 3 : VK_SHADER_UNUSED_KHR;"}}

Every trace passes SBT offset 0, stride 0, miss index 0, so there is no per-material
hit record and the table is three handles wide. Bouncing is a loop in the raygen;
closest-hit and miss never trace.

The four stages are one Slang file, `rt/path_tracer.slang`, whose four entry points
compile to four `.spv` files. `PathTracerShaderSet` names the files and their entry
points, and its default is that file:

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* closestHitEntry{"closestHit"};"}}

Both profiles take the default; what differs between them is settings (bounces, the
denoiser, whether the trace keeps a running mean), not shaders.
`RTProfileRendererBase::init` hands the set to the tracer before `PathTracer::init`
runs, and the pipeline takes each stage's entry name from it:

{{cite ohao/render/rt/rt_profile_renderer.hpp ": RTProfileRendererBase(kRealtimeRTSettings, PathTracerShaderSet{}) {}   // rt/path_tracer.slang"}}
{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}
{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pName = m_shaderSet.raygenEntry;"}}

The raygen reads the bindless textures (binding 12, for the environment it samples
itself) and OpenPBR's energy tables at binding 38:

{{cite shaders/rt/path_tracer.slang "[[vk::binding(38, 0)]] StructuredBuffer<float> openpbrTables;"}}

## The payload is the calling convention

A raygen drives everything, so the single payload is the only channel between
programs. `PathPayload` carries a hit's place, its geometric and shading normals and
its resolved OpenPBR inputs, or a light's index, or nothing. The miss shader only
clears the hit flag; the raygen looks the environment up itself, and with no
environment bound it returns black rather than a constant sky:

{{cite shaders/rt/path_tracer.slang "if (tex >= 4096u) return float3(0.0);"}}

Visibility has no shadow payload and no shadow miss shader. The raygen sets the hit
flag before the trace, traces with accept-first-hit plus skip-closest-hit, and asks
whether the miss shader ran — it alone clears the flag:

{{cite shaders/rt/path_tracer.slang "p.flags = kHit;   // the closest hit is skipped: only the miss shader clears it"}}
{{cite shaders/rt/path_tracer.slang "payload.flags = 0u;"}}

## An any-hit shader that runs only when a material asks

The any-hit is an alpha test — read the material's alpha row, interpolate the UV,
multiply the base colour texture's alpha in, and drop the intersection below the
cutoff (alpha-tested materials) or by a stochastic test against the alpha (blended
ones).

{{cite shaders/rt/path_tracer.slang "if (alpha < row3.y) IgnoreHit();"}}

Most scenes never run it. Both BLAS builders in `rt_acceleration_structure.cpp` stamp
`VK_GEOMETRY_OPAQUE_BIT_KHR` on the geometry, and the raygen traces with the opaque
ray flag, which suppresses any-hit, unless the scene's light header says an instance
has an alpha material:

{{cite shaders/rt/path_tracer.slang "uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }"}}

Both trace sites go through it (`trace` and `visible`). `RenderScene` sets the TLAS
instance of an alpha-tested or blended material to
`VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR`, which overrides the BLAS bit for that
instance only, and an instance whose materials are all blended gets its own mask that
camera rays alone see. A scene with no alpha material traces every ray opaque. The
hybrid shadow trace has no hit shaders at all: it is a compute pass of inline ray
queries that ends on the first hit, and for a non-opaque candidate it asks
`rt/alpha_test.slang` whether the triangle counts before committing it.

{{cite shaders/rt/shadow_trace.slang "RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;"}}

Its visibility is the query's committed status — lit when nothing was committed:

{{cite shaders/rt/shadow_trace.slang "return rand01(pixelKey, l, s, 2u) < T ? 1.0 : 0.0;"}}

:::key
Occlusion is signalled by the *absence* of a hit — the path tracer's miss shader
running, the shadow query committing nothing — not by any hit shader. An alpha cutout
works by making a hit disappear (`IgnoreHit`, or a candidate the query never
commits), which is why it needs the ray to be non-opaque in the first place.
:::

## What the closest hit decodes, and the 24-bit fuse

The closest hit resolves a triangle through the instance table. The TLAS instance's
custom index is the `RenderScene` slot, and the shader uses it to fetch the instance,
its mesh, and the mesh-local indices into the shared vertex and index arenas:

{{cite shaders/rt/path_tracer.slang "asuint(instances[slot * stride + 8u])"}}

The host stages that field when it builds each TLAS instance:

{{cite ohao/render/scene/render_scene.cpp "vi.instanceCustomIndex = slot & 0xFFFFFFu;   // 24 bits"}}

The field is 24 bits wide in `VkAccelerationStructureInstanceKHR`, so the scheme wraps
past 16,777,216 instance slots — masked rather than checked, and the symptom would be a
hit reading another instance's mesh and material. The fuse is on instances, not
triangles, which keeps it far out of reach of any scene the engine builds today.

The closest hit hands the raygen every OpenPBR input already resolved: base colour
times its texture, roughness and metalness times the packed map, specular weight and
colour times theirs, emission colour times luminance times the emissive texture. A
normal map is read in the vertex tangents' frame, as the G-buffer reads it:

{{cite shaders/rt/path_tracer.slang "// A normal map in the vertex tangents' frame, as the G-buffer reads it."}}

A hit on a light's emitter mesh returns no material: it flags the payload and carries
the light's index, which the host stored in the instance's tenth row:

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}

## One buffer, one declaration

Binding 11 is a four-word header — light count, environment map index, environment
intensity, the alpha-rays flag — followed by the lights. The raygen reads it as rows of
four floats, row 0 the header and five rows a light after it, and decodes each light
by hand:

{{cite shaders/rt/path_tracer.slang "[[vk::binding(11, 0)]] StructuredBuffer<float4> lightData;    // header, then 5 rows a light"}}

{{cite shaders/rt/path_tracer.slang "bool alphaRays() { return asuint(lightData[0].w) == 1u; }"}}

The C++ side fills the same layout, with the lights at byte 16:

{{cite ohao/render/scene/light_table.hpp "static constexpr VkDeviceSize kHeaderBytes = 16;"}}

Nothing generates either side: the header's four words and a light's five rows are a
hand-maintained agreement between `light_table.cpp`, `GPULight` and `loadLight`.

## One path, split for a denoiser

The raygen traces one path a sample. The OpenPBR base samples the next direction and
reports the lobe it drew from; the first bounce's lobe decides which channel —
diffuse or specular — everything after the primary hit is attributed to, so the
beauty stays one estimate and the denoiser still gets two signals:

{{cite shaders/rt/path_tracer.slang "if (depth == 0u) firstLobe = s.lobe;"}}

Direct light at the primary hit goes to the diffuse channel whatever the surface. Hit
distances are one segment: the second vertex's, recorded on the channel the first
bounce's lobe chose.

## The packing that keeps a sign bit alive

Binding 26 is `R10G10B10A2_UNORM`, NRD's `IN_NORMAL_ROUGHNESS` layout, and the raygen
ports NRD's encoder verbatim: rotated octahedral normal in x and y, roughness in z,
and the normal's z-sign smuggled into the *sign of the roughness*. Zero roughness has
no sign, so the encoder floors it at 1.5/512 ≈ 0.0029 first:

{{cite shaders/rt/path_tracer.slang "roughness = max(roughness, 1.5 / 512.0);"}}

In this engine that floor can never bind. The raygen clamps its first hit's roughness
at 0.01 before packing, more than three times the floor: the line is correct, but not
load-bearing.

{{cite shaders/rt/path_tracer.slang "firstRough = clamp(hit.specularRoughness, 0.01, 1.0);"}}

The remaining two bits carry NRD's material ID, left at slot 0.

## Where the demodulation round trip closes

`nrd_compose.comp` is thirty-five lines and one line of arithmetic, applied after
unpacking REBLUR's YCoCg output back to linear:

{{cite shaders/rt/nrd_compose.comp "vec3 composed = diffRad * diffAlbedo + specRad * specColor;"}}

That multiply is the second half of a round trip whose first half — dividing radiance
by the diffuse albedo AOV before packing — lives in the raygen:

{{cite shaders/rt/path_tracer.slang "const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));"}}

Only the diffuse channel is divided. Specular is packed raw, and its factor is the
base's directional albedo toward the viewer less the diffuse part, while the specular
radiance already carries the BSDF's Fresnel; the composite therefore applies the
specular albedo twice (see the NRD unit).

{{cite shaders/rt/path_tracer.slang "specColor = max(a - diffAlbedo, float3(0.0));"}}

When the trace keeps a running mean of its AOVs (offline NRD), the radiance and guide
AOVs are folded into a `1/(n+1)` running mean, so an N-spp render hands the denoiser
its average rather than its last sample:

{{cite shaders/rt/path_tracer.slang "diffuseRadiance[pixel] = lerp(diffuseRadiance[pixel], nrdPackRadianceHitDist(diffDemod,"}}

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
light's emitter mesh or a first-person gun (camera rays only, `kRayMaskCamera`)
nor a hidden instance (mask 0). Whatever triangle the GI trace's query commits, it
resolves that instance's material unconditionally; the mask is why an emitter's instance,
which carries no surface material (its `emitterLight` holds its radiance and light index), is never read as a surface.

{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceEmitter) != 0   ? gpu::shared::kTlasMaskEmitter"}}
{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceViewModel) != 0 ? gpu::shared::kTlasMaskViewModel"}}
{{cite shaders/rt/gi_trace.slang "const HitMaterial hm = hitMaterial(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),"}}

## The rest of shaders/rt

Fourteen files sit in `shaders/rt/`: the path tracer, the hybrid pairs above, two Slang
modules they import (`alpha_test.slang`, `smoke_proxy.slang`), NRD's compose pass, and
two denoising families run after the trace. Four `cinematic_*.comp` passes run after
NRD compose — bright-pass extract, a 13-tap downsampling blur that builds the bloom
mip chain, a gather-based DoF, and the composite that applies Khronos PBR Neutral and
writes the final LDR image — each built and dispatched through `nrd_cinematic.cpp`.

`DenoiseMode::Atrous` is the variance-guided pair `rt_svgf_temporal.comp` and
`rt_svgf_atrous.comp`, the two pipelines `AtrousDenoiser` builds. Its 5×5 B3-spline
tap weight multiplies edge-stopping terms on luminance, normal and depth. The luminance
term is the one doing the averaging, and its width scales with the local variance, so
a converged pixel stays sharp; the normal and depth guides only stop the kernel
crossing geometric edges.

{{cite shaders/rt/rt_svgf_atrous.comp "w *= exp(-abs(cL - sL) / (sqrtVar * pc.sigmaL + 1e-6));"}}

{{cite ohao/render/rt/denoise/atrous_denoise.cpp "rt_rt_svgf_atrous.comp.spv"}}

## Contracts

- Binding 11's header sits at fixed byte offsets 0 / 4 / 8 / 12 with the lights at 16, read by `path_tracer.slang` as raw `float4` rows (five a light) and written in one C++ file. Change it in one place and the shader reads a light's position as an env-map index.
- `instanceCustomIndex` is the `RenderScene` slot, masked to 24 bits; past 2²⁴ slots a hit reads another instance.
- Alpha-tested and blended geometry reaches a ray only while the scene has an instance with such a material: `traceFlags()` drops the opaque flag only then, and the TLAS instance carries `FORCE_NO_OPAQUE` over the BLAS's `VK_GEOMETRY_OPAQUE_BIT_KHR`. The hybrid ray queries decide through `rt/alpha_test.slang`.
- A profile's shader set names files *and* entry points: a Slang stage is entered by its function name. A set that names a Slang file with `main` fails pipeline creation.
- Bindings 24/25 are demodulation factors matched to what the raygen already did — diffuse pre-divided, specular not. Change one side without the other and the NRD composite's colours move.
