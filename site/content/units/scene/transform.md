---
module: scene
id: transform
title: Transform math
standard: v2
---

## Two transform classes behind one accessor name

`ohao/scene/transform.hpp` declares a complete TRS transform — position, unit
quaternion, per-axis scale, cached local and world matrices, a dirty flag. Grep
the tree for anything that calls one of its methods and you get nothing. The
class is instantiated exactly once per entity, as a private member of the
`SceneObject` base every `Actor` inherits from:

{{cite ohao/scene/scene_object.hpp "Transform transform;"}}

and then immediately shadowed. `SceneObject::getTransform()` returns
`Transform&`; `Actor::getTransform()` returns a pointer to a different class
entirely, the `TransformComponent` added in the `Actor` constructor:

{{cite ohao/scene/actor/actor.hpp "TransformComponent* getTransform() const;"}}

Every consumer in the engine — the GBuffer pass, the CSM pass, TLAS instance
build, the light SSBO upload, picking — reaches the transform through an `Actor`,
so every consumer gets the component. The inherited `Transform` is 172 bytes of
identity matrix per actor that no code ever reads (measured with `sizeof` under
GCC and this tree's default GLM configuration, where `mat4` is 64 bytes and
`vec3` is unpadded).

What is *not* dead is the math inside it. The T·R·S product in `transform.cpp`
is built factor-for-factor identically in `TransformComponent::updateLocalMatrix`,
so the file you are reading is a stale copy of live code rather than an unused
idea.

## The order of the product

Both classes build the local matrix as the same ordered product:

$$M_{\text{local}} = T(\mathbf{t})\; R(q)\; S(\mathbf{s})$$

where $\mathbf{t}$ is the local position, $q$ the local unit quaternion expanded
to a rotation matrix by `glm::toMat4`, and $\mathbf{s}$ the per-axis scale. GLM
uses the column-vector convention, so a point is transformed as
$M\mathbf{v}$ and the rightmost factor applies first: scale acts along the
object's own axes, then rotation orients it, then translation places it. That is
the only ordering under which an artist's "make this twice as wide" edit stays
axis-aligned to the model regardless of how the object is rotated.

{{cite ohao/scene/component/transform_component.cpp "localMatrix = translationMatrix * rotationMatrix * scaleMatrix;"}}

The cost of this representation is that it cannot express shear. A non-uniform
scale above a rotated child in a hierarchy produces a composed world matrix that
is genuinely sheared, and no TRS triple reproduces it. `getWorldRotation()` and
`getWorldScale()` paper over that by calling `glm::decompose` and discarding the
skew and perspective out-parameters:

{{cite ohao/scene/component/transform_component.cpp "glm::quat TransformComponent::getWorldRotation() const {"}}

so on such a chain that pair returns values which do not rebuild the matrix they
came from. `getWorldPosition()` is exempt: it reads the world matrix's
translation column, `getWorldMatrix()[3]`, and decomposes nothing. All three
take those branches only when a parent exists, so in the shipping tree none of
them fires, for the reason in the next-but-one section.

## A dirty flag that clears on one path only

In the legacy `Transform`, `getLocalMatrix()` is the only accessor that consults
the flag: it rebuilds, caches into both `localMatrix` and `worldMatrix`, and
clears `dirty`.

{{cite ohao/scene/transform.cpp "glm::mat4 Transform::getLocalMatrix() const {"}}

Every world accessor instead goes through a private helper that rebuilds the
full product unconditionally and never looks at, or clears, the flag:

{{cite ohao/scene/transform.cpp "void Transform::updateWorldMatrix() const {"}}

So `getWorldMatrix()` costs a fresh quaternion-to-matrix expansion and two
matrix multiplies on *every* call, and leaves `dirty` exactly as it found it —
`updateWorldMatrix` neither reads the flag nor writes it. Move the object and
then read world before local, and the identical product is built twice: once by
`updateWorldMatrix()`, which cannot clear a flag it never touches, and again by
`getLocalMatrix()`, which still sees the setter's `dirty`. The public
`isDirty()` accessor has no callers at all. This is the concrete reason the
class was superseded rather than extended.

`TransformComponent` fixes it with two flags — `dirty` for the local matrix,
`worldDirty` for the composed one. Both are raised together by the single
`setDirty()` every mutator funnels through, and each is cleared by its own
rebuild helper. `setDirty()` also, crucially, pushes that invalidation
*downward*:

{{cite ohao/scene/component/transform_component.cpp "child->setDirty();"}}

That recursion is the whole reason the component keeps a `children` vector. A
flat transform has no way to know that moving it invalidated somebody else's
cached world matrix.

One sharp edge survives: `TransformComponent::getLocalMatrix()` is `const` and
returns the cached matrix without consulting `dirty`, so it can hand back a
matrix that predates the last `setPosition`. It has zero callers today, which is
the only reason nobody has been bitten.

:::why
Invalidation is pushed at edit time down raw `TransformComponent*` child
pointers, rather than the usual alternative — a flat, depth-sorted array of
transforms swept once per frame before rendering. The push scheme costs nothing
when nothing moves, which suits an offline renderer that re-renders a static
scene. What it buys instead is a lifetime problem: the pointers in `children`
are unowned, and nothing unwires them.
:::

## The hierarchy nothing uses

When a parent exists, the world matrix is the recursive product
`parent->getWorldMatrix() * localMatrix`, with every level independently cached,
so a chain of depth $d$ costs $d$ matrix multiplies only on the frame its root
moves:

{{cite ohao/scene/component/transform_component.cpp "worldMatrix = parent->getWorldMatrix() * localMatrix;"}}

That branch never executes. Nothing outside `actor.cpp`'s own plumbing calls
`setParent` or `addChild` on an actor or a transform, and no importer hands the
scene a hierarchy to parent in the first place: `Model` is a flat vertex and
index buffer with per-material side arrays and no node list of any kind. The
native glTF loader, which `env_demo` and `turntable` call directly, walks the
mesh array and never reads a node — the string `nodes` does not occur anywhere
in the file:

{{cite ohao/scene/asset/model_gltf.cpp "for (const auto& mesh : gltfModel.meshes) {"}}

The ufbx path is the one importer that does read node data, and it disposes of
it rather than preserving it: each mesh's `geometry_to_node` is baked into
vertex positions and normals at load time, so what reaches an actor is again one
flat buffer with nothing above it.

{{cite ohao/scene/asset/model_fbx.cpp "v.position = glm::vec3(geoTransform * glm::vec4(v.position, 1.0f));"}}

So in every shipping example, `parent == nullptr`, world *is* local, and the
"local == world for now" comment in the dead class is an accurate description of
the live one.

Two contracts are waiting for whoever turns hierarchies on. First,
`Actor::detachFromParent()` routes through `removeChild`, which nulls the actor
back-pointer directly instead of going through `setParent`:

{{cite ohao/scene/actor/actor.cpp "child->parent = nullptr;"}}

`setParent` is the only path that also calls `transform->setParent(...)`, so
detaching leaves the child's transform still composing against its ex-parent.
Second, `~Actor()` detaches through that same path and then destroys the
component, whose destructor does not unregister it from the parent's `children`
vector:

{{cite ohao/scene/component/transform_component.hpp "~TransformComponent() override = default;"}}

The next `setDirty()` on the surviving parent walks a freed pointer.

## Four consumers, four conventions

A world matrix leaves the transform system in four different shapes.

The raster path passes it straight through as a column-major `glm::mat4` push
constant. The ray-tracing path cannot: `VkAccelerationStructureInstanceKHR`
wants a `VkTransformMatrixKHR`, which is a **row-major `float[3][4]`**. The TLAS
builder transposes and memcpys the first 48 bytes:

{{cite ohao/render/rt/rt_acceleration_structure.cpp "glm::mat4 t = glm::transpose(inst.transform);"}}

This works because after transposition the first three columns in memory are the
original matrix's first three rows, which is exactly the 3×4 the API asks for —
the discarded fourth column is the affine $(0,0,0,1)$ bottom row. Delete the
transpose as a "redundant copy" and the memcpy instead grabs the first three
*columns*, so every instance in the TLAS gets a transposed basis and zero
translation: geometry collapses to the origin with its rotation inverted.

The normal matrix is not uploaded at all. It is recomputed per vertex on the GPU:

{{cite shaders/core/gbuffer.vert "mat3 normalMatrix = transpose(inverse(mat3(pc.model)));"}}

The comment above that line claims uniform scaling, but the code computes the
general inverse-transpose, which is correct for non-uniform scale too — the code
is stricter than its comment. The price is a 3×3 inverse per vertex per pass
rather than one on the CPU per object.

Culling uses Arvo's transform rather than transforming eight corners. With $A$
the upper-left 3×3 of the world matrix, $\mathbf{t}$ its translation column, and
$\mathbf{c}, \mathbf{e}$ the local box centre and half-extent:

$$\mathbf{c}' = A\mathbf{c} + \mathbf{t}, \qquad e'_i = \sum_{j=0}^{2} \lvert A_{ij}\rvert\, e_j$$

The absolute-value sum is exactly the tightest axis-aligned box around the
transformed box. `AABB::transformed` pays one full point transform for the
centre plus nine absolute-value multiply-adds for the extent, against eight
point transforms for the corner-by-corner version:

{{cite ohao/render/culling.hpp "newExtents[i] += std::abs(model[j][i]) * halfExt[j];"}}

The `model[j][i]` indexing looks transposed only because GLM subscripts column
first; column $j$, row $i$ is $A_{ij}$. The saving is then thrown away one level
up, where the *local* box is rebuilt from every vertex of the mesh, every frame,
before being transformed:

{{cite ohao/render/deferred/gbuffer_pass.cpp "localAABB.min = glm::min(localAABB.min, v.position);"}}

## Motion vectors with no object history

The GBuffer pass builds the previous-frame clip matrix from the *previous*
view-projection and the *current* model matrix:

{{cite ohao/render/deferred/gbuffer_pass.cpp "const glm::mat4 prevMVP = m_prevViewProj * modelMatrix;"}}

Only the camera is double-buffered, and the vertex shader reprojects the
object's *current* position through that product. An object's own displacement
therefore cancels out of the difference: what lands in the velocity buffer is
camera parallax computed on a point the object has already left. Hold the camera
still and the residual is just the TAA sub-pixel jitter, which the current
frame's projection carries and `m_prevViewProj` does not. Move it and the buffer
reports a confident wrong vector rather than a conspicuous zero.

{{cite shaders/core/gbuffer.frag "outVelocity = (currentNDC - prevNDC) * 0.5;"}}

Actor transforms do move in this tree. The inverse-rendering dataset exporter
jitters three light actors' positions and re-yaws the hero between samples, and
its caller has to throw away the entire GPU binding because of it:

{{cite ohao/inverse/export_dataset.hpp "session.bound = false; // transforms changed — full rebind"}}

That is an edit *between* renders, though — each sample resets accumulation and
runs its own frame batch at fixed transforms — and no shipping example moves an
actor at all: all five set their transforms during scene build and thereafter
only fly the camera. So the wrong velocity stays invisible until someone
animates a transform, and the fix is in the transform system (store last frame's
world matrix per actor), not in the shader.

:::key
The file this page is nominally about does not run. `TransformComponent` is the
live implementation, it never composes a parent because nothing parents
anything, and its T·R·S product is rebuilt only when a setter runs — which in
every shipping example is once per actor at scene build, before the render loop
opens. Per frame the transform system does no arithmetic at all: each consumer
gets back a cached world matrix that is a copy of its local.
:::
