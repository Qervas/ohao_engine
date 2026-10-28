---
module: graph
id: culling
title: GPU culling
standard: v2
---

## A draw table in, indirect lists out

`GpuCulling` (`render/deferred/gpu_culling.{hpp,cpp}`) decides on the GPU which draws
a frame rasterizes. Its input is `RenderScene`'s draw table, one 64-byte `DrawRecord`
per visible instance's submesh: the index range and vertex offset, the instance-table
slot, the material slot, a one-sided flag, the submesh's meshlets and the mesh's local
bounding box. Its output is lists of indirect commands. Most are
`VkDrawIndexedIndirectCommand`s that the G-buffer and CSM passes draw with
`vkCmdDrawIndexedIndirectCount`, the count read from a GPU buffer:

{{cite ohao/render/deferred/gpu_culling.cpp "vkCmdDrawIndexedIndirectCount(cmd, m_commands.buffer"}}

The rest are the G-buffer's meshlet lists, task-shader dispatches drawn with
`vkCmdDrawMeshTasksIndirectCountEXT` (below, and the meshlets unit). So the CPU records
the same handful of commands whatever the instance count: three dispatches per culling
phase (six when the mesh-shader fallback runs), a pyramid build, and four indirect draws
per G-buffer render pass. The work is all in Slang modules under `shaders/cull/`:
`instances.slang`, with the entry points `classify`, `scanBlocks` and `scatter` and the
fallback's `classifyMeshlets` and `scatterMeshlets`; `hiz.slang`, with `fromDepth` and
`downsample`; and `common.slang`, no entry point, the tests every culler shares.
`DeferredRenderer` owns the one `GpuCulling`, hands it to both passes, and exposes it
through `culling()` for its settings and for tests.

## Three phases

A culling call names a phase, the lists it writes, the view-projection the draws will be
rasterized with and the camera's position (for the meshlets' cone test):

{{cite ohao/render/deferred/gpu_culling.hpp "void cull(VkCommandBuffer cmd, uint32_t phase, uint32_t firstList, uint32_t listCount, const glm::mat4& viewProj,"}}

The phases are the constants `kCullPhaseEarly`, `kCullPhaseLate` and `kCullPhaseShadow`
in `shaders/shared/layout.json`, generated into Slang and C++ alike. Their roles follow
the two-pass occlusion scheme. A per-draw visibility buffer remembers what the last late
phase saw. The early phase lists the draws that were visible then and are inside the
frustum now (an alpha-tested draw and a blended one are in no G-buffer list: the
first is drawn by a discard pipeline after the late phase, the second by the forward pass):

{{cite shaders/cull/instances.slang "member = gbuffer && frustum && visibility[i] != 0u && inFrustum;"}}

The early G-buffer pass draws them, and its depth is a good guess at this frame's
occluders. The late phase then tests every draw against the frustum and a Hi-Z pyramid
of that depth, records the result for the next frame, and lists only what is visible now
and was not drawn early:

{{cite shaders/cull/instances.slang "member = gbuffer && visible && !drawnEarly;"}}

A draw drawn early is never drawn again, and a draw that came into view this frame (or
was disoccluded) is drawn late, so every visible draw is drawn exactly once. The G-buffer
records the whole sequence itself, between its two render passes:

{{cite ohao/render/deferred/gbuffer_pass.cpp "if (haveScene) m_culling->cull(cmd, shared::kCullPhaseEarly, shared::kCullListEarly, 4, viewProj, m_cameraPos);"}}

The shadow phase is for a CSM cascade: frustum only, no history, no pyramid. It tests
only the four side planes, because the cascade pipeline clamps depth and a caster in
front of the cascade's near plane still lands in the map:

{{cite shaders/cull/common.slang "if (sidesOnly) allOut &= 15u;"}}

## Four lists a G-buffer phase

A G-buffer phase writes four lists, from `kCullListEarly` (0) or `kCullListLate` (4).
The first two are instance draws, one-sided (drawn with back faces culled) and
double-sided (no face culling); the last two are the same split for draws on the
meshlet path. `classify` picks a draw's list from its sidedness and its path:

{{cite shaders/cull/instances.slang "if (pc.extra.y >= 2u && (d.info.y & kDrawOneSided) == 0u) k = 1u;"}}
{{cite shaders/cull/instances.slang "if (pc.extra.y >= 4u && onMeshletPath(d)) k += 2u;"}}

A cascade writes one list, `kCullListCascade0 + c`, every draw on the instance path, since
the shadow pipeline's face culling is its own. Two G-buffer phases of four and four
cascades of one make `kCullLists`, twelve lists, each with room for every draw. Which
draws take the meshlet path is the `geometry` setting, carried in `CullPush.extra.z`: none
(`instances`), every draw that has meshlets (`meshlets`), or, the default, those of
`kMeshletMinTriangles` (4,096) triangles or more (`auto`):

{{cite shaders/cull/instances.slang "return pc.extra.z == kGeometryAuto && d.draw.y / 3u >= kMeshletMinTriangles;"}}

The meshlets unit gives the measurement behind that threshold.

## Compaction without atomics

An atomic append is the obvious way to build a list on the GPU, and it orders the list
by whichever thread won each race. These lists are built by prefix sum instead, so a
list holds its draws in draw-table order on every run. `classify` runs 256 threads a
workgroup (`kCullGroupSize`), one per draw: it decides the draw's membership in the
phase's lists, a `uint4` with at most one bit set, then a Hillis-Steele scan of those
`uint4`s in shared memory gives each member its offset within its workgroup, for all
four lists at once:

{{cite shaders/cull/instances.slang "return gScan[t] - c;"}}

`scanBlocks` is a single workgroup that turns the workgroups' totals into their first
output slots. Each of its threads sums a contiguous chunk of the totals, the threads
scan those sums, and each walks its chunk again writing offsets, so one workgroup covers
any number of blocks; its last thread writes the phase's list counts the indirect draws
read:

{{cite shaders/cull/instances.slang "const uint chunk = (blocks + kCullGroupSize - 1u) / kCullGroupSize;"}}

`scatter` writes each member's command at its block offset plus its local offset. On an
instance list the command's `firstInstance` is the draw's index in the draw table:

{{cite shaders/cull/instances.slang "commands[at + 4u] = i;"}}

That is how the vertex shaders find their data: `gbuffer.vert` and `shadow_csm.vert`
read `draws[gl_InstanceIndex]` for the instance slot (and the G-buffer the material
slot), which needs the device's `drawIndirectFirstInstance` feature, as the GPU count
needs `drawIndirectCount`. On a meshlet list the command is a task dispatch instead, one
task workgroup per `kMeshletTaskGroup` (32) of the draw's meshlets, with the draw's
index in the fourth word, where the task shader reads it:

{{cite shaders/cull/instances.slang "commands[at + 3u] = i;    // the draw record (the task shader reads it)"}}

Both kinds are five words, so the lists share one buffer and one stride.
`cull_parity_gpu_test` checks the determinism directly: two fresh renderers produce
byte-identical counts, visibility and commands.

## The meshlet lists and the fallback

With task and mesh shaders, a meshlet list is drawn as it stands: `drawMeshTasks` hands
it to `vkCmdDrawMeshTasksIndirectCountEXT`, and each draw's task shader culls its own
meshlets. Without them (no `VK_EXT_mesh_shader`, or `Settings::meshShaders` off),
`cull` runs three more dispatches after a G-buffer phase's scatter:

{{cite ohao/render/deferred/gpu_culling.cpp "if (gbufferPhase && listCount == 4 && fallbackRuns()) {"}}

`classifyMeshlets` runs one thread per *meshlet entry*, every meshlet of every draw in
draw order, finds its draw by binary search, and tests the meshlet only if that draw is
on one of the phase's two meshlet lists; `scanBlocks` compacts again; `scatterMeshlets`
writes one `VkDrawIndirectCommand` per surviving meshlet. The four per-meshlet lists
(`kCullMeshletLists`: early and late, one- and double-sided) live in their own buffer,
and their counts follow the twelve draw-list counts, `kCullCounts` words in all:

{{cite ohao/render/deferred/gpu_culling.cpp "pc.extra = {shared::kCullLists + meshletList, 2u, drawPush.extra[2], 0u};"}}

The meshlet test itself, in the task shader and in `classifyMeshlets` alike, is
`common.slang`'s `meshletVisible`: the normal cone, then the frustum and, in the late
phase, the pyramid against a box around the meshlet's bounding sphere. The meshlets unit
covers it, the per-meshlet draws and the vertex shader that pulls them.

## The Hi-Z pyramid

`buildHiZ` reduces the early pass's depth to a pyramid of maxima, an `R32_SFLOAT` image
whose texels each hold the farthest depth over their footprint. Level 0 is the frame
halved, rounded up, then padded to powers of two:

{{cite ohao/render/deferred/gpu_culling.cpp "uint32_t w = pow2((width + 1) / 2), h = pow2((height + 1) / 2);"}}

The padding is the correctness of the whole test. Vulkan sizes each mip level by halving
the one above, rounding down. Unpadded, a 1280×720 frame's half-size level of 640×360
would halve to 320×180, 160×90, 80×45 and then 40×22: an odd level's last row of texels
is dropped by the next. With power-of-two levels, texel $t$ of level $L$ covers exactly the
pixels

$$\left[\, t \cdot 2^{L+1},\ (t+1) \cdot 2^{L+1} \right)$$

in each axis, so a rectangle of pixels maps to texels by a shift. Here $L$ counts from
the half-resolution level 0, which is why the exponent is $L + 1$. The first build
assumed levels rounded up, read past the levels that existed, and culled visible walls.
`fromDepth` takes the maximum of a 2×2 block of the depth buffer, clamping reads to its
edge, so the padding repeats the frame's edge depth and no footprint over the frame
loses a pixel:

{{cite shaders/cull/hiz.slang "const float a = depth.Load(int3(min(s, last), 0));"}}

`downsample` does the same from one level to the next, one dispatch of 8×8 workgroups a
level, down to 1×1. The pyramid is read by the late phase's `classify`, by
`classifyMeshlets`, and by the late G-buffer pass's task shaders, which is why the build
waits for the task stage's reads of the previous frame.

## The tests

The tests live in `common.slang`, one module, so that the draw culler and both meshlet
cullers cannot drift apart. `boxBounds` projects a local box's eight corners through the
instance's world matrix (the first four `float4`s of its instance-table entry, the
matrix's columns) and the phase's view-projection. A box is outside the frustum only when
all eight corners are outside one clip plane, which is conservative: a box that misses
the frustum across a corner, outside no single plane, is kept. A corner behind the near
plane leaves the box with no screen rectangle, and such a box is never occluded:

{{cite shaders/cull/common.slang "if (clip.w <= 1e-6 || clip.z < 0.0)"}}

Otherwise the corners give an NDC rectangle and the nearest depth. `occluded` widens the
rectangle by a pixel on each side, for jitter and rounding:

{{cite shaders/cull/common.slang "const int2 r0 = clamp(int2(floor(pmin)) - 1, int2(0), size - 1);"}}

and picks the finest level at which it spans at most two texels in each axis:

{{cite shaders/cull/common.slang "if ((r1.x >> s) - (r0.x >> s) <= 1 && (r1.y >> s) - (r0.y >> s) <= 1) break;"}}

The maximum of those at most four texels is the farthest depth drawn anywhere over the
rectangle. If the box's nearest point is farther still, every pixel it could cover
already holds something nearer, and it is culled:

{{cite shaders/cull/common.slang "return b.minDepth - 1e-6 > far;"}}

The depth tested is the depth the early pass wrote, and the view-projection is the
jittered one the G-buffer rasterizes with, so the test and the raster agree to the
pixel.

## History and settings

The visibility history belongs to one version of the draw table: draw $i$ of another
version is another draw. `beginFrame`, called by `DeferredRenderer` before the graph
runs, clears it when the table's version has changed or a reset was asked for:

{{cite ohao/render/deferred/gpu_culling.cpp "if (m_historyReset || m_historyVersion != scene.drawsVersion()) {"}}

`DeferredRenderer::resetTemporalState` asks, so the first frame after a reset draws
nothing early and everything late, and a capture depends only on the frames since its
reset, never on what the renderer drew before:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_culling) m_culling->resetHistory();"}}

`beginFrame` also rewrites the frame slot's descriptor set (the scene's tables can be
replaced when they grow) and grows the culling buffers to the draw count, doubling from
1024; the per-meshlet buffers grow to the meshlet-entry count only when the fallback
will run, since task shaders never touch them:

{{cite ohao/render/deferred/gpu_culling.cpp "const bool fallback = !meshShadersActive() && m_settings.geometry != Geometry::Instances;"}}

Growth waits for the device once, since frames in flight still read the old buffers.
`Settings` holds four switches: `frustum` (off, every draw is drawn late, in draw order,
and every meshlet), `occlusion` (the late phase's pyramid test, applied only with
`frustum`), `geometry` (above) and `meshShaders` (off, the fallback even where task and
mesh shaders exist):

{{cite ohao/render/deferred/gpu_culling.cpp "if (m_settings.frustum && m_settings.occlusion && phase == shared::kCullPhaseLate) flags |= shared::kCullOcclusion;"}}

A meshlet test gets the same flags plus `kCullCone`, from `meshletFlags(phase)`.
`setCapture(true)` copies every frame's counts, visibility and commands (and the
fallback's per-meshlet commands when it ran) to a host buffer after its late phase, and
`capture()` returns the last one; that is what the parity test reads.

## What it costs

Measured with `render_bench` on an RTX 5070 Laptop GPU at step 11, before meshlets, in
runs flagged noisy by outside load: on `cornell_720p`, `cull.instances`, `hiz.build` and
`cull.late` took 0.039 ms together and the two G-buffer passes 0.068 ms. On
`bench100k_1080p`, 100,000 instances of 20 meshes on a 400 m grid
(`buildRenderBenchWorld` in `tools/bench/bench_world.hpp`), `cull.instances` took
0.056 ms, `hiz.build` 0.025 ms and `cull.late` 0.058 ms, against 1.69 ms for
`gbuffer.early`; the CPU recorded and submitted the frame in 0.123 ms, as against
0.130 ms for 10,000 instances. Culling and the pyramid together are budgeted at 0.10 ms
on `cornell_720p`:

{{cite tests/bench/render_budgets.json ""name": "cull+hiz""}}

:::why
The early list comes from last frame's visibility rather than from a depth pre-pass so
that the occluders cost nothing extra: they are drawn once, into the G-buffer, and their
depth is the pyramid's source. The price is the late pass. A draw that disappears from
view is still drawn early for one frame (it was visible last frame), and a draw that
appears is drawn late, after the pyramid, so the image never depends on the guess: test
6 renders the same frames with culling on and off and requires identical images.
:::

## Contracts

- Culling changes what is drawn, never what is seen. `cull_parity_gpu_test` (test 6) holds it: a built occlusion scene over 1, 2 and 4 frames, Cornell at 10 frames and the helmet room at 30 frames with post render identically (max_abs 0, identical PNGs) with culling on and off; the boxes behind the wall and off screen are in no list and every box in view is in exactly one. Test 7 holds the same for the meshlet path.
- The view-projection passed to `cull` must be the one the draws are rasterized with, jitter included, or the occlusion test and the raster disagree by the jitter.
- `firstInstance` in every instance-list command is a draw-table index, not an instance slot. A vertex shader drawing from these lists must read its draw record first. A meshlet-list command carries the same index in its fourth word.
- A G-buffer phase writes four lists in a fixed order (instance one-sided, instance double-sided, meshlet one-sided, meshlet double-sided); `GBufferPass::drawPass` draws them by that order, and the fallback reads the third and fourth lists' membership from the draw scan's `local` words.
- The pyramid's levels must stay powers of two. Sized from the frame directly, Vulkan's round-down halving drops texels at odd levels and the test reads past them.
- `resize` takes the G-buffer's depth view and must run after the G-buffer is recreated; `DeferredRenderer::onResize` calls it after `GBufferPass::onResize`.
- The visibility history is valid only for the draw-table version it was recorded against and only since the last temporal reset; anything that reorders the draw table must bump its version.
- The culling bounds are the mesh bounds and meshlet spheres captured at registration. A mesh whose vertices change on the GPU afterwards keeps its old boxes, and is culled against them.
