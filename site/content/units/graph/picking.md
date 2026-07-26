---
module: graph
id: picking
title: Picking
standard: v2
---

## The ray a click has to become

A mouse click is two integers. Everything downstream depends on turning that pixel
into a world-space ray that agrees, to within a pixel, with what the rasteriser
drew. `screenToWorldRay` does it the textbook way: pixel to NDC, unproject two
points through
`inverse(projection)`, perspective-divide, then push both through
`inverse(view)`. The origin is the near-plane point (not the eye), and the
direction is the normalized difference of the two.

The two z samples are −1 and +1, the OpenGL depth range:

{{cite ohao/render/picking/picking_system.cpp "glm::vec4 nearPointNDC = glm::vec4(ndcX, ndcY, -1.0f, 1.0f);"}}

That is right here for a reason that is not local to this file:
`GLM_FORCE_DEPTH_ZERO_TO_ONE` is defined nowhere — not in a header, not as a compile
definition; the engine's only mention of it is the comment two lines above — so
`glm::perspective` hands back a GL-range matrix even though the engine renders through
Vulkan. Define it and the ray does not *rotate*: under a perspective projection every
NDC point sharing an (x, y) lies on the same eye ray, so the direction survives
untouched. The origin does not. Through a zero-to-one matrix, $z_{ndc} = -1$
unprojects to eye depth $-fn/(2f-n)$, which for the camera's never-overridden defaults
of $n = 0.1$ and $f = 100$ is about $0.05$ — half the near distance, *in front* of the
near plane. The picker would keep its aim and quietly start returning hits on geometry
the rasteriser clips away.

## The Y flip that is not the Y flip

The Y story is the fragile one, and it is fragile because the two files that depend
on it use the word *flip* for different things. `screenToWorldRay` converts a
top-left-origin pixel to NDC:

{{cite ohao/render/picking/picking_system.cpp "float ndcY = 1.0f - (2.0f * screenPos.y) / viewportSize.y;"}}

The comment on that line says "Flip Y for Vulkan". It is not. That expression maps a
screen Y that grows downward to an NDC Y that grows upward — the ordinary
screen-origin conversion, byte-identical to what a pure OpenGL engine with no Vulkan
anywhere would write. It is the *correct* conversion here precisely because the matrix
inverted on the next line is a GL-convention one: `Camera` builds it with a plain
`glm::perspective` and negates nothing.

{{cite ohao/render/camera/camera.cpp "projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);"}}

So the picker stays in OpenGL conventions from pixel to world ray, and never touches
Vulkan's NDC at all. The renderer cannot afford that. `render_dispatch` hands the
deferred pipeline the same raw matrix:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);"}}

and `DeferredRenderer::setCameraData` negates the second diagonal entry on its own
private copy, at the last possible moment before it reaches a UBO:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_proj[1][1] *= -1;"}}

*That* is the Vulkan flip. The two consumers therefore do different things to the
same input and land on the same screen mapping, because each converts only what its
own destination needs — and the unflipped matrix `Camera` hands out is the shared
premise neither file states. Consolidating the negation into `Camera` is the obvious
cleanup — the same `[1][1]` negation recurs in `buffer_setup` and `diff_camera`, so it
reads like duplication asking to be hoisted. Hoist it and the deferred path stays
correct while picking silently inverts: no crash, no compile error, clicks land
mirrored about the horizon.

`Camera::getProjectionMatrix()` has three call sites but only two consumers. The
third, in `renderRTPipeline`, is a dead local — `proj` is assigned and never read,
because the RT path immediately builds its own projection instead:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);"}}

That rebuild is where picking and the path tracer part company. `aspect` there is
`float(m_width) / float(m_height)`, the frame's real shape. `Camera`'s aspect ratio is
whatever its constructor defaulted to —

{{cite ohao/render/camera/camera.hpp "float aspect = 16.0f / 10.0f,"}}

— and nothing ever resets it: `VulkanRenderer` default-constructs the camera,
`SceneFramer::applyCamera` sets position, FOV and rotation only, and `setAspectRatio`
has no callers. The deferred pipeline inherits that 16:10 projection, so the picker
agrees with a deferred frame exactly. It agrees with a path-traced one only if the
frame is 16:10, and no example is: all five render 1920×1080 or 1280×720, 16:9 frames
unprojected through a 16:10 inverse.

## Möller-Trumbore, kept for the third barycentric

The narrow phase is Möller-Trumbore, which sidesteps computing the plane
intersection first by solving directly for the ray parameter and two barycentrics:

$$\begin{bmatrix} t \\ u \\ v \end{bmatrix} = \frac{1}{(\mathbf{d} \times \mathbf{e}_2)\cdot\mathbf{e}_1}\begin{bmatrix} (\mathbf{s} \times \mathbf{e}_1)\cdot\mathbf{e}_2 \\ (\mathbf{d} \times \mathbf{e}_2)\cdot\mathbf{s} \\ (\mathbf{s} \times \mathbf{e}_1)\cdot\mathbf{d} \end{bmatrix}$$

with $\mathbf{e}_1 = v_1 - v_0$, $\mathbf{e}_2 = v_2 - v_0$, $\mathbf{s}$ the vector
from $v_0$ to the ray origin, and $\mathbf{d}$ the ray direction. The code names the
shared cross products `h` = $\mathbf{d} \times \mathbf{e}_2$ and `q` = $\mathbf{s}
\times \mathbf{e}_1$, so each of the three scalars costs one dot product; the
determinant $\mathbf{h}\cdot\mathbf{e}_1$ is reciprocated once into `f`:

{{cite ohao/render/picking/picking_system.cpp "t = f * glm::dot(edge2, q);"}}

Where OHAO's version differs from the canonical one is the output. Instead of
returning $(u, v)$ it reconstitutes the full three-weight barycentric, because the
caller interpolates a shading normal from the triangle's three vertex normals and
needs the $v_0$ weight explicitly:

{{cite ohao/render/picking/picking_system.cpp "barycentricCoords.x = 1.0f - u - v;"}}

The test is two-sided. Only a near-zero determinant is rejected — there is no
back-face branch — so a ray can pick the inside of a wall or the far side of a
closed mesh:

{{cite ohao/render/picking/picking_system.cpp "if (a > -EPSILON && a < EPSILON) {"}}

The tree's *other* Möller-Trumbore implementation, in the physics collision layer,
makes the opposite call on both counts: it takes back-face culling as a parameter
and returns only the two-component barycentric.

{{cite ohao/physics/collision/shapes/triangle_mesh_shape.hpp "if (backfaceCulling && a < math::constants::EPSILON) {"}}

Two independent copies of one algorithm, each shaped by its caller — worth knowing
before someone factors them into a shared header and has to pick one set of
defaults.

## The broad phase that walks every vertex

Before the triangle loop there is an AABB rejection test, and it is not O(1).
`calculateWorldAABB` transforms *every vertex* of the mesh into world space and
grows a box around them — on every ray, against every actor:

{{cite ohao/render/picking/picking_system.cpp "AABB worldAABB = calculateWorldAABB(model->vertices, worldMatrix);"}}

So an O(V) "fast rejection" runs ahead of an O(T) narrow phase over the same data. It
still earns its place: the narrow phase transforms three vertices per triangle, one
per index, and a closed triangle mesh has $T \approx 2V$ by Euler's formula, so
$|\text{indices}| \approx 6|\text{vertices}|$ — the box costs about a seventh of the
triangle loop it can skip, before a single Möller-Trumbore is counted. What is wrong
with this broad phase is not its price. It is what it computes.

The header this file's `Ray` already pulls in ships the cheaper construction — take
absolute values of the matrix and accumulate three products per axis to rotate a
half-extent, no vertex loop at all:

{{cite ohao/render/culling.hpp "[[nodiscard]] AABB transformed(const glm::mat4& model) const {"}}

It would not save the sweep, though, and not through laziness: `transformed` needs an
object-space bound to start from, and neither `Model` nor `MeshComponent` stores one.
The deferred GBuffer pass faces exactly that constraint and calls `transformed`
anyway — it sweeps the vertices into a local box first, initialising the bound to
±`FLT_MAX` by hand, then rotates that box:

{{cite ohao/render/deferred/gbuffer_pass.cpp "AABB worldAABB = localAABB.transformed(modelMatrix);"}}

which is the same O(V) sweep, per actor, on every frame rather than on every click.
`transformed` is usable here; it is simply not cheaper. The sweep is the price of a
mesh representation that never computed its own extents, and both consumers pay it.

The picker's copy differs from the GBuffer's in one respect, and that is the second,
quieter problem. `AABB`'s members default to zero, not to ±infinity:

{{cite ohao/render/culling.hpp "glm::vec3 min{0.0f};"}}

`calculateWorldAABB` starts from a default-constructed box and only ever calls
`expand`, so every computed bound is the union of the mesh *and the world origin*.
A prop 50 units from the origin gets a 50-unit box. The broad phase therefore only
ever produces false positives, never false negatives — correctness survives,
because the triangle test cleans up after it. Unless you turn the triangle test
off:

{{cite ohao/render/picking/picking_system.hpp "bool usePreciseMeshTesting = true;"}}

With `setUsePreciseMeshTesting(false)` the AABB hit *is* the result, so the fast
mode reports selections on empty space between an object and the origin, at a hit
point on a phantom box, with a hardcoded normal:

{{cite ohao/render/picking/picking_system.cpp "outHitNormal = glm::vec3(0.0f, 1.0f, 0.0f);"}}

The slab test itself is written in the ternary form whose float semantics are load
bearing. A ray with a zero direction component gives `invD = ±inf`, and where that
slab's plane also coincides with the ray origin, `0 * inf` makes the interval bound
NaN. Because the updates are
`t0 > tMin ? t0 : tMin` rather than `max()`, a NaN comparison is false and the
bound is left untouched:

{{cite ohao/render/picking/picking_system.cpp "tMin = t0 > tMin ? t0 : tMin;"}}

The degenerate axis contributes no constraint instead of killing the hit — the
conservative direction, consistent with the rest of the broad phase. The early-out
itself survives intact: because no NaN ever reaches either bound, `tMax < tMin`
compares clean floats and rejects disjoint slabs normally, including the parallel case
where `invD` is infinite, the origin misses the slab, and `t0` comes out `+inf`. Only
the one coincident axis stops voting.

## Nothing calls it

The honest headline: no code in the engine, the examples, or the tests constructs a
`PickingSystem` or calls `screenToWorldRay`. The only reference anywhere is an
umbrella header that re-exports it —

{{cite ohao/render/render_module.hpp "render/picking/picking_system.hpp"}}

— and `render_module.hpp` itself has no includers. The translation unit is compiled
into `ohao_renderer` regardless, because the render library globs its sources
recursively rather than listing them:

{{cite ohao/render/CMakeLists.txt "file(GLOB_RECURSE RENDERER_SOURCES"}}

This is editor infrastructure in an engine that, by design, has no editor. It
matters for how you read this page: none of the conventions above have ever been
validated against a frame on screen. They are consistent with the renderer by
inspection, not by observation — and the phantom hits the AABB-only mode returns
are exactly the kind of defect one afternoon of clicking would have caught.

:::why
Picking is CPU brute force — no BVH, no spatial index, one linear pass over the actor
map per ray — and that is a trade, not just an omission. Invisible, model-less and
transform-less actors are dropped before any per-vertex work, and an actor whose box
the ray misses never enters the triangle loop, so "every triangle" is the worst case
rather than the behaviour. What the CPU buys is a picker that needs no device, no
GPU readback and no frame of latency. Both alternatives cost more than they look. An
ID-buffer readback ties selection to the raster path and to a round trip through a
fence. Querying the RT TLAS would be nearly free, and the structure is already built
one BLAS per actor — but the instance custom index it would hand back is the global
triangle offset, not an actor id:

{{cite ohao/gpu/vulkan/rt_build.cpp "m_rtAccel->addInstance(blasIt->second, actor->getTransform()->getWorldMatrix(), globalTriOffset, instanceMask);"}}

so a hit would recover an actor only through a side table the engine does not build —
the instance-to-actor correspondence exists solely as the iteration order of the same
`unordered_map` and is persisted nowhere. Selection would also then require the
ray-tracing extensions, and would go stale between TLAS rebuilds while an actor is
being dragged. The brute-force picker is always correct and never stale.
:::

:::key
The picker and the deferred renderer start from the same unflipped, GL-convention
`Camera::getProjectionMatrix()`. The picker stays in that convention end to end and
converts only the screen origin; the renderer converts the matrix to Vulkan's on a
private copy it never shares. The matrix staying unflipped is the contract, and
neither file says so.
:::

## Contracts

- `screenToWorldRay` unprojects the camera matrix in GL convention and converts only the screen origin; `DeferredRenderer::setCameraData` converts its private copy to Vulkan convention. `Camera` must keep handing out the unflipped matrix. Hoisting the negation into `Camera` to remove the apparent duplication inverts picking vertically with no other symptom.
- The ray origin is on the near plane, so geometry between the eye and the near plane is unpickable. This matches what the rasteriser clips away — and it holds only for the GL depth range; a zero-to-one matrix would slide the origin to roughly half the near distance, making the band between there and the near plane pickable.
- The picker inherits `Camera`'s aspect ratio, which is the constructor default and is never set from the render resolution. Deferred frames use the same matrix, so picking agrees with them exactly; the path tracer builds its own projection from the frame's real aspect, so on any non-16:10 frame a click does not correspond to the path-traced image.
- The AABB broad phase is conservative-only (its boxes always contain the world origin), so it is safe *only* while `usePreciseMeshTesting` is true. The fast path is not a coarser approximation of the precise path; it returns hits the precise path rejects outright.
- `pickActor` breaks exact-distance ties by first-encountered — the comparison against a `FLT_MAX`-initialised `PickResult::distance` is strict `<` — while iterating `Scene`'s `unordered_map`. That order is unspecified and moves when the actor set, the insertion order or the bucket count changes; it is not run-varying, since the key type is `uint64_t` and hashing it is the identity. Two coplanar surfaces at identical `t` therefore resolve the same way on every run of one binary, and differently after an unrelated scene edit.
{{cite ohao/scene/scene.hpp "using ActorMap = std::unordered_map<std::uint64_t, Actor::Ptr>;"}}
- `testActorIntersection` requires a `MeshComponent` that reports visible and a non-empty vertex list, so hidden actors, lights, and cameras are never pickable — there is no proxy geometry for non-mesh actors.
