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
−90: the constructor's own default, the renderer's initial pose, and `SceneFramer`.
It is a datum rather than a constant.

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
have changed, and every host calls `setPosition`, `setPerspectiveProjection` and
`setRotation` on every frame it renders a world with a camera — `ohao_game` included,
whose `FlyController` moves the camera entity's `Transform`, not the `Camera`:

{{cite ohao/runtime/scene_view_apply.hpp "cam.setRotation(c.pitchDeg, c.yawDeg);"}}

The rejected
alternative — lazy recompute behind a dirty bit — would force `getViewMatrix() const`
to be mutable-cached and would open the classic stale-matrix window where a setter
runs after the frame's matrices were already read. A few redundant
`glm::perspective` calls are a rounding error next to a path-traced frame; a stale
view matrix is a wrong image.
:::

## The basis reaches the renderer through the view matrix

`getFront`, `getUp`, `getRight`, `getPitch` and `getYaw` have no caller in the
tree. The windowed host's fly camera is an ECS system that turns and moves the
camera entity's `Transform` from the world's input, and the scene view hands the
result to the camera as a position and two angles:

{{cite ohao/ecs/world.cpp "addSimulate("fly_controller", [](World& w, float dt) { updateFlyControllers(w.registry(), w.input(), dt); });"}}

No renderer reads the basis through these accessors either — it
round-trips through the view matrix. `glm::lookAt` packs the three axes into the
rotation block:

{{cite ohao/render/camera/camera.cpp "viewMatrix = glm::lookAt(position, position + front, up);"}}

and the path tracer's raygen (`rt/path_tracer.slang`, both RT profiles) pulls them
straight back out of the inverse, column by column, to build the pinhole ray:

{{cite shaders/rt/path_tracer.slang "const float3 fwd = -pc.invView[2].xyz;"}}

So the accessors are a gameplay convenience; the vectors they expose are what the
renderer actually consumes.

## Two projections from one camera

`updateVectors` builds a projection matrix from the camera's own fov, aspect, near
and far:

{{cite ohao/render/camera/camera.cpp "projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);"}}

Exactly one of the two pipelines uses it. The deferred renderer takes it verbatim
and applies the Vulkan Y-flip downstream, in `setCameraData`, not in the camera:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_proj[1][1] *= -1;"}}

The path tracer throws the matrix away and rebuilds one from the camera's *fov only*,
with the framebuffer's true aspect and a far plane of 1000, unflipped because the
raygen negates Y itself when forming the ray:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);"}}

`cornell_box`'s `camera.setFov(38.0f)` is therefore honoured by both: the path
tracer reads the fov and nothing else, the deferred rasteriser reads the whole
matrix.

## The aspect ratio the renderer owns

`setAspectRatio` has two callers, both in `VulkanRenderer`. The constructor, which
takes the output size, sets it before anything renders — a window opened at its final
size never resizes, so construction is the only chance:

{{cite ohao/gpu/vulkan/renderer.cpp "m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));"}}

and `resize()` sets it again from the new extent:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (m_camera) m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));"}}

`setPerspectiveProjection`,
`setOrthographicProjection`, `setProjectionType` and `focusOnPoint` have no callers
at all — which means the
orthographic branch of `updateVectors` is unreachable in shipping code, and near/far
are frozen at their constructor defaults for the process lifetime. The renderer
default-constructs the camera, and the constructor's default argument wins over the
member initialiser, which disagrees with it — the class declares two different
default aspect ratios, 16:9 on the field and 16:10 in the signature, and 1.6 is the
one a default-constructed camera starts with, until the renderer overwrites it:

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

The push constants carry the matrix as its four columns, so that element is the `.y`
of the 0-indexed column `[1]`, and that is the one the raygen reads. $P_{11}$, and the aspect baked into it, is never
touched:

{{cite shaders/rt/path_tracer.slang "float3 dir = normalize(fwd + right * (ndc.x * tanX) - up * (ndc.y * tanY));"}}

## The near plane is twice what it says

`GLM_FORCE_DEPTH_ZERO_TO_ONE` appears nowhere in the repository, so `glm::perspective` and
`glm::ortho` hand back OpenGL-convention matrices that map the frustum to
$z_{ndc} \in [-1, 1]$. Vulkan clips against $0 \le z_c \le w_c$. The surface where
$z_c = 0$ sits at view depth

$$z_{\text{eye}} = \frac{2fn}{f+n}$$

the harmonic mean of the near and far distances — with the camera's permanent
$n = 0.1$, $f = 100$ that is 0.1998. The GBuffer pipeline runs with depth clamping
off, so that slab is clipped rather than flattened:

{{cite ohao/render/deferred/gbuffer_pass.cpp "rasterizer.depthClampEnable = VK_FALSE;"}}

The deferred pipeline's effective near plane is therefore twice its nominal one, and
the depth buffer only ever receives the outer half of GL's depth mapping. CSM
escapes: it is the single raster pipeline in the engine that enables depth clamp,
so its slab is flattened rather than clipped:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.depthClampEnable = VK_TRUE; // Clamp depth to [0, 1]"}}

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

## Contracts

- Pitch must stay strictly inside ±90° or `cross(front, worldUp)` degenerates and the view matrix's rotation block stops describing the camera. All three writers of `pitch` clamp it — `setRotation`, `rotate`, `focusOnPoint` — and a "cleanup" that widens any of them to ±90 exactly will produce a frame with a randomly rolled camera, not a gimbal-locked one.
- The camera's aspect is the renderer's: set from the extent in `VulkanRenderer`'s constructor and again on every `resize()` that changes it. Code that replaces the camera, or renders through a different extent, must set it too.
- The Y-flip is *not* in this class. Consumers that build a Vulkan clip-space matrix must apply `proj[1][1] *= -1` themselves (deferred does; the path tracer instead negates Y when forming the ray). Adding the flip inside `Camera` would double-flip one of the two.
- Near and far are immutable after construction (0.1 and 100), because the only two mutators that touch them have no callers. Any geometry beyond 100 units is outside the deferred frustum regardless of what the CSM cascade range claims.
