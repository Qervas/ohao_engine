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
`--deferred` path; four others — the three dense inverse-rendering fits and the
differentiable forward pass — disable it explicitly, and `cornell_box`, `interactive`,
`env_demo` and `turntable` never touch it. When jitter *is* live it is added to two
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
for the culled ones — what the foliage cull shader under `shaders/_disabled/` already
does.

{{cite shaders/_disabled/foliage/foliage_cull.comp "commands[i].instanceCount = 1;"}}

What is missing is plumbing, not a primitive: `GBufferPass` has no indirect path, and no
code in the build fills the mesh-descriptor and instance buffers `gpu_cull.comp` reads.
:::

## The GPU cull that never dispatches

`gpu_cull.comp` is a 64-wide pass over mesh descriptors: optional distance rejection,
then a bounding-sphere test whose world radius is scaled by the largest of the three
model-matrix column lengths — conservative, and visibly too large for a non-uniformly
stretched mesh.

{{cite shaders/compute/gpu_cull.comp "if (!sphereInFrustum(worldCenter, worldRadius)) {"}}

The stricter `aabbInFrustum` in the same file, which picks the p-vertex per plane instead
of projecting a radius, has no caller anywhere. Occlusion is a declared flag bit with a
commented-out body, and that comment is the only mention of Hi-Z in the file — an
intention, not a wiring:

{{cite shaders/compute/gpu_cull.comp "//     visible = !isOccluded(worldCenter, worldRadius);"}}

The dispatcher this shader was written for did exist, as `GpuCullPass` in
`ohao/render/deferred/_disabled/indirect_draw_buffer.cpp`, deleted in commit `e8eb9d1` —
and it never ran. `createPipeline()` built a pipeline layout, logged "shader loading
deferred" and returned without creating a pipeline, so `execute()` bailed out on a null
`m_cullPipeline`. This shader is a staged design, not a regression.

Three things must change before it can dispatch, none of them obvious from a skim:

- The push-constant block is 192 bytes under std430 (`mat4` + `vec4` + 6×`vec4` + four
  `uint`s); Vulkan guarantees only 128. The `mat4 viewProj` is never read — the frustum
  arrives pre-extracted in `frustumPlanes` — so deleting those 64 bytes puts the block
  exactly on the guaranteed floor.
- Each surviving mesh writes `firstInstance = meshIdx` so the vertex stage can index
  instance data. A non-zero `firstInstance` in an *indirect* draw requires
  `drawIndirectFirstInstance`; the device is created with `multiDrawIndirect` enabled and
  that feature absent. {{cite ohao/gpu/vulkan/device_setup.cpp "deviceFeatures2.features.multiDrawIndirect = VK_TRUE;"}}
- Commands are appended with an atomic bump, so surviving draws land in an order that
  changes frame to frame — harmless for opaque depth-tested geometry, flicker for any
  blended pass. The counter must also be host-zeroed before every dispatch, since the
  shader only increments. {{cite shaders/compute/gpu_cull.comp "uint drawIdx = atomicAdd(drawCount, 1);"}}

Instance data is indexed as `instances[meshIdx]`, baking in a 1:1 mesh-to-instance
mapping even though `DrawInstance` carries a `meshIndex` field for the general case.

## The depth pyramid was built for a different reader

`hiz_generate.comp` is not the occlusion companion to `gpu_cull.comp`; its own header says
otherwise:

{{cite shaders/compute/hiz_generate.comp "// Generates depth pyramid for efficient ray marching"}}

and the only shader in the tree that has ever sampled it is screen-space reflections:

{{cite shaders/_disabled/ssr.comp "layout(set = 0, binding = 3) uniform sampler2D hizBuffer;"}}

which starts at the coarsest level, drops a mip and steps back when a hit looks possible,
and climbs one when the ray is clear.

{{cite shaders/_disabled/ssr.comp "int mipLevel = int(params.hizMipLevels) - 1;"}}

That reader was not a sketch. Until commit `e8eb9d1` ("refactor: Delete dead passes"),
`SSRPass` built a compute pipeline from `compute_hiz_generate.comp.spv`, dispatched it
once per mip at the top of its `execute`, and `PostProcessingPipeline::execute` called
that every frame SSR was on. The pyramid ran; the code that ran it was deleted. Today's
SSR pass is a different shader, marching the full-resolution depth buffer directly with no
mip chain and no Hi-Z binding:

{{cite shaders/postprocess/ssr.comp "layout(set = 0, binding = 2) uniform sampler2D depthBuffer;      // scene depth"}}

So the pyramid has no reader by regression, not by design — and which reader it lost
decides what its reduction should have been. Each invocation takes the maximum of a 2×2
block into an `r32f` storage image, 8×8 per workgroup:

{{cite shaders/compute/hiz_generate.comp "float maxDepth = max(max(d0, d1), max(d2, d3));"}}

The GBuffer depth test is `VK_COMPARE_OP_LESS`, so smaller is nearer and the maximum over
a footprint is the *furthest* surface in it.

{{cite ohao/render/deferred/gbuffer_pass.cpp "depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;"}}

For occlusion culling that is the conservative direction: a max can only make the occluder
look further away, so such a test errs toward "visible". For a hierarchical ray march it
is the wrong one. The march treats "ray depth exceeds the stored depth" as a possible hit
and refines; the else branch calls the block clear and takes a bigger step at a coarser
mip.

{{cite shaders/_disabled/ssr.comp "if (currentPos.z > sceneDepth) {"}}

Being nearer than a block's *furthest* surface says nothing about the surfaces in front of
it, so that coarse step can jump over a real intersection. The minimum is the safe
reduction here — nearer than the block minimum, nothing in the footprint can be hit. The
pyramid and its consumer disagree about which extreme is conservative, and the
disagreement is invisible because the consumer is disabled.

The reduction is also a strict 2×2 with no odd-size fallback:

{{cite shaders/compute/hiz_generate.comp "ivec2 srcCoord = dstCoord * 2;"}}

Real pyramids gather 3×3 whenever a source dimension is odd, or the last row or column is
never sampled and the level below summarizes a smaller footprint than it claims. A
1080-tall chain halves to 540, 270, 135 and on to 67, 33, 16, 8, 4, 2, 1; the three odd
sources in it — 135, 67 and 33 — each drop their bottom row, and the even ones drop
nothing. The two sampling branches differ only in how the level is named: mip 0 uses
`texture()`, later mips `textureLod(..., srcMip)`. A compute dispatch has no fragment-quad
derivatives for an implicit LOD to come from, and glslang lowers the implicit form here to
an explicit `Lod 0` anyway — so the first branch would be clearer, and compile the same,
as `textureLod(..., 0.0)`.

:::key
The only culling OHAO performs per frame is one CPU frustum test per actor in the GBuffer
pass — and that pass then rasterizes every back face, because its pipeline sets
`VK_CULL_MODE_NONE`. `gpu_cull.comp` has never been dispatched. `hiz_generate.comp` has:
its dispatcher went out with the old SSR pass, and the SSR pass that replaced it does not
read a pyramid.
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
