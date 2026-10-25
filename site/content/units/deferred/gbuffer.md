---
module: deferred
id: gbuffer
title: GBuffer pass
standard: v2
figures: [deferred-gbuffer-channels]
---

## What the record has to hold

A deferred renderer bets that everything the lighting equation needs about a
visible surface fits into a fixed-size per-pixel record, so shading cost stops
scaling with geometry × lights. The interesting part is never the bet; it is the
record — what fits, what was dropped to make it fit, and which downstream pass
finds out.

OHAO writes five attachments, in two render passes over one framebuffer (below): world position + metallic, an
encoded normal + roughness + emissive luminance (or subsurface strength), albedo + AO, a two-component
velocity vector, and depth. That is 28 bytes per pixel, roughly 55 MiB resident
at the 1920×1080 default the pass initialises to.

{{figure deferred-gbuffer-channels "Conceptual layout, not a captured render. Cell widths are proportional to bytes per pixel; formats and channel assignments are read from createGBuffer() and gbuffer.frag."}}

Check one thing before trusting anything else written about this buffer: the
normal target is `R16G16B16A16_SFLOAT`, not a packed 10-bit format.

{{cite ohao/render/deferred/gbuffer_pass.cpp "VK_FORMAT_R16G16B16A16_SFLOAT,   // octahedral normal + roughness + emissive / -subsurface"}}

Comments in five files still name a packed 10-bit format for this attachment,
including the header of the shared octahedral decoder:

{{cite shaders/includes/common/encoding.glsl "Octahedron normal encoding/decoding (A2R10G10B10 GBuffer format)"}}

Three of those five, and four of the nine shaders that `#include` that header,
sit under `shaders/_disabled/` — a directory nothing actually excludes, because
the shader target globs the tree recursively and compiles it too:

{{cite shaders/CMakeLists.txt "file(GLOB_RECURSE SHADER_SOURCES"}}

The octahedral encoding is a leftover from the 10-bit format. It is not
vestigial — it is now paying for something else.

## The packing that buys a channel

Octahedral mapping projects a unit normal onto an octahedron and folds the lower
hemisphere outward, turning any direction on the sphere into two numbers in
[-1, 1] with no seam and near-uniform angular density. The G-buffer writes it
biased into [0, 1] alongside two unrelated scalars:

{{cite shaders/core/gbuffer.frag "outGBuffer1 = vec4(encodedNormal, roughness, subsurface > 0.0 ? -subsurface : emissiveLuminance);"}}

At 16 bits per channel the encoding buys almost no precision over storing
`N.xyz` directly — half-float already resolves a normal component far below the
visible threshold. What it buys is one *channel*. The normal costs two slots
instead of three; roughness would have fitted in the fourth slot of an `N.xyz`
layout anyway, so the one slot the encoding actually liberates is the last, and
it carries the emissive intensity that would otherwise need its own attachment.

That slot is shared by sign. Emissive luminance is never negative, so a material
with a `subsurface` strength (row 2, z of the material table) stores *minus* that
strength there instead, and an emissive surface takes no subsurface at all:

{{cite shaders/core/gbuffer.frag "const float subsurface = emissiveLuminance > 0.0 ? 0.0 : row2.z;"}}

Lighting reads the slot through `max(w, 0)`, so a subsurface pixel emits nothing;
the SSS blur reads `max(-w, 0)` as the pixel's blur strength.

{{cite shaders/core/deferred_lighting.frag "float emissiveLuminance = max(gBuffer1Sample.a, 0.0);"}}

The price is colour. Only luminance survives the write, so deferred lighting
reconstructs the emitted colour by tinting albedo with it:

{{cite shaders/core/deferred_lighting.frag "vec3 emissive = albedo * emissiveLuminance;"}}

A red glow on a white surface renders white. That is a deliberate trade, but it
means emissive look-dev cannot be trusted against the raster viewport.

Roughness is floored before storage:

{{cite shaders/core/gbuffer.frag "roughness = max(roughness, 0.04);"}}

`initBRDFSurface` applies the same floor again, but only to the copy it owns:

{{cite shaders/includes/brdf/brdf_common.glsl "surface.roughness = max(roughness, 0.04);"}}

Deferred lighting keeps the raw local value alongside that surface and uses it
for everything image-based — the prefiltered environment's level, the
roughness-aware Fresnel term, and the split-sum fit all read the unclamped local, not
`surface.roughness`:

{{cite shaders/core/deferred_lighting.frag "textureLod(envPrefiltered, equirectUV(R), roughness * float(kEnvPrefilterLevels - 1u)).rgb;"}}

For those uses the G-buffer clamp is the only floor there is, so it is not
redundant. It is also not sufficient, because the weather modifiers run before
the surface is built and can push the local value back under it — wet
horizontal faces remap roughness toward `max(roughness * 0.15, 0.02)`, which
bottoms out at 0.02:

{{cite shaders/core/deferred_lighting.frag "roughness  = mix(roughness, max(roughness * 0.15, 0.02), w);"}}

The other raw reader, screen-space reflections, is insensitive to the floor in
either direction. It uses the channel exactly twice: a `roughness > 0.7`
rejection branch, which a 0.04 floor can never flip, and a `1.0 - roughness` hit
fade, which it can move by at most 4%.

{{cite shaders/postprocess/ssr.slang "if (roughness > 0.7 && metallic < 0.5)"}}

## World position, and the fp16 tax on it

GBuffer0 stores the interpolated world position outright rather than making
consumers reconstruct it from depth:

{{cite shaders/core/gbuffer.frag "outGBuffer0 = vec4(fragWorldPos, metallic);"}}

:::why
Reconstruction from `D32_SFLOAT` plus an inverse view-projection is the standard
alternative, and it is cheaper: 8 bytes per pixel saved, full float precision
kept. OHAO rejects it because seven consumers read this target directly —
deferred lighting, SSAO, SSR, the subsurface-scattering blur, the RT shadow trace,
the RT GI trace and RT GI's upsample — and two of them launch rays from the value.
Storing it once removes a class of per-consumer reconstruction bugs. Depth is still
read where only a depth comparison is wanted: SSR's hit test and the tonemap's SSR
upsample.
:::

The precision cost is worth quantifying, because it is not uniform. A binary16
value carries a 10-bit stored mantissa, so the gap between adjacent
representable numbers near a magnitude $|x|$ is

$$\Delta(x) = 2^{\lfloor \log_2 |x| \rfloor - 10}$$

where $\lfloor \log_2 |x| \rfloor$ is the binary exponent. Near the origin this
is invisible. For a coordinate in [64, 128) the exponent is 6, giving $\Delta =
2^{-4} = 0.0625$; at 1000 units, $\Delta = 0.5$. Round-to-nearest leaves the
stored coordinate within half a gap of the true one, so the error to compare
against anything is $\Delta/2$, not $\Delta$. Compare it to the fixed offset the
RT shadow trace applies to the position it reads back out of this buffer:

{{cite shaders/rt/shadow_trace.slang "const float3 origin = p + n * 0.05;"}}

The bias is 0.05. Half a gap first exceeds it in [128, 256), where $\Delta =
0.125$ and the worst-case per-component error is 0.0625 — past roughly 128 units
from the origin, quantisation can displace the ray origin further than the bias
was meant to lift it, and the ray can start on the wrong side of its own surface.
Scenes authored far from the origin develop shadow acne that will not reproduce
near it — the buffer format, not the tracer, is the cause.

## The clear colour is the coverage bit

There is no coverage channel and no stencil. Consumers decide "was anything
rasterised here" by testing GBuffer0 against its clear value of `(0,0,0,0)`:

{{cite shaders/core/deferred_lighting.frag "if (gBuffer0Sample.rgb == vec3(0.0) && gBuffer0Sample.a == 0.0) {"}}

The same exact-equality test is duplicated in `rt/shadow_trace.slang`,
`rt/gi_trace.slang`, `rt/gi_denoise.slang`'s upsample, `compute/ssao.slang` and
`postprocess/ssr.slang` (each as `all(position == float4(0.0))`). The seventh reader
opts out: the subsurface blur runs only on pixels whose normal `w` is negative, so
its centre is never sky, but its taps are not tested. It weights a tap by its
distance from the camera against the centre's, and a sky tap arrives as the world
origin, at the camera's own distance from it, rather than as an absence:

{{cite shaders/postprocess/sss.slang "float distanceAt(int2 p) { return distance(gPosition.Load(int3(p, 0)).xyz, pc.cameraPos.xyz); }"}}

The Gaussian depth weight rejects those taps only because a skin pixel's distance
is normally far from the camera's distance to the origin; where the two agree, sky
taps get real weight in the blur.

:::key
GBuffer0's clear value is load-bearing. Change it or renumber the attachments and
six independent shaders silently begin treating geometry as sky — no validation
error, because each is still reading a legal float. Storing view-space instead of
world-space position breaks the buffer differently and just as quietly: the
coverage test still fires correctly on unwritten pixels, because the clear value
is unchanged, but every reader goes on treating the coordinate as world space —
the two raygens trace from it, SSR marches from it, deferred lighting differences
it against the camera position — and the one locus that *is* misclassified moves
from the world origin to the camera. Any non-metallic surface passing exactly
through the world origin is already misclassified today.
:::

## What the velocity vector sees

The vertex stage carries two clip positions; the fragment stage differences them
into a screen-space UV delta:

{{cite shaders/core/gbuffer.frag "outVelocity = (currentNDC - prevNDC) * 0.5;"}}

The previous position is the vertex pushed through the instance's *previous* world
matrix and the previous frame's view-projection, both read on the GPU:

{{cite shaders/core/gbuffer.vert "fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(inPosition, 1.0));"}}

So the buffer carries object motion as well as camera motion. `prevWorld` is
maintained by the ECS transform system: when an entity's transform changes, its old
world matrix becomes `prevWorld`; on the next frame in which it does not change
again, `prevWorld` catches up to `world`, so a mover that stops reports zero motion
one frame later rather than carrying a stale delta forever.

One asymmetry remains. The current frame's projection is TAA-jittered before it
reaches the pass, while the stored previous view-projection is not:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_prevViewProj = m_proj * m_view;   // unjittered"}}

The recorded delta therefore carries this frame's sub-pixel jitter. Anything
consuming the buffer for other than `historyUV = uv - velocity` — an optical-flow
metric, motion blur, a DLSS motion input — must account for that term.

## What a draw is

Draw granularity is one material run of one instance, not one mesh. When a model is
uploaded, `materialRuns` run-length-encodes its per-triangle material IDs into
submeshes; `RenderScene` expands every visible instance into one `Draw` per submesh,
and each is one indirect command when it is drawn. A mesh
whose triangles already arrive grouped by material costs a handful of draws and one
with interleaved materials costs one draw per group. Nothing in the engine does the
grouping: the loaders append `materialPerTriangle` in traversal order, one Assimp mesh
(a glTF primitive) at a time as the node walk reaches it, and no sort by material
exists anywhere in the asset pipeline. Runs are grouped by construction or not at all.

{{cite ohao/render/scene/model_upload.cpp "std::vector<SubmeshDesc> materialRuns(const Model& model, uint32_t triangleCount) {"}}

{{cite ohao/scene/asset/model_loader.cpp "model->materialPerTriangle.push_back(matIdx);"}}

The pass issues no per-draw command at all. `RenderScene` uploads the draw list as its
draw table, GPU culling turns the table into indirect command lists, and the pass
binds a pipeline and draws a list with `vkCmdDrawIndexedIndirectCount`, the count read
on the GPU. Each command's `firstInstance` is the draw's index in the draw table, so
the vertex shader starts from the draw record (set 1, binding 5), which names the
instance slot and the submesh's material slot:

{{cite shaders/core/gbuffer.vert "const DrawRecord d = draws[gl_InstanceIndex];"}}

It reads the instance (world and previous world matrices) at that slot and passes the
slot and the material slot to the fragment shader as flat varyings; the fragment shader
turns the pair into a row of the material table. The instance owns a list of material
references; a slot past the end of the list falls back to material 0, the default
material, rather than reading out of bounds:

{{cite shaders/includes/scene/scene_data.glsl "((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)"}}

Three 16-byte rows per material then give base colour and albedo texture, roughness,
metallic, normal and emissive textures, and the rough/metal texture and emissive
strength. Texture indices are bindless slots stored as float bits and recovered with
`floatBitsToUint`. Nothing is resolved by name: the only per-frame descriptor work is
rewriting the slot's scene set, since the table buffers can be replaced when they
grow.

## Two render passes, five timed parts

The pass is GPU-driven, and `execute()` records five parts, each timed under its own
name in the pass registry: `cull.instances` (the draws visible last frame and in the
frustum), `gbuffer.early` (draws them), `hiz.build` (the max-depth pyramid of that
depth), `cull.late` (every draw against the frustum and the pyramid) and
`gbuffer.late` (draws what is newly visible):

{{cite ohao/render/deferred/gbuffer_pass.cpp "timerBegin(cmd, "hiz.build");"}}

The render graph does not time the pass — it is registered self-timed, since the
timer keeps one query open at a time — so `render.stats` reports these five names and
no `gbuffer`. The culling itself (phases, compaction, the Hi-Z test) is the GPU culling
unit's subject; the mesh bounds it tests are computed once, at registration, by a
single pass over the vertices:

{{cite ohao/render/scene/mesh_registry.cpp "mn = glm::min(mn, v.position);"}}

The two draws need two render passes over the same targets. The early one clears all
five and leaves the colours as attachments and the depth in
`DEPTH_STENCIL_READ_ONLY_OPTIMAL`, where the Hi-Z build samples it; the late one loads
them and leaves them for the readers, the colours shader-readable:

{{cite ohao/render/deferred/gbuffer_pass.cpp "a.loadOp = late ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;"}}

They share one framebuffer and one set of pipelines, which Vulkan allows only between
compatible render passes, and compatibility includes the subpass dependencies. So both
passes carry the same two dependencies, each the union of what either needs: in, the
previous frame's readers, the early pass's writes and the Hi-Z build's reads; out, the
late pass's attachments and every later reader.

{{cite ohao/render/deferred/gbuffer_pass.cpp "// Identical in both passes (render pass compatibility includes the dependencies: one"}}

Each render pass draws its phase's four lists. The first two are instance draws: the
one-sided ones drawn with back faces culled, the double-sided ones with no face
culling:

{{cite ohao/render/deferred/gbuffer_pass.cpp "const VkPipeline pipeline = wire ? m_wireframePipeline : (k == 0 ? m_cullBackPipeline : m_pipeline);"}}

The other two are the meshlet path's, one-sided and double-sided again, drawn with task
and mesh shaders where the device has them and through the vertex-pulling fallback where
it does not; the meshlets unit covers both:

{{cite ohao/render/deferred/gbuffer_pass.cpp "m_culling->drawMeshletFallback(cmd, (late ? 2u : 0u) + k);"}}

Sidedness is the material's. `MaterialDesc::doubleSided` defaults to true, and only the
glTF loader says otherwise, from Assimp's `TWOSIDED` key (glTF's `doubleSided`, false
unless set); scene-file, OBJ and FBX materials stay double-sided. The culled pipeline
takes counter-clockwise triangles as front faces, glTF's convention:

{{cite ohao/render/deferred/gbuffer_pass.cpp "rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;"}}

`cull_parity_gpu_test` renders closed boxes with one-sided and with double-sided
materials and finds the images identical: the faces culled were hidden anyway.

## Flagging tiles for subsurface

The pass also classifies the frame for the SSS blur. A fragment of a subsurface
material flags its 16×16 tile (`kSSSTile`) in a storage buffer at set 1, binding 4,
owned by the pass, and appends the tile to the vertical blur's list the first time;
it also flags the `kSSSRadiusTiles` (two) tile rows above and below for the
horizontal blur, whose output the vertical one reads:

{{cite shaders/core/gbuffer.frag "void flagSkin(uvec2 pixel) {"}}

A plain read comes before the atomic, so most fragments of a tile already flagged
take no atomic at all. Hidden fragments should not flag, so the shader declares early
fragment tests: depth is tested before it runs, and a skin fragment behind what the
depth buffer already holds never writes the buffer. That is the depth buffer *so far*:
a skin fragment drawn before the surface that later covers it still flags its tile,
which costs the blur a tile of work and changes no pixel, since the blur only acts
where the final normal `w` is negative.

{{cite shaders/core/gbuffer.frag "layout(early_fragment_tests) in;"}}

`execute()` resets the buffer before the render pass every frame — two empty
indirect dispatches in the header, the tile mask zeroed for the frame's tile count —
behind a barrier against the previous frame's indirect and shader reads of it:

{{cite ohao/render/deferred/gbuffer_pass.cpp "const uint32_t header[shared::kSSSHeaderWords] = {0u, 1u, 1u, 0u, 0u, 1u, 1u, 0u};"}}

The buffer is host-mapped, and `sssTilesFlagged()` reads the vertical list's count
from it: the tiles the last frame whose fence has been waited on flagged.

The fragment stage's push-constant block is 16 bytes, the SSS tile grid and padding;
the meshlet stages see a second range, those 16 bytes followed by the 48-byte
`MeshletPush`, 64 bytes in all, half the 128-byte floor Vulkan guarantees, so the pass
has no device-limit hazard to check for:

{{cite ohao/render/deferred/gbuffer_pass.cpp "static_assert(sizeof(DrawPushConstant) + sizeof(gpu::shared::MeshletPush) <= 128);"}}
The per-pass data — the camera's current and previous view-projection — lives in a
128-byte uniform buffer, one per frame slot, host-visible and persistently mapped,
written in `execute` after that slot's fence wait.

## The descriptor set nobody reads

`createDescriptors()` builds a complete five-binding combined-image-sampler set
over the G-buffer, with its own nearest-filter sampler, and re-creates it on
every resize. Its two accessors have zero callers in the tree:

{{cite ohao/render/deferred/gbuffer_pass.hpp "VkDescriptorSet getGBufferDescriptor() const"}}

This is not the "unused function, live formula" trap — the work is genuinely
duplicated rather than shared. `DeferredLightingPass` builds an equivalent set
itself from the raw image views, with its own sampler, inside the wider layout it
needs anyway:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_gbufferPass->getPositionView(),"}}

The accidental benefit is that resize is safe: `createDescriptors()` destroys its
pool and allocates a fresh `VkDescriptorSet`, so the handle changes every resize,
and nobody holds a stale one only because nobody holds one at all.

## Contracts

- GBuffer0's clear value `(0,0,0,0)` *is* the coverage bit, tested by exact float equality in six of its seven readers. Changing the clear colour or the attachment order breaks all four with no diagnostic; changing the position's space leaves the test intact and breaks the two ray-tracing readers instead.
- `m_prevViewProj` must still hold last frame's value when `setViewProjection` runs, and must not be refreshed until the frame ends. Refresh it first and the previous clip position is built from this frame's *unjittered* `m_proj * m_view` while the current one uses the *jittered* projection, so camera-induced velocity collapses to the sub-pixel jitter delta rather than to zero — TAA keeps reprojecting, onto itself.
- Velocity encodes camera and object motion (through `prevWorld`) and includes the current frame's TAA jitter.
- The normal target's `w` is signed: emissive luminance when positive, minus the subsurface strength when negative. Every reader must clamp it to the half it means (`max(w, 0)` in lighting, `max(-w, 0)` in SSS).
- The SSS tile buffer is one buffer shared by every frame in flight, reset by a transfer each frame; the barrier in `resetSSSTiles` orders that reset after the previous frame's SSS reads only because both are recorded on the same queue.
- The only per-draw state is `firstInstance`, the draw's index in `RenderScene`'s draw table; the draw record gives the instance slot and the material slot, and the instance gives its material list, so the pass is only as correct as the draw table `RenderScene` uploads.
- Bindless sentinels are inconsistent: albedo and normal test `!= 0xFFFFFFFFu`, while rough/metal and emissive test a hard-coded capacity — {{cite shaders/core/gbuffer.frag "if (roughMetalTexIdx < 4096u) {"}} — matching `BindlessTextureManager`'s default `maxTextures` of 4096. Raise that capacity and those two slots stop binding above index 4095.
- Back faces are culled only for one-sided materials, with counter-clockwise front faces. A one-sided material on a mesh wound the other way renders inside out; materials that never state their sidedness stay double-sided (`VK_CULL_MODE_NONE`).
- The early and late render passes must stay compatible — the same attachments, formats, sample counts and subpass dependencies — because they share the framebuffer and the pipelines. A dependency added to one must be added to both.
- `execute()` begins both render passes, and so performs all five clears, every frame — with no scene bound only the culling and the draws are skipped. Lighting, sky, SSR and TAA all sample these attachments; the clears are what make an empty frame defined, all-background rather than whatever memory held. {{cite ohao/render/deferred/gbuffer_pass.cpp "// Always run both render passes, scene or not: the early one's clears are what every"}}
- The vertex shader declares bone index and weight inputs it never reads; skinning was removed from this pass and the slots remain only to match the shared `Vertex` layout.
