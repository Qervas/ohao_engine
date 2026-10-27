---
module: graph
id: meshlets
title: Meshlets and mesh shaders
standard: v2
---

## Clusters below the draw

GPU culling (the GPU culling unit) decides per draw: a draw is a whole instance's
submesh, and a draw that is partly visible is drawn whole. A meshlet is a smaller unit,
a cluster of at most `kMeshletMaxVertices` (64) vertices and `kMeshletMaxTriangles`
(124) triangles of one submesh, with a bounding sphere and a normal cone. A large draw
on the meshlet path is culled meshlet by meshlet, so the part of a mesh behind an
occluder, off screen or facing away is never rasterized. The limits are the mesh
shader's output sizes:

{{cite shaders/cull/meshlets.slang "out vertices GVertex verts[kMeshletMaxVertices], out indices uint3 tris[kMeshletMaxTriangles])"}}

Meshlets are built once, at mesh registration, by `render/scene/meshlet_builder`; drawn
by task and mesh shaders where the device has `VK_EXT_mesh_shader`; and drawn through a
compute pass and a vertex-pulling shader where it does not. Both paths cull with one
test and transform with one function, and a test requires their images to be identical
to each other and to the instance path's.

## The builder

`buildMeshlets` is greedy. It starts a meshlet at the first unused triangle in index
order, then keeps adding the unused triangle that shares the most vertices with the
meshlet so far. It finds those from a vertex-to-triangle adjacency built once per call,
in compressed rows:

{{cite ohao/render/scene/meshlet_builder.cpp "// Vertex -> triangles (CSR): counts, then each vertex's end, filled backwards to its start."}}

When a vertex joins the meshlet, every unused triangle around it gains a point of
score, from 1 to 3, and is appended to the bucket of its new score. Buckets are read
oldest first, so among the triangles that share as many vertices, the one that became a
candidate earliest wins:

{{cite ohao/render/scene/meshlet_builder.cpp "std::array<std::vector<uint32_t>, 4> bucket;"}}

That order is what makes a meshlet a compact patch rather than a strip. Newest first, a
meshlet chases the edge it just grew and becomes a long ribbon; oldest first, it fills in
around where it started. `scene_tests` measures it on a 300×300 grid, which is
vertex-bound: 64 vertices as an 8×8 patch hold 98 triangles, as a 2×32 strip 62:

{{cite tests/scene/scene_tests.cpp "CHECK(tfill > 0.7, "compact patches, not strips: over 70% of the triangle limit");"}}

A candidate is taken only if it is still unused, still in the bucket it was read from
(a stale entry has since been raised to a higher one), and coherent:

{{cite ohao/render/scene/meshlet_builder.cpp "if (!used[t] && score[t] == s && coherent(t)) {"}}

Coherent means its unit face normal is within about 53 degrees of the meshlet's mean
normal so far, $\hat n \cdot \sum \hat n_i \ge 0.6 \, \lvert \sum \hat n_i \rvert$
(arccos 0.6 is 53.1 degrees), so a meshlet stays flat enough for its normal cone to cull.
A degenerate triangle has no normal and fits any meshlet:

{{cite ohao/render/scene/meshlet_builder.cpp "constexpr float kConeLimit = 0.6f;"}}
{{cite ohao/render/scene/meshlet_builder.cpp "glm::dot(n, normalSum) >= normalLimit;"}}

With no neighbour left, the builder takes the next unused triangle in index order, if
it is coherent:

{{cite ohao/render/scene/meshlet_builder.cpp "if (next == kNone) {   // no neighbour: the next triangle in index order"}}

The meshlet closes when there is no candidate or the next one does not fit, by triangle
count or by the vertices it would add:

{{cite ohao/render/scene/meshlet_builder.cpp "return m.triangleCount < kMaxTriangles && m.vertexCount + newVertices(t) <= kMaxVertices;"}}

Each triangle is stored as its three meshlet-local indices, a byte each, in one `uint`:

{{cite ohao/render/scene/meshlet_builder.cpp "out.triangles.push_back(local[0] | (local[1] << 8) | (local[2] << 16));"}}

Every triangle lands in exactly one meshlet, in a meshlet that holds each of its
vertices once.

## Sphere and cone

A closed meshlet gets its bounds. The sphere is centred on its vertices' box, with the
farthest vertex as its radius:

{{cite ohao/render/scene/meshlet_builder.cpp "m.center = 0.5f * (mn + mx);"}}

The cone's axis is the triangles' mean unit normal. Its cutoff is the sine of the widest
angle between the axis and any triangle's normal, meshoptimizer's convention; a meshlet
whose normals spread past about 84 degrees (cosine 0.1) keeps the cutoff 1, which never
culls:

{{cite ohao/render/scene/meshlet_builder.cpp "if (minDot > 0.1f) m.coneCutoff = std::sqrt(1.0f - minDot * minDot);"}}

With $c$ the sphere's centre, $r$ its radius, $e$ the camera and $\hat a$ the axis, the
meshlet faces wholly away from the camera when

$$(c - e) \cdot \hat a \;\ge\; s \, \lvert c - e \rvert + r$$

where $s$ is the cutoff. The radius term widens the test from the centre to the whole
sphere, the conservative side: a meshlet is culled only when the cone faces away from
every point the sphere could hold.

## Determinism and cost

The builder has no hash maps and no randomness, and it visits triangles in a fixed
order, so the same mesh gives the same meshlets byte for byte, which `scene_tests`
checks on the grid:

{{cite tests/scene/scene_tests.cpp "CHECK(sameMeshlets(set, again), "two builds are identical byte for byte");"}}

It runs inside `registerMesh`, which is load-time work, so its cost is load time.
`render_bench` times it on a million triangles, a 708×708 grid in row order, nine builds
back to back:

{{cite tools/bench/render_bench.cpp "constexpr uint32_t n = 708;   // 2 * 708^2 = 1,002,528 triangles"}}

The median is budgeted at 60 ms:

{{cite tests/bench/render_budgets.json ""measure": "meshletBuild", "budgetMs": 60"}}

The builder sits behind one function, so a better clusterer can replace it. The spec
chose it over meshoptimizer, which would cluster better but adds a dependency:

{{cite docs/specs/2026-10-24-renderer-upgrade-design.md "The builder is deterministic and adds no dependency; meshoptimizer"}}

## Arenas and draw records

`MeshRegistry` keeps three more arenas beside the vertex and index arenas: the meshlets
as 48-byte `gpu::shared::MeshletGpu` records, their vertices, and their packed
triangles:

{{cite ohao/render/scene/mesh_registry.cpp "createArena(m_meshlets, sizeof(gpu::shared::MeshletGpu), kInitialMeshlets, kTableUsage) &&"}}

`registerMesh` builds each submesh's meshlets in turn and records where they start and
how many there are in `MeshInfo::submeshMeshlets`:

{{cite ohao/render/scene/mesh_registry.cpp "buildMeshlets(data.vertices, data.indices, s.firstIndex, s.indexCount, meshlets);"}}

A `MeshletGpu`'s `range` holds its first entries in the two meshlet streams (arena
offsets) and its vertex and triangle counts; `sphere` and `cone` hold the bounds. The
meshlet vertices stay mesh-local, as the index arena's indices do, and a draw adds its
vertex offset:

{{cite ohao/render/scene/mesh_registry.cpp "g.range = {vertexBase + s.vertexOffset, triangleBase + s.triangleOffset, s.vertexCount, s.triangleCount};"}}

Ray tracing does not use them: each mesh's BLAS is built from its original triangles.

{{cite ohao/render/scene/mesh_registry.hpp "Each mesh owns one BLAS built against the arenas, from its original"}}

`RenderScene` carries a submesh's meshlets into its draw: `DrawRecord.info.z` is the first
meshlet and `info.w` the count:

{{cite ohao/render/scene/render_scene.cpp "d.firstMeshlet, d.meshletCount};"}}

and `aabbMin.w`, as a uint's bits, the draw's first *meshlet entry*: the number of
meshlets of all the draws before it. Entries number every meshlet of every draw in draw
order, which is what the fallback culls one by one:

{{cite ohao/render/scene/render_scene.cpp "r.aabbMin = {d.aabbMin.x, d.aabbMin.y, d.aabbMin.z, std::bit_cast<float>(entries)};"}}

## Task and mesh shaders

The device enables `VK_EXT_mesh_shader` when the GPU offers both its task and mesh
stages, and Vulkan 1.1's `shaderDrawParameters`, since the task shader reads its draw's
index (`DrawIndex`):

{{cite ohao/gpu/vulkan/device_setup.cpp "m_meshShaderEnabled = msExt && meshShaderProbe.taskShader == VK_TRUE && meshShaderProbe.meshShader == VK_TRUE;"}}
{{cite ohao/gpu/vulkan/device_setup.cpp "features11.shaderDrawParameters = VK_TRUE;"}}

GPU culling puts the meshlet path's draws on their own lists, as task dispatches of one
workgroup per 32 meshlets (`kMeshletTaskGroup`), and `drawMeshTasks` draws a list with
`vkCmdDrawMeshTasksIndirectCountEXT`:

{{cite ohao/render/deferred/gpu_culling.cpp "m_drawMeshTasksIndirectCount(cmd, m_commands.buffer, VkDeviceSize{list} * m_drawCount * kCommandBytes,"}}

A task workgroup reads its draw's index from the command that launched it, in the
culling commands bound at set 1, binding 10:

{{cite shaders/cull/meshlets.slang "const uint drawIndex = commands[pc.m.config.x + drawId * 5u + 3u];"}}

Each of its 32 threads tests one meshlet:

{{cite shaders/cull/meshlets.slang "keep = meshletVisible(meshlets[d.info.z + local], view, hiz);"}}

then an inclusive scan over the survivors places each in the payload in meshlet order,
and the workgroup launches one mesh workgroup per survivor:

{{cite shaders/cull/meshlets.slang "DispatchMesh(gKeep[kMeshletTaskGroup - 1u], 1, 1, gPayload);"}}

A mesh workgroup has 64 threads, one per output vertex. Its first thread builds the
instance's matrices once into shared memory; then each thread transforms its vertex and
unpacks up to two triangles:

{{cite shaders/cull/meshlets.slang "verts[k] = shadeVertex(d.draw.z + meshletVertices[m.range.x + k], d, gXform);"}}
{{cite shaders/cull/meshlets.slang "tris[k] = uint3(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu);"}}

Thirty-two threads with two vertices each measured slower, 0.232 ms for the helmet
room's `gbuffer.early` at 1080p against 0.218 ms:

{{cite shaders/cull/meshlets.slang "// A thread a vertex (64; 32 threads with two vertices each measured slower: helmet room 1080p"}}

## The fallback

Without mesh shaders, or with `Settings::meshShaders` off, the meshlet lists are culled
in compute. `classifyMeshlets` runs one thread per meshlet entry and finds its draw by
binary search over the draws' first entries, the last draw whose first entry is at most
the thread's (a draw without meshlets shares its first entry with the next, and the
search passes over it):

{{cite shaders/cull/instances.slang "if (asuint(draws[mid].aabbMin.w) <= e) lo = mid;"}}

The entry's meshlet is the draw's first meshlet plus the entry's offset within the draw:

{{cite shaders/cull/instances.slang "const MeshletGpu m = meshlets[d.info.z + e - asuint(d.aabbMin.w)];"}}

Only the meshlets of draws the phase put on a meshlet list are tested; the survivors are
compacted by the same block scan as the draws, and `scatterMeshlets` writes a
`VkDrawIndirectCommand` per survivor. Its vertex count is the meshlet's triangle corners,
its instance count 1, and the two remaining words carry what the vertex shader needs:
`firstVertex` is the draw's index shifted up by `kMeshletVertexShift`, and
`firstInstance` is the meshlet:

{{cite shaders/cull/instances.slang "meshletCommands[at + 2u] = i << kMeshletVertexShift;         // firstVertex: the draw record"}}
{{cite shaders/cull/instances.slang "meshletCommands[at + 3u] = meshlet;                          // firstInstance: the meshlet"}}

The shift is 9: a meshlet has at most 124 × 3 = 372 corners, which fit below 512, so
the vertex index splits into the draw (high bits) and the corner (low bits):

{{cite shaders/shared/layout.json "{"name": "kMeshletVertexShift", "type": "uint", "value": 9,"}}

`drawMeshletFallback` draws the lists with `vkCmdDrawIndirectCount`, and the pull vertex
shader, bound with no vertex input, recovers its draw, its corner and its meshlet:

{{cite shaders/cull/meshlets.slang "const DrawRecord d = draws[vertexIndex >> kMeshletVertexShift];"}}

and reads the corner's local index from the packed triangle, then the vertex from the
vertex arena as raw floats:

{{cite shaders/cull/meshlets.slang "const uint local = (packed >> (8u * (corner % 3u))) & 0xFFu;"}}

The fallback's draws are not indexed: three vertex invocations a triangle, each pulling
and transforming its own corner, where a mesh workgroup transforms each of the
meshlet's vertices once.

## One test, one transform

Both paths cull with `common.slang`'s `meshletVisible`, the same module the draw culler
uses. Its setup per draw puts the camera in the mesh's space, inverting the world matrix
by Cramer's rule, so the cone and sphere stay in mesh space:

{{cite shaders/cull/common.slang "v.cameraLocal = det != 0.0 ? float3(dot(d, cross(b, c)), dot(d, cross(c, a)), dot(d, cross(a, b))) / det : float3(0.0);"}}

The cone test runs only for a one-sided draw (a double-sided one shows its back faces)
whose matrix keeps winding: facing is invariant under an affine map of positive
determinant, and a mirroring matrix flips it:

{{cite shaders/cull/common.slang "v.cone = (flags & kCullCone) != 0u && oneSided && det > 0.0;"}}
{{cite shaders/cull/common.slang "if (dot(d, m.cone.xyz) >= m.cone.w * length(d) + m.sphere.w) return false;"}}

Then the sphere's box goes through `boxBounds` and, in the late phase, `occluded`, the
draw culler's own frustum and Hi-Z tests:

{{cite shaders/cull/common.slang "const Bounds b = boxBounds(v.viewProj, v.c0, v.c1, v.c2, v.c3, m.sphere.xyz - r, m.sphere.xyz + r, false);"}}

The mesh and pull shaders transform with one function, `shadeVertex`, which repeats
`gbuffer.vert`'s operations in order, and take the instance's matrices from one function,
`instanceXform`:

{{cite shaders/cull/meshlets.slang "// gbuffer.vert's transform, the same operations: the world position, the normal and tangent"}}

The normal matrix is where the match is easy to lose. `gbuffer.vert` computes it as
`transpose(inverse(mat3(inst.world)))`, and glslc compiles that `inverse` to the
`MatrixInverse` instruction of GLSL.std.450. The Slang shaders emit that same
instruction through `spirv_asm`, so the driver computes the inverse the same way on all
three paths:

{{cite shaders/core/gbuffer.vert "mat3 normalMatrix = transpose(inverse(mat3(inst.world)));"}}
{{cite shaders/cull/meshlets.slang "const float3x3 inv = spirv_asm { result:$$float3x3 = OpExtInst glsl450 MatrixInverse $w3 };"}}

The meshlet pipelines share the instance pipelines' fragment shader and state, one
culling back faces with counter-clockwise front faces and one culling none. A meshlet
draw therefore writes the same G-buffer values a whole-draw draw would, and culling a
meshlet removes only triangles that would not have reached the image.

## When a draw takes meshlets

A meshlet draw is launched as a task workgroup per 32 meshlets and a mesh workgroup per
survivor. Under `geometry: auto`, the default, a draw stays on the instance path below
`kMeshletMinTriangles` triangles, because on a draw of a few meshlets that launch costs
more than the culling saves:

{{cite shaders/shared/layout.json "{"name": "kMeshletMinTriangles", "type": "uint", "value": 4096,"}}
{{cite shaders/shared/layout.json "a few-meshlet draw costs more to launch than its culling saves"}}

The threshold was first 256. At 256, `bench100k_1080p`'s spheres, 256 triangles each, took
the meshlet path, and its `gbuffer.early` rose from 1.70 to 2.41 ms:

{{cite docs/specs/2026-10-24-renderer-upgrade-design.md "rose from 1.70 to 2.41 ms"}}

At 4,096 those draws stay instanced, and the helmet (15,452 triangles) takes meshlets:

{{cite tests/renderer/cull_parity_gpu_test.cpp "the helmet (one-sided, 15,452 triangles)"}} `geometry: meshlets` puts every
draw that has meshlets on the path, `geometry: instances` none; `render_bench`'s
`--geometry` and `--fallback` switches select them for a run:

{{cite tools/bench/render_bench.cpp "else if (a == "--fallback") geometry.fallback = true;"}}

:::why
Where the device allows it, meshlets are culled in the task shader rather than by a
compute pass that writes per-meshlet draws: the compaction is a 32-wide scan in shared
memory and the mesh workgroups are launched from it, so no per-meshlet list is written
to memory and read back by an indirect draw. The fallback writes those lists because
without mesh shaders an indirect draw is the only way to draw part of a draw. One
culling module keeps the two in step: a meshlet the task shader drops, the fallback
drops too.
:::

## Test 7

`cull_parity_gpu_test`'s test 7 holds the paths together. The occlusion scene with every
draw on meshlets renders identically through task and mesh shaders and through the
fallback, over 1, 2 and 4 frames, and with meshlet culling on and off:

{{cite tests/renderer/cull_parity_gpu_test.cpp "check(d == 0, "t07: mesh shaders = fallback, occlusion scene, " + std::to_string(frames) + " frames");"}}

A one-sided 16,000-triangle sphere, on the meshlet path under `auto`, has some of its
meshlets dropped by the cone test, with the image unchanged against the instance path:

{{cite tests/renderer/cull_parity_gpu_test.cpp "check(drawn > 0 && drawn < f.meshlets, "t07: the cone test drops the sphere's back-facing meshlets");"}}

Cornell with every draw on meshlets and the helmet room under `auto` with post give
byte-identical PNGs on the mesh-shader path, the fallback and the instance path:

{{cite tests/renderer/cull_parity_gpu_test.cpp "check(mesh == instances, std::string("t07: ") + c.name + ": the meshlet path = the instance path");"}}

and two fresh renderers build the fallback's per-meshlet lists byte for byte:

{{cite tests/renderer/cull_parity_gpu_test.cpp ""t07: twin runs: the same per-meshlet commands, byte for byte");"}}

The builder's half of test 7 is in `scene_tests`: every triangle in exactly one meshlet
within the limits, the bounds and cones containing their vertices and normals, for the
primitives, the grid, degenerate and repeated triangles, and the DamagedHelmet.

## Contracts

- The meshlet path and the instance path must render the same pixels. `shadeVertex` repeats `gbuffer.vert`; a change to one is a change to the other, and the normal matrix must stay `MatrixInverse` on both. Test 7 compares them exactly.
- The task shader and the fallback must cull with `common.slang`'s `meshletVisible`, never a copy.
- `DrawRecord.aabbMin.w` must be the prefix sum of the meshlet counts of the draws before it, in draw order; the fallback's binary search relies on it being non-decreasing.
- A meshlet has at most 372 corners, below `1 << kMeshletVertexShift`. Raising `kMeshletMaxTriangles` past 170 needs a larger shift, which halves the draws the fallback can address.
- Meshlet bounds are computed at registration; a mesh whose vertices change on the GPU afterwards keeps its old spheres and cones.
- BLAS builds use the original triangles; the meshlet arenas are raster-only.
