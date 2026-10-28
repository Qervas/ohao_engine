---
module: deferred
id: clustered-lighting
title: Clustered lighting
standard: v2
---

## A light list per froxel

A deferred lighting pass that loops over every light pays, at every pixel, for lights
whose range ends metres away. Clustered lighting cuts the view frustum into cells and
gives each cell the list of lights that can reach it; a pixel then loops over its
cell's list. OHAO's cells are froxels: 16 screen tiles across, 9 down and 24 depth
slices, the same 3,456 at any resolution, numbered slice-major:

{{cite shaders/shared/layout.json "kClusterTilesX * kClusterTilesY * kClusterSlices; froxel f = (slice * kClusterTilesY + y) * kClusterTilesX + x"}}

The lists are rebuilt every frame by a compute pass, `lights.cluster`, which the pass
registry places right after the G-buffer's late phase:

{{cite ohao/render/frame/pass_registry.hpp "{"lights.cluster", PassKind::Compute,"}}

The lists live in one device-local buffer: 3,456 counts, then 256 light indices per
froxel, about 3.4 MiB. A froxel lists at most `kClusterMaxLights` (256) lights.

## Exponential slices

Tiles are uniform in screen space; slices are uniform in the logarithm of view depth,
from `kClusterNear` (0.1 m) to the far plane the deferred renderer was given. With $N$
slices, near distance $n$ and far distance $f$, a point at view depth $d$ falls in slice

$$k(d) \;=\; \Big\lfloor N\,\frac{\ln(d/n)}{\ln(f/n)} \Big\rfloor \;=\; \big\lfloor S \ln d + B \big\rfloor,\qquad S = \frac{N}{\ln(f/n)},\; B = -S\ln n$$

so every slice spans the same depth *ratio*, $(f/n)^{1/N}$. A froxel's depth extent then
grows with distance as its screen footprint does, which keeps froxels roughly the same
shape near and far; uniform slices would waste most of them on distant depth and leave
the near field, where lights crowd the screen, coarse. `LightClusters::build` computes
$S$ and $B$ once per frame and hands the same two numbers to the kernel and to the
lighting pass:

{{cite ohao/render/deferred/light_clusters.cpp "m_sliceScale = static_cast<float>(shared::kClusterSlices) / std::log(farD / shared::kClusterNear);"}}

$f$ is the camera's own far plane, handed over every frame, so the last slice ends where
the projection clips: a surface the G-buffer kept always lies inside some slice's box.
(The lighting shader clamps a pixel's slice to the last one; with a shorter $f$ than the
camera's, pixels past it would land in a box that stops short of them and miss their
lights.) With $n = 0.1$ and $f = 1000$, each slice is about 1.47 times deeper than the
one before it.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setClusterFarPlane(m_camera->getFarPlane());"}}

The froxels are slices of a perspective frustum. An orthographic projection (its
`[2][3]` is 0) builds no lists: lighting sums every light instead, the same image.

{{cite ohao/render/deferred/deferred_renderer.cpp "const bool perspective = m_proj[2][3] != 0.0f;"}}

## The froxel's box, made conservative

One workgroup of 64 threads builds one froxel's list. It first bounds the froxel in view
space: the tile's four screen corners at the slice's near and far depths, eight points,
and their axis-aligned box. A corner is the point at depth $d$ on the ray through an NDC
position, inverted through the projection's four non-trivial terms:

{{cite shaders/compute/light_cluster.slang "return float3(d * (ndc.x + pc.proj.z) / pc.proj.x, d * (ndc.y + pc.proj.w) / pc.proj.y, -d);"}}

`proj.z` and `proj.w` are the projection's `[2][0]` and `[2][1]`, where TAA's sub-pixel
jitter sits, and the renderer builds the lists with the jittered projection the G-buffer
was rasterized with. A pixel's G-buffer position lies on the jittered ray through its
centre, so it lies inside its own tile's frustum, not the unjittered one's:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_lightClusters->build(c, frameIndex, m_view, jitteredProj, m_width, m_height, m_clusterFar, m_frameNumber);"}}

Then the box is padded. The tile grows by 1.5 pixels on every side, the slice by 0.1%
in each direction (the first slice reaches back to the eye, so geometry nearer than
0.1 m is still covered), and each light's range by 0.01% plus $10^{-4}$:

{{cite shaders/compute/light_cluster.slang "const float2 p0 = float2(tx, ty) * tile - 1.5, p1 = float2(tx + 1u, ty + 1u) * tile + 1.5;"}}
{{cite shaders/compute/light_cluster.slang "const float dFar = exp((float(slice + 1u) - pc.screen.w) / pc.screen.z) * 1.001;"}}
{{cite shaders/compute/light_cluster.slang "const float r = l.direction.w * 1.0001 + 1e-4;"}}

The padding is what the exactness argument below needs. The kernel decides membership
in view space with its own float arithmetic, and the lighting shader picks a pixel's
froxel and measures its distance to a light with different arithmetic (world space, a
logarithm, a division); a light sitting on a froxel boundary must never be dropped by one
side and counted by the other. Padding only ever adds lights to a list.

## The sphere test, in light order

Each thread tests one light a round, 64 lights per round, until the table is exhausted.
A directional light reaches everywhere; a point or spot light is its range sphere, and
the test is the closest point of the box to the sphere's centre:

{{cite shaders/compute/light_cluster.slang "hit = dot(q, q) <= r * r;"}}

where `q = clamp(c, lo, hi) - c`, the vector from the centre to the nearest point of the
box. A spot light is tested as its full sphere, not its cone: a spot facing away from a
froxel still enters the list and costs the lighting loop one cone test there, never a
wrong pixel.

The hits must be written in light-index order, because the lighting loop must add the
same terms in the same order as a loop over every light. The workgroup compacts them
through a 64-bit mask in shared memory: each hitting thread sets its bit, then counts
the set bits of the threads below it, and that count is its place in the list:

{{cite shaders/compute/light_cluster.slang "if (hit) InterlockedOr(gMask[t >> 5u], 1u << (t & 31u));"}}
{{cite shaders/compute/light_cluster.slang "if (hit && written + before < kClusterMaxLights) clusters[listBase + written + before] = i;"}}

The `InterlockedOr` is an atomic on shared memory within one workgroup, and OR commutes,
so the mask does not depend on which thread got there first. No workgroup writes another
froxel's list, so there are no atomics across workgroups on the lists, and two runs build
them word for word identically.

:::why
The usual light-binning kernels let threads append with a global atomic counter: one
pass, any order. The order would then depend on scheduling, and a floating-point sum in a
different order is a different sum, so the image would change run to run and could not
equal the every-light loop. The ordered compaction costs two barriers and a few
`countbits` per round of 64 lights; it buys bit-identical lists and images.
:::

## The cap and the overflow

A froxel keeps its first 256 hits, in light order, and thread 0 writes the count.
It also records what the cap cut into a three-word stats buffer: the lights dropped, the
froxels that dropped any, and the largest count before the cut. Those integer atomics do
cross workgroups, but integer addition and maximum commute, so the totals are exact.

{{cite shaders/compute/light_cluster.slang "InterlockedAdd(stats[0], written - kClusterMaxLights);"}}

Each frame slot has its own host-visible stats buffer, cleared when the slot is built and
read once that slot's fence has signalled; the newest finished frame's values are kept:

{{cite ohao/render/deferred/light_clusters.cpp "if (!m_latest || s.frame >= m_latest->frame) m_latest = Overflow{s.frame, words[0], words[1], words[2]};"}}

`render.stats` reports the dropped-light count as `lightsOverflow` when it is nonzero and
the frame it came from is among the frames the call summarises; absent otherwise:

{{cite ohao/runtime/frame_capture.cpp "if (overflow && overflow->lights > 0 && overflow->frame >= frames.front().frame &&"}}

An overflowing froxel is the one case where the clustered image differs from summing
every light: the lights past the 256th are dropped whatever their brightness, because
the list keeps the lowest indices. The overflow is reported rather than hidden.

## How lighting reads the lists

The lighting fragment shader finds its pixel's tile from `gl_FragCoord` and its slice from
the logarithm of its view depth, with the same $S$ and $B$:

{{cite shaders/core/deferred_lighting.frag "const float s = viewDepth > 0.0 ? floor(log(viewDepth) * pc.sliceScale + pc.sliceBias) : 0.0;"}}

clamps the slice to 0..23 (a pixel nearer than 0.1 m uses slice 0; none lies past the
far plane, which clips it), reads the froxel's count and loops over its indices into the light
table:

{{cite shaders/core/deferred_lighting.frag "const LightGpu light = lights[clustered ? clusterData[listBase + k] : k];"}}

With push flag bit 6 the same loop runs over every light in the table instead. The pass
sets the bit when it has no froxel buffer, which is what `setClusteredLighting(false)`
arranges: the deferred renderer then hands the lighting pass a null buffer and does not
add `lights.cluster` to the graph at all.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_clusterBuffer == VK_NULL_HANDLE) m_params.flags |= 64; // every light (no froxel lists)"}}

## Why the two loops give the same image

The every-light loop adds, for each light $i$ in index order, a term $t_i$, and skips
the light when its attenuation or its shadow is exactly zero:

{{cite shaders/core/deferred_lighting.frag "if (attenuation == 0.0) continue;   // adds exactly zero: past its range or its cone"}}

A light left out of a pixel's froxel has its range sphere entirely outside the froxel's
padded box, so at that pixel $d > r$ and the attenuation's window is
$\mathrm{clamp}(1 - (d/r)^4, 0, 1) = 0$ exactly: the every-light loop skips that light
too. The clustered loop therefore visits exactly the lights the every-light loop does not
skip, plus some it does skip, in the same ascending order, and the two compute the same
floating-point sum operation for operation.

{{cite shaders/core/deferred_lighting.frag "float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);"}}

:::key
Exactness rests on three things: the attenuation is exactly 0, not small, past the range;
a light left out would have been skipped anyway, so nothing that adds a nonzero term is
removed; and the lists keep light-index order. Change the falloff to one without a hard
zero, reorder the lists, or let a froxel overflow, and the image moves.
:::

## The light table per frame slot

The lists index a light table, which `LightClusters` also owns: one host-visible storage
buffer of 64-byte `LightGpu` records per frame slot, at most `kLightCapacity` (65,536),
grown by doubling from 64 records. The host writes it, so it must be per slot: a GPU
barrier cannot order a host write against a frame still in flight. The slot is written
after its fence wait, and only when the light list's version differs from the version the
slot holds:

{{cite ohao/render/deferred/light_clusters.cpp "if (version != 0 && s.version == version && s.count == count && s.lights.buffer) return;"}}

The version is the light table's counter, raised whenever the raster lights change; the
fallback sun passes 0, which always writes. A still scene of a thousand lights therefore
uploads nothing per frame.

The froxel lists, by contrast, are one buffer shared by every frame slot. Only the GPU
writes them, so a pipeline barrier at the start of the build, which orders this frame's
writes after the previous frame's fragment reads in queue order, is enough:

{{cite ohao/render/deferred/light_clusters.cpp "// The last frame's lighting read the lists before this rewrite (queue order)."}}

## What does not read the lists

The deferred lighting pass is the lists' only reader. RT GI shades its bounce with its own
selection, the four brightest lights by intensity, and the effects' expand pass lights each particle at its centre from the same lists:

{{cite ohao/render/rt/rt_gi_technique.cpp "// The bounce's lights: the brightest by intensity, ties by order, emissive proxies out."}}

Shadows are separate as well: only the first eight shadow-casting lights own an RT
visibility channel (the shadow unit), however many lights the froxels hold.

## Cost

The build is brute force: every froxel tests every light, 3,456 sphere tests per light,
64 lights a round per workgroup. `render_bench` gates it at 0.03 ms on `cornell_720p` and
at 0.10 ms on `bench100k_1080p`, whose scene carries 1,000 point lights and a sun:

{{cite tests/bench/render_budgets.json "{"scene": "bench100k_1080p", "name": "lights.cluster", "passes": ["lights.cluster"], "budgetMs": 0.10, "gatedFrom": 13},"}}

Past a few thousand lights the per-froxel loop over the whole table is the cost to
replace; the spec leaves a hierarchical pre-pass for later:

{{cite docs/specs/2026-10-24-renderer-upgrade-design.md "larger counts is later work."}}

## Test 8

`light_cluster_gpu_test` holds the pass to its promises. Cornell (12 lights, RT shadows),
the helmet room with post effects, and a scene with the bench's 1,000 point lights and
sun each render to byte-identical PNGs with the froxel lists and with every light:

{{cite tests/renderer/light_cluster_gpu_test.cpp "check(same, std::string("t08: ") + c.name + ": clustered = every light, byte for byte");"}}

On the bench lights the froxels hold some of the lights but not all, none overflows,
`render.stats` carries no `lightsOverflow`, and every list holds valid indices in
ascending order:

{{cite tests/renderer/light_cluster_gpu_test.cpp ""t08: bench lights: every list holds valid light indices, ascending");"}}

A thousand lights packed into a 3 m cube overflow on purpose: full froxels keep 256, the
frame renders, the stats count the dropped lights and the largest froxel (all 1,000), and
`render.stats` reports `lightsOverflow`:

{{cite tests/renderer/light_cluster_gpu_test.cpp "check(reported, "t08: packed lights: render.stats reports lightsOverflow");"}}

Two fresh renderers build the same lists word for word:

{{cite tests/renderer/light_cluster_gpu_test.cpp "check(a.lists == b.lists, "t08: twin runs: the same froxel counts and indices, word for word");"}}

On Cornell the first eight casters own shadow channels while the other four render
unshadowed and are counted. Two cases guard the slicing itself: a wall 1,500 m away
under a 3,000 m far plane, lit by a light 20 m in front of it (slices cut off at a fixed
1,000 m would leave it unlit), and an orthographic camera 40 m above a floor of small
lights (perspective froxels there sit far from the pixels they hold). Both render the
every-light image.

## Contracts

- The attenuation must be exactly 0 at and past `direction.w`, and the lighting loop must skip zero terms; both the froxel lists and the every-light loop rely on it to agree.
- Lists must stay in ascending light-index order. Any change to the compaction (a global atomic append, a sort by distance) breaks the byte-for-byte equality with the every-light loop.
- The lists must be built with the projection the G-buffer was rasterized with, jitter included, and the slice mapping the lighting pass uses must be the one the build computed (`sliceScale()`, `sliceBias()`).
- The far plane given to `build` must be at least the farthest depth lighting shades (the renderer passes the camera's): pixels past it are clamped into the last slice, whose box ends at 1.001 times the far plane.
- A froxel past 256 lights drops the highest-indexed ones; the image is then not the every-light image, and `render.stats`' `lightsOverflow` says so.
- The light table is written by the host and must stay one buffer per frame slot, written after that slot's fence; the froxel lists are written only by the GPU and are one buffer, ordered by the barrier at the start of the build.
- `kLightCapacity` and `MAX_LIGHTS` are both 65,536; `kClusterCount` must equal tiles × tiles × slices, which `light_clusters.cpp` asserts.
- Froxels exist only for a perspective projection; an orthographic one must keep lighting on the every-light loop.
