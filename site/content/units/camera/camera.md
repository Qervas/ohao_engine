---
module: camera
id: camera
title: Camera
standard: v2
---

## The datum is −90°

`Camera` stores a world position and two Euler angles; everything else — basis,
view matrix, projection matrix — is derived. The forward vector uses the spherical
convention in which yaw is measured from the **+X** axis inside the XZ plane:

$$\mathbf{f} = (\cos\psi\cos\theta,\; \sin\theta,\; \sin\psi\cos\theta)$$

with $\psi$ yaw and $\theta$ pitch, both stored in degrees and converted per call by
`glm::radians`.

{{cite ohao/render/camera/camera.cpp "newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));"}}

Measuring from +X means yaw = 0 looks down +X, not down −Z. Every camera in the tree
that wants the conventional "into the screen" direction therefore carries a literal
−90: the constructor's own default, the renderer's initial pose, `SceneFramer`, and
the three `CameraView`s the inverse-rendering scene builder names `front`. It is a
datum rather than a constant — the builder's other shipped views are offsets from it
(−110 and −70 in `buildCornell`, −48 and −128 in `buildStudio`), and domain
randomisation draws yaw as a ±58° excursion around the same origin:

{{cite ohao/inverse/scene_builder.hpp@223ff7f "const float yaw = -90.0f + n11() * 58.0f;"}}

Right and up are re-orthogonalised from a fixed world up of (0, 1, 0), so roll is not
representable at all.

## Why pitch stops at 89 and not 90

The clamp in `rotate` is not a taste decision about neck anatomy.

{{cite ohao/render/camera/camera.cpp "pitch = std::clamp(pitch, -89.0f, 89.0f);"}}

At $|\theta| = 90°$ the forward vector is parallel to world up — $(0, 1, 0)$ at
$+90°$, $(0, -1, 0)$ at $-90°$ — so `cross(front, worldUp)` degenerates. In exact
arithmetic it is the zero vector and `normalize` divides by zero; in `float` it is a
vector of length $\approx 4.4\times10^{-8}$, the residue of `cos` applied to a rounded
$\pi/2$, whose *direction* is pure rounding noise. Either way `right` carries no
information about the camera, `up` inherits that, and `glm::lookAt` writes a rotation
block that no longer describes anything — including into `inverse(view)`, which is
what the path tracer's raygen actually reads. One
degree of margin keeps the cross product's length at $\sin 1° \approx 0.0175$, small
but finite. All three writers of `pitch` clamp it — `setRotation`, `rotate` and
`focusOnPoint`; yaw is clamped in none of them and never wrapped, so it accumulates
without bound across a session.

Every mutator ends in `updateVectors()`, which rebuilds the basis *and* both matrices
eagerly.

:::why
There is no dirty flag: `setPosition` recomputes a perspective matrix that cannot
have changed, and the interactive viewer calls three setters per frame. The rejected
alternative — lazy recompute behind a dirty bit — would force `getViewMatrix() const`
to be mutable-cached and would open the classic stale-matrix window where a setter
runs after the frame's matrices were already read. A few redundant
`glm::perspective` calls are a rounding error next to a path-traced frame; a stale
view matrix is a wrong image.
:::

## The basis accessors nobody calls

`getFront`, `getRight`, `getUp`, `getPitch` and `getYaw` have zero callers anywhere
in `ohao/`, `examples/` or `tests/`. That is not evidence the basis is unused — it is
evidence that it round-trips through the view matrix. `glm::lookAt` packs the three
axes into the rotation block:

{{cite ohao/render/camera/camera.cpp "viewMatrix = glm::lookAt(position, position + front, up);"}}

and the path tracer's raygen pulls them straight back out of the inverse, column by
column, to build the pinhole ray:

{{cite shaders/rt/pt_raygen.rgen "vec3 fwd = -vec3(pc.invView[2]);"}}

So the accessors are deletable; the vectors they expose are not.

## Three projections from one camera

`updateVectors` builds a projection matrix from the camera's own fov, aspect, near
and far:

{{cite ohao/render/camera/camera.cpp "projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);"}}

Exactly one of the three pipelines uses it. The deferred renderer takes it verbatim
and applies the Vulkan Y-flip downstream, in `setCameraData`, not in the camera:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_proj[1][1] *= -1;"}}

The path tracer throws the matrix away and rebuilds one from the camera's *fov only*,
with the framebuffer's true aspect and a far plane of 1000, unflipped because the
raygen negates Y itself when forming the ray:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);"}}

The forward path does not even read the fov — both overloads of `updateUniformBuffer`
hard-code 45° and a 0.1–100 range into the `CameraUniformBuffer` that `forward.vert`
samples at binding 0:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "void VulkanRenderer::updateUniformBuffer() {"}}

`cornell_box`'s `camera.setFov(38.0f)` is therefore honoured by the path tracer and
by the deferred rasteriser, and silently ignored by forward rendering.

## The aspect ratio that is never set

`setAspectRatio` has no callers. Neither do `setPerspectiveProjection`,
`setOrthographicProjection`, `setProjectionType` or `focusOnPoint` — which means the
orthographic branch of `updateVectors` is unreachable in shipping code, and near/far
are frozen at their constructor defaults for the process lifetime. The renderer
default-constructs the camera, and the constructor's default argument wins over the
member initialiser, which disagrees with it — the class declares two different
default aspect ratios, 16:9 on the field and 16:10 in the signature, and 1.6 is the
one that ships:

{{cite ohao/render/camera/camera.hpp "float aspect = 16.0f / 10.0f,"}}

The path tracer never noticed because its raygen does not trust the projection's
aspect at all: it derives aspect from the dispatch dimensions and extracts only the
vertical field of view from the inverse projection. Write $P_{ij}$ for row $i$,
column $j$ in 1-based notation. `glm::perspective` puts the aspect-bearing term
$1/(a\tan(\phi/2))$ at $P_{11}$ — the entry this whole section is about — and the
*pure* half-angle term at $P_{22}$, where $\phi$ is the vertical fov and $a$ the
aspect. The leading $2\times2$ block is diagonal, so inverting the matrix inverts
that element in place:

$$P_{22} = \frac{1}{\tan(\phi/2)} \quad\Longrightarrow\quad \left(P^{-1}\right)_{22} = \tan(\phi/2)$$

GLSL addresses that same element as the 0-indexed `[1][1]` of a column-major `mat4`,
and that is the one the raygen reads. $P_{11}$, and the aspect baked into it, is never
touched:

{{cite shaders/rt/pt_raygen.rgen "float tanFovY = abs(pc.invProj[1][1]);"}}

The deferred rasteriser has no such escape hatch. It renders with a matrix built for
1.6 regardless of the target's real ratio, so an offline deferred frame at 1:1 is
horizontally compressed by that factor. `getProjectionMatrix` has two other readers
and neither renders: `renderRTPipeline` assigns it to a local it then never uses, and
`PickingSystem::screenToWorldRay` unprojects screen rays with it — inheriting the same
1.6 — but no `PickingSystem` is ever constructed, because `render_module.hpp`, the
only file that includes it, is itself included by nothing.

## The near plane is twice what it says

`GLM_FORCE_DEPTH_ZERO_TO_ONE` is never defined in the repository — its one appearance
anywhere in the tree is a comment noting the absence — so `glm::perspective` and
`glm::ortho` hand back OpenGL-convention matrices that map the frustum to
$z_{ndc} \in [-1, 1]$. Vulkan clips against $0 \le z_c \le w_c$. The surface where
$z_c = 0$ sits at view depth

$$z_{\text{eye}} = \frac{2fn}{f+n}$$

the harmonic mean of the near and far distances — with the camera's permanent
$n = 0.1$, $f = 100$ that is 0.1998. The GBuffer pipeline runs with depth clamping
off, so that slab is clipped rather than flattened:

{{cite ohao/render/deferred/gbuffer_pass.cpp "rasterizer.depthClampEnable = VK_FALSE;"}}

The deferred pipeline's effective near plane is therefore twice its nominal one, and
the depth buffer only ever receives the outer half of GL's depth mapping. Forward
rendering inherits the same defect from its own matrix — `updateUniformBuffer` builds
a raw `glm::perspective` and applies only the Y-flip. Two passes dodge it, by
different means. CSM is the single raster pipeline in the engine that enables depth
clamp, so its slab is flattened rather than clipped:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.depthClampEnable = VK_TRUE; // Clamp depth to [0, 1]"}}

The forward path's own shadow pipeline, `createShadowPipeline`, leaves
`depthClampEnable` at `VK_FALSE` and escapes anyway, because the light-space matrix it
consumes is rescaled from GL NDC to Vulkan's $[0, 1]$ by hand before upload:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "lightSpaceMatrix[3][2] += 0.5f;  // Add bias (multiplied by w=1)"}}

"The shadow pass" is two pipelines here, reaching safety by different routes; the
phrase is only meaningful once you say which.

Worse, the scalars travelling beside the matrix disagree with it. `renderDeferred`
announces a 0.1–1000 range while handing over a matrix that ends at 100:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);"}}

and CSM derives its cascade splits from those scalars:

{{cite ohao/render/deferred/csm_pass.cpp "float ratio = m_farPlane / m_nearPlane;"}}

The splits are not logarithmic; they are a per-cascade lerp between a logarithmic and
a linear schedule, weighted by `m_splitLambda`, which defaults to 0.95:

{{cite ohao/render/deferred/csm_pass.cpp "float d = m_splitLambda * log + (1.0f - m_splitLambda) * linear;"}}

Either schedule is anchored at the same 1000, so the outermost cascade is fitted to a
frustum depth the projection cannot draw.

## A second camera model, with the opposite yaw

The differentiable renderer does not use this class. `DiffCamera` re-derives its own
forward vector with a different convention:

{{cite ohao/render/diff/diff_camera.hpp@223ff7f "const glm::vec3 forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),"}}

that is $\mathbf{f}' = (\sin\psi\cos\theta,\; \sin\theta,\; -\cos\psi\cos\theta)$.
Matching it to `Camera`'s $(\cos\psi\cos\theta,\; \sin\theta,\; \sin\psi\cos\theta)$
requires $\sin\psi' = \cos\psi$ *and* $-\cos\psi' = \sin\psi$ simultaneously, whose
only solution is $\psi' = \psi + 90°$. `DiffCamera` at yaw $\psi$ is `Camera` at yaw
$\psi - 90°$: an exact quarter turn about world up.

Nothing applies that offset. Both are driven from the same `CameraView` list and both
read the same field — `InverseScene::applyCamera` hands `v.yawDeg` to
`Camera::setRotation`,

{{cite ohao/inverse/scene_builder.hpp@223ff7f "cam.setRotation(v.pitchDeg, v.yawDeg);"}}

and `DiffSession::setupFromInverse` copies it across verbatim:

{{cite ohao/render/diff/diff_session.hpp@223ff7f "c.yawDeg = v.yawDeg;"}}

With $\psi$ and $\theta$ shared, the two forwards satisfy

$$\mathbf{f}\cdot\mathbf{f}' = \sin^2\theta$$

so at level pitch they are exactly perpendicular *for every yaw*, and inside the ±89°
pitch clamp — where $\sin^2\theta < 1$ always — they never coincide at any yaw or
pitch the engine can reach. At the standard "front" view, yaw −90° and pitch 0,
`Camera` looks down −Z and `DiffCamera` looks down −X. The header comment claiming it
"matches studio yaw/pitch convention" is not what the arithmetic says. Anything that
compares a Vulkan render against a `DiffCamera`-projected quantity — the ground-plane
albedo map, in particular — is comparing two viewpoints a quarter turn apart, at every
view the builder ships.

:::key
`Camera` is a pose, not a lens. Its view matrix is what every Vulkan pipeline reads —
forward, deferred and path tracer alike — while `DiffCamera` builds its own from a yaw
convention 90° away. Only the deferred pipeline ever renders with its *projection*
matrix. Fov, aspect, near and far do not mean one thing engine-wide, so "the camera's
near plane" is an ambiguous phrase in this codebase — always name the pipeline.
:::

## The 240 bytes the forward shader never agreed to

`getViewProjectionMatrix` has exactly one call site, and it is a live layout bug.
`renderSceneObjects` fills an `ObjectPushConstants` — `mat4 model`, `mat4 viewProj`,
`mat4 prevMVP`, then three `vec4`s of material, 240 bytes in all — and pushes the
whole thing through `m_pipelineLayout`:

{{cite ohao/gpu/vulkan/scene_upload.cpp "vkCmdPushConstants(cmd, m_pipelineLayout,"}}

That layout belongs to the forward pipeline, built from `core_forward.vert.spv` and
`core_forward.frag.spv`, and those shaders declare a 96-byte block instead:

{{cite shaders/core/forward.frag "float albedoTexIdx;   // uint32 packed as float"}}

`mat4 model` occupies bytes 0–63 on both sides; after that they diverge. std430 puts
`baseColor` at 64, `metallic` at 76, `roughness` at 80, `ao` at 84, and the two
texture indices at 88 and 92 — exactly the sixteen bytes of `viewProj[0]` followed by
the sixteen of `viewProj[1]`. The view-projection matrix therefore *does* reach the
shader: its first column is read as base colour and metallic, its second as roughness,
ao, and two `floatBitsToUint`-decoded bindless texture indices. The fields
`renderSceneObjects` actually meant as material — `materialParams` at byte 192,
`albedoColor` at 208 — sit past the end of what either shader declares and are never
read.

The shadow path escapes: `renderShadowPass` pushes the same struct through
`m_shadowPipelineLayout`, but `shadow_depth.vert` reads only `pc.model`.

## Contracts

- Pitch must stay strictly inside ±90° or `cross(front, worldUp)` degenerates and the view matrix's rotation block stops describing the camera. All three writers of `pitch` clamp it — `setRotation`, `rotate`, `focusOnPoint` — and a "cleanup" that widens any of them to ±90 exactly will produce a frame with a randomly rolled camera, not a gimbal-locked one.
- Nothing calls `setAspectRatio`, so a resize changes the render target without changing the projection. Adding a resize hook is the fix; until then the deferred image's horizontal fov is that of a 16:10 frame.
- The Y-flip is *not* in this class. Consumers that build a Vulkan clip-space matrix must apply `proj[1][1] *= -1` themselves (deferred does, `DiffCamera` does, the path tracer instead negates Y when forming the ray). Adding the flip inside `Camera` would double-flip two of the three.
- `getViewProjectionMatrix()` has a single call site, and the forward shaders do not declare the `viewProj` field it writes — they read view and proj from the uniform buffer at binding 0. The bytes are still consumed, as material data. Fixing the layout on one side only (deleting `viewProj` from the struct, or adding it to the shaders) changes what the forward pass renders; both sides must move together.
- Near and far are immutable after construction (0.1 and 100), because the only two mutators that touch them have no callers. Any geometry beyond 100 units is outside the deferred frustum regardless of what the CSM cascade range claims.
