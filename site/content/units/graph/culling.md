---
module: graph
id: culling
title: Culling
standard: v2
---

## One frustum, one call site

This unit is three files — `render/culling.hpp`, `shaders/compute/gpu_cull.comp`,
`shaders/compute/hiz_generate.comp` — and only the header runs. The CPU frustum in
`culling.hpp` is built once per frame inside the GBuffer pass's command recording, from
the same view-projection that pass is about to rasterize with:

{{cite ohao/render/deferred/gbuffer_pass.cpp "frustum.extractFromViewProj(m_projection * m_view);"}}

That is the only construction of a `Frustum` in the tree, and `isAABBVisible` its only
test with a caller — `isSphereVisible`, `isPointVisible` and `getPlane` have none. Both
compute shaders compile anyway (`shaders/CMakeLists.txt` globs every `.comp` recursively)
but nothing in the C++ tree loads either `.spv`. Their histories differ, and that
difference is the substance of this unit: `gpu_cull.comp` never had a working dispatcher,
`hiz_generate.comp` lost one. `shaders/compute/light_culling.comp` sits in the same
limbo, compiled and unreferenced, but it is a tile-lighting design rather than part of
this one.

Hardware face culling is a separate mechanism, and the GBuffer pipeline turns it off:

{{cite ohao/render/deferred/gbuffer_pass.cpp "rasterizer.cullMode = VK_CULL_MODE_NONE;  // Disable culling — matches forward pipeline"}}

so the GBuffer pass rasterizes every back-facing triangle in the scene. The one pipeline
in the deferred path that culls faces is the cascade shadow pass, and it culls the *front*
ones:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // Front-face culling for shadow"}}

recording shadow depth on the back surface of each caster — the usual trade of acne for
peter-panning, on top of the depth bias the same pipeline enables. A single-sided mesh
that rasterizes correctly into the GBuffer can therefore vanish from the shadow map.

## Where the six planes come from

The extraction is Gribb-Hartmann, worth deriving because the code looks transposed. A
point $\mathbf{p}$ is inside the left clip boundary when its clip-space $x$ satisfies
$-w \le x$. Writing $M = P V$ with rows $\mathbf{m}_0 \ldots \mathbf{m}_3$, so that
$x_{clip} = \mathbf{m}_0 \cdot \mathbf{p}$ and $w = \mathbf{m}_3 \cdot \mathbf{p}$, that
inequality is:

$$(\mathbf{m}_3 + \mathbf{m}_0)\cdot \mathbf{p} \ \ge\ 0$$

The left plane's $(\mathbf{n}, d)$ is thus a sum of two *rows* of the view-projection —
no inverse, no decomposition, six adds. GLM stores column-major, so row 3 is
`m[0][3], m[1][3], m[2][3], m[3][3]`, which is why the indices look swapped:

{{cite ohao/render/culling.hpp "m_planes[LEFT].normal = glm::vec3(m[0][3] + m[0][0]"}}

Near and far are $\mathbf{m}_3 \pm \mathbf{m}_2$, and that is the line with a fuse in it.
$\mathbf{m}_3 + \mathbf{m}_2$ is the near plane only when clip-space $z$ runs
$-w \ldots w$ — the OpenGL convention. OHAO's camera builds its projection with plain
`glm::perspective`, and the tree never defines `GLM_FORCE_DEPTH_ZERO_TO_ONE`:

{{cite ohao/render/picking/picking_system.cpp "glm::perspective() uses OpenGL depth range [-1, 1] (no GLM_FORCE_DEPTH_ZERO_TO_ONE)"}}

so the extraction is correct as written. Define that macro — the obvious "make GLM
Vulkan-native" cleanup — and exactly one plane changes. Far survives, since $z_{clip} \le
w$ is the far boundary in both conventions; left, right, top and bottom never involved
$z$. Near does not. Under zero-to-one depth the near boundary is $z_{clip} \ge 0$, i.e.
$\mathbf{m}_2$ alone, whose zero set for `glm::perspective(fovy, aspect, n, f)` is
$z = -n$. The row *sum* the code computes is
$(0,\,0,\,-1-\tfrac{f}{f-n},\ -\tfrac{fn}{f-n})$, whose zero set is:

$$z \;=\; -\frac{fn}{2f-n} \;\approx\; -\frac{n}{2}$$

That is a plane at half the near distance — *closer* to the camera than the true one, not
behind it. Everything nearer than $n/2$ is still rejected; only the shell between $n/2$
and $n$ stops being. Nothing renders wrong, since the rasterizer still clips; the cull
just quietly gets weaker in a region nobody puts geometry in.

One perturbation reaches the matrix before extraction. `DeferredRenderer::setCameraData`
negates a single element for Vulkan's Y-down NDC:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_proj[1][1] *= -1;"}}

which is a negation of all of row 1, because every other entry of that row in a
`glm::perspective` matrix is zero. `BOTTOM` is extracted as $\mathbf{m}_3 + \mathbf{m}_1$,
the $y_{clip} \ge -w$ boundary — the bottom of the image in OpenGL's Y-up NDC, the *top*
of it in the Y-down NDC this matrix now targets. Both tests loop over all six planes
without caring which is which, so only the labels lie: `getPlane(Frustum::TOP)` hands back
the plane bounding the bottom of the image, and the only reason that is harmless is that
nothing calls it.

The matrix the GBuffer pass receives is `jitteredProj`, not `m_proj`:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_gbufferPass->setViewProjection(m_view, jitteredProj, m_prevViewProj);"}}

Usually those are the same matrix. TAA is off by default, and `getJitterOffset`
short-circuits to `glm::vec2(0.0f)` while it stays off:

{{cite ohao/render/deferred/post_processing_pipeline.hpp "bool m_taaEnabled{false};"}}

The only site in the tree that enables it is `examples/model_viewer.cpp` on the
`--deferred` path; `cornell_box`, `interactive`, `env_demo` and `turntable` never
touch it. (Four callers that disabled it explicitly have since been removed.) When jitter *is* live it is added to two
entries of the projection's third column:

{{cite ohao/render/deferred/deferred_renderer.cpp "jitteredProj[2][1] += jitter.y;"}}

shearing the cull frustum sub-pixel each frame. That is right — it then matches the
geometry actually rasterized — and worth knowing before someone "fixes" it by passing
`m_proj`. It also costs the Y-flip its symmetry: `[2][1]` is a row-1 entry, so under
active jitter `TOP` and `BOTTOM` are still interchanged but no longer exact mirrors.

## Rejecting eight corners with one dot product

The box test never touches a corner. For a plane $(\mathbf{n}, d)$ and a box with centre
$\mathbf{c}$ and half-extents $\mathbf{e}$:

$$r = e_x|n_x| + e_y|n_y| + e_z|n_z|, \qquad \text{reject iff } \ \mathbf{n}\cdot\mathbf{c} + d < -r$$

$r$ is the projected radius — the half-width of the box's shadow on the plane normal.
For an axis-aligned box it is exact: the extreme corner along $\mathbf{n}$ is the one
whose sign pattern matches $\mathbf{n}$ componentwise, so the $|n_k|$-weighted sum of
extents *is* that corner's offset.

{{cite ohao/render/culling.hpp "const float r = e.x * std::abs(plane.normal.x) +"}}

The test is invariant to plane scale — both $\mathbf{n}\cdot\mathbf{c}+d$ and $r$ scale
linearly with the plane vector — so `normalize()` is not needed by `isAABBVisible` at
all. It is needed by `isSphereVisible`, whose radius arrives in world units, and that
function has no callers. The normalization is paid for a consumer that does not exist.

The world-space box comes from Arvo's transform: transformed centre, plus half-extents
pushed through the absolute value of the upper 3×3.

{{cite ohao/render/culling.hpp "newExtents[i] += std::abs(model[j][i]) * halfExt[j];"}}

Under rotation that is conservative rather than tight — the safe direction for a
rejection test.

## What the cull costs to run

Per frame, per actor, the GBuffer pass rebuilds the local bounding box by walking every
vertex of the mesh:

{{cite ohao/render/deferred/gbuffer_pass.cpp "localAABB.min = glm::min(localAABB.min, v.position);"}}

Nothing is cached; the box is recomputed whether or not the transform moved. That is
$O(V)$ of CPU work per actor to skip that actor's draws — plural: the loop emits one call
per material range, so culling an actor saves as many calls as it has ranges, not one:

{{cite ohao/render/deferred/gbuffer_pass.cpp "vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);"}}

The scan is guarded by `if (model && !model->vertices.empty())`, so actors without a model
are never tested and always draw. At a handful of actors that is invisible; it is also why
the technique does not scale, and what `gpu_cull.comp` was drafted to replace.

The scan needs `model->vertices` to stay CPU-resident after GPU upload, and it is not the
only thing that does — picking walks the same array twice per ray, once for the world AABB
and once for the triangles:

{{cite ohao/render/picking/picking_system.cpp "AABB worldAABB = calculateWorldAABB(model->vertices, worldMatrix);"}}

so `AABB` has two users beyond the cull: that ray-box rejection, and the engine test
suite, which builds one by hand to exercise `isValid`, `center` and `size`.

{{cite tests/engine/engine_tests.cpp "AABB box{glm::vec3(-1.f), glm::vec3(1.f)};"}}

The shadow pass makes the opposite call and filters only on the component visibility flag:

{{cite ohao/render/deferred/csm_pass.cpp "if (!meshComp || !meshComp->isVisible()) continue;"}}

That is correct rather than an oversight: an object outside the camera frustum can still
cast a shadow into it, so camera-frustum culling of shadow casters deletes shadows.
Culling for CSM needs the *light's* frustum per cascade, which is not implemented.

:::why
Culling on the CPU keeps the draw loop ordinary — one `vkCmdDrawIndexed` per material
range, material indices resolved host-side into push constants. The rejected alternative,
dispatch `gpu_cull.comp` and draw indirectly, is not blocked by the render graph, which
hands each pass a raw `VkCommandBuffer` to record plain Vulkan into.

{{cite ohao/render/deferred/deferred_renderer.cpp "[&](VkCommandBuffer c) { m_gbufferPass->execute(c, frameIndex); }"}}

`vkCmdDrawIndexedIndirect` is therefore as reachable as `vkCmdDrawIndexed` — the particle
system already issues a live `vkCmdDrawIndirect` into the same frame command buffer a few
passes later. `render_pass.hpp` does define a `PassCommandBuffer` wrapper whose indirect
helper takes the draw count as a *host* value, but nothing in the tree ever constructs
one, so it constrains nothing.

{{cite ohao/render/graph/render_pass.hpp "void drawIndexedIndirect(VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {"}}

Nor is the device-side count the obstacle. `vkCmdDrawIndexedIndirectCount` would read it
directly and nothing in the tree calls that entry point, but the standard workaround needs
no extension: a fixed `maxDrawCount` of command slots, with `instanceCount = 0` written
for the culled ones.

What is missing is plumbing, not a primitive: `GBufferPass` has no indirect path. A
GPU cull shader and a foliage cull shader both existed and were deleted with the
rest of the unreferenced shader tree, because neither had any C++ to fill the
mesh-descriptor and instance buffers they read.
:::

## Contracts

- `extractFromViewProj` assumes OpenGL clip space ($-w \le z \le w$). Defining
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` anywhere in the build moves the near plane from $n$ to
  about $n/2$, weakening the cull with no error and no visual artifact. The other five
  planes are unaffected.
- The frustum must come from the *same* matrix the pass rasterizes with, jitter included.
- Plane labels (`TOP`/`BOTTOM`) are inverted relative to the screen after the Vulkan
  Y-flip. Future code reading `getPlane(index)` by name — cascade fitting, portal tests —
  inherits the swap.
- The GBuffer cull depends on `model->vertices` staying CPU-resident. Freeing host vertex
  data after upload turns every actor into an untested actor, and picking, which guards on
  the same array, stops hitting anything.
- `ohao::AABB` (`render/culling.hpp`) is constructed from `(min, max)`;
  `ohao::physics::math::AABB` (`physics/utils/physics_math.hpp`) has identical fields and
  an identical two-`vec3` signature but takes `(center, halfExtents)`. Confusing them
  compiles cleanly and yields a wrong-sized box.
