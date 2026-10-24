---
module: architecture
id: request-path
title: Request path
standard: v2
figures: []
---

## One sequence, in an order that matters

OHAO has no editor host. The offline examples — `cornell_box`, `model_viewer`,
`env_demo`, `turntable` — are thin `main()`s over one sequence: construct a `VulkanRenderer` at a fixed output size, bring
up the device, create an `ecs::World` and wire its render extract (and, for a world
built from data, instantiate) to the renderer's `RenderScene`, build entities, apply
the world's camera and environment, choose a `RenderMode`, then loop
`world.sync()` and `render()` and read the pixels back. `ohao run --render` runs the
same sequence on a scene file. `cornell_box`
is the shortest complete instance of it, and it shows the one convention that
surprises people reading the CLI: the output resolution is a compile-time constant
in the driver, never an argument — 1920×1080 in `cornell_box`, `model_viewer` and
`env_demo`, 1280×720 in `turntable`.

{{cite examples/cornell_box.cpp "const uint32_t W = 1920, H = 1080;"}}

The windowed host, `ohao_game`, runs the same sequence through `ohao_runtime`'s
`LiveHost` with two substitutions: the renderer is sized from the window's
framebuffer extent and initialised against a present target, and the loop presents
to a swapchain instead of reading pixels back. `LiveHost` also owns the
`ecs::World` and an `api::Session` on it, and installs the extract and instantiate
itself:

{{cite ohao/runtime/live_host.cpp "if (!m_renderer->initialize(m_window.presentTarget(cfg.vsync))) {"}}

Scene construction takes one of two forms. `cornell_box` and `model_viewer` build
the entities a scene file holds — a `MeshSource` naming a primitive or a model path,
a `MaterialOverride`, lights, a `Camera` and an `Environment` — and install
instantiate, which builds each GPU mesh on the first update; `ohao_game` and
`ohao run` load those entities from a scene file instead:

{{cite examples/cornell_box.cpp "ecs::installInstantiate(world, renderer.renderScene(), renderer.getTextureManager(), assets);"}}

`env_demo` and `turntable` still call the `spawn` helpers, which upload
directly. `spawnModel` registers the mesh and its materials with the `RenderScene` —
an upload and a BLAS build, blocking, at load time — and returns an entity carrying a
`MeshRenderer` that holds the handles:

{{cite ohao/ecs/spawn.hpp "entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,"}}

`spawnLight` makes an entity with a `Light`. Neither path touches a command buffer,
and neither makes anything visible yet: an entity reaches the screen only after a
`world.sync()`.

## The frame, from game hooks to passes

One frame is the same chain in every host. In `ohao_game` the live host runs the
steps its timeline (`core/timeline.hpp`) owes for the wall time that passed — for
each, `Session::step` (the step's input, then the world's simulate phase) and then
the fixed-step game hook — then the per-frame hook once, then the world's sync phase,
then the renderer:

{{cite ohao/runtime/live_host.cpp "m_world->sync();   // instantiate, transforms, the render extract: never a simulation step"}}

The headless drivers have no live host, so they call `world.sync()` before each
`render()` themselves: a render never steps the simulation. The sync phase runs its
systems in exactly the order they were added. The constructor adds the structural
`transform` (and, to the simulate phase, the engine-provided `spin`):

{{cite ohao/ecs/world.cpp "addSync("transform", [](World& w) { updateTransforms(w.registry()); });"}}

Instantiate, when installed, goes immediately before `transform`, so a mesh it builds
gets its world matrix in the same sync. `TransformSystem` recomputes `WorldTransform`
for every dirty entity and its descendants, parents first, and tags the movers
`RenderDirty`. The render extract is installed straight after it and consumes those
tags:

{{cite ohao/ecs/render_extract.cpp "world.addSyncAfter("transform", "render_extract", [state](World&) { state->run(); });"}}

It releases the slots of vanished renderables, creates an instance for every
renderable without one — in creation order, so slot numbers are deterministic —
rewrites the moved ones, and rebuilds the light list when a light changed. All of
that is host-side: `createInstance`, `updateInstance` and `setLights` edit the
`RenderScene`'s tables and mark entries dirty. Nothing reaches device memory until
the renderer records its frame. There the first thing in the command buffer, ahead
of every pass, is the `RenderScene`'s own update:

{{cite ohao/render/scene/render_scene.hpp "void recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot);"}}

`recordUpdates` copies the dirty instance, material and light rows from the frame
slot's staging buffer into the device-local tables, then builds or refits the TLAS.
The slot's fence has already been waited on, so a host edit can never race a frame
still in flight. The deferred chain follows it in the same command buffer; on the
RT path the profile is handed the freshly updated buffers first:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->setSceneBuffers(*m_renderScene);"}}

So the order is spin → game hooks → instantiate → `TransformSystem` → `RenderExtract`
→ `RenderScene::recordUpdates` → passes, and every arrow is a one-way hand-off.

Two mistakes in that order fail silently. `renderScene()` dereferences the
renderer's `RenderScene` without a check, and that object exists only after
`initialize()` has run:

{{cite ohao/gpu/vulkan/renderer.hpp "[[nodiscard]] render::RenderScene& renderScene() { return *m_renderScene; }"}}

Call it first and you hold a null reference. And an entity spawned into a world
whose extract was never installed keeps its mesh and materials in the scene but
never gets an instance, so no pass has anything to draw. The rest of the chain runs
anyway, and the Preetham sky pass is on by default, so what lands is an empty
sky-lit frame — not black, and not an error either.

{{cite ohao/render/deferred/deferred_renderer.hpp "bool      m_skyEnabled{true};"}}

Teardown runs the same chain backwards. The world holds `RenderScene` slots, so it
goes first, while the scene still exists:

{{cite ohao/runtime/live_host.cpp "m_world.reset();   // releases its RenderScene slots while the RenderScene exists"}}

## Why ray tracing has nothing to replay

`setRenderMode()` is where ray tracing actually comes into existence. The renderer
deliberately does *not* create a path tracer during `initialize()`:

{{cite ohao/gpu/vulkan/renderer.cpp "(eager dual init at 4K can OOM)."}}

What it does create is the `RenderScene`, which owns the BLAS per mesh and the TLAS
over the instance table whether or not any path tracer exists. So a profile created
later has nothing to catch up on: the scene data lives in the render world, not in
the profile. `ensureRTRenderer` constructs the profile and returns, with a comment
saying why nothing else is needed:

{{cite ohao/gpu/vulkan/renderer.cpp "Scene buffers, textures and lights are bound every RT frame from the"}}

Each RT frame the renderer binds the tables, the bindless texture list and the
environment resources into the profile's descriptor set before dispatch. A driver
may therefore select its mode before or after spawning — both orders cost the same,
one upload, and neither needs a replay.

:::why
The rejected alternative is eager construction: build both the realtime and the
offline `PathTracer` in `initialize()`. It lost on memory. Each profile owns a
beauty image plus the full AOV set at output resolution — motion vectors, depth,
roughness, split diffuse/specular radiance, albedo, normal-roughness — and the
source states the hazard as a possibility, not a measurement: eager dual init *can*
OOM at 4K. No shipped driver renders at 4K, so at the resolutions the examples
actually hardcode the avoided cost is a projection, not observed history. Lazy
construction is cheap to keep because scene data no longer lives in the profile:
creating one later copies nothing.
:::

Mode selection can also fail closed. If RT is unavailable, `setRenderMode` prints
and returns *without* assigning `m_renderMode`, leaving the renderer in whatever
mode it already had — which at startup is `Deferred`.

{{cite ohao/gpu/vulkan/renderer.cpp "Path tracing not available, staying in current mode"}}

{{cite ohao/gpu/vulkan/renderer.hpp "RenderMode m_renderMode{RenderMode::Deferred};"}}

So an RT request on a device without ray tracing renders deferred instead, and the
examples print the mode they *requested*, not the one the renderer accepted, so a
silent downgrade reads as success on stdout.

## One frame per sample, and the last one read

The offline drivers render exactly the frames they were asked for and read the
last:

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : samples;"}}
{{cite examples/cornell_box.cpp "if (i == frames - 1) renderer.requestReadback();"}}

With `samplesPerFrame` at its default of 1, one `render()` call adds one sample to
the accumulator, so the last of `samples` frames has integrated exactly `samples`
samples. The request makes that frame, and no other, copy its image into its ring
slot's staging buffer, and `readback()` waits on that frame's fence alone.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

Deferred takes the other branch of that ternary and renders a fixed count that
ignores the sample argument entirely: in raster mode the spp number on the
command line does nothing. The count is per driver, not engine-wide (ten in
`cornell_box`, thirty in `model_viewer`), long enough for RT GI's and TAA's
histories, and the PNG is the last of them, as `ohao run --frames` writes it.

{{cite examples/model_viewer.cpp "const int frames = cli.useDeferred ? 30 : samples;"}}

## Three different exits from the GPU

`readback()` has three exits behind one signature, and which one runs depends on
the denoiser. `None`, `Atrous` and `DLSSRR` share the first: return the requested
frame's staging buffer. `None` does nothing to it; `Atrous` denoises the beauty
image (`m_outputImage`) in place on the GPU, and the DLSSRR path, when it runs at
all, tonemaps into that same image, so neither needs a readback of its own.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (isRTRenderMode(m_renderMode) && (m_denoiseMode == DenoiseMode::OIDN || m_denoiseMode == DenoiseMode::NRD) &&"}}

NRD takes the second, and it touches neither the staging buffer nor the accumulator: it
copies the tracer's RGBA8 tonemapped image — already denoised and tonemapped on
the GPU — straight into host memory behind a
`vkQueueWaitIdle` on the graphics queue.

{{cite ohao/gpu/vulkan/renderer.cpp "VkImage srcImage = getNrdTonemappedAOVImage();"}}

OIDN takes the third: a full `vkDeviceWaitIdle`, then three float readbacks — the
accumulation image plus the albedo and normal AOVs — into host memory, where the
CPU denoiser runs over them.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readbackImage(rtRenderer->getAccumImage(), beauty);"}}

That third path matters because the offline profile denoises by default:

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

so a plain `cornell_box out.png 1024` writes a PNG denoised from the accumulator
of its last frame, holding 1024 samples: the same budget `--denoise=none` lands
on. The result of either denoising path is cached and invalidated by every
`render()`, so calling `readback()` twice for one frame costs one denoise, not
two. Both allocate a command buffer, submit it and block, which is safe only
after `render()` returns, on the thread that called it.

## The settings reset that eats your overrides

Once the loop starts, the renderer re-derives its RT settings from the pipeline
object on every single frame:

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = pipeline.getDefaultSettings();"}}

An override a driver configured once does not survive even to its first dispatch.
`renderRTPipeline` calls `prepareRTSceneForFrame` — the function that performs that
reset — before it records the command buffer that dispatches the tracer:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "prepareRTSceneForFrame(pipeline);"}}

The same call is where the frame's scene-side bindings are refreshed — the light
buffer, the bindless texture list, the environment CDFs and the HDRI scale — so it
is the single per-frame seam between the renderer and the `RenderScene`. Four values
are explicitly re-injected around the reset — anisotropy strength and
rotation, subsurface strength, and the realtime per-frame sample count:

{{cite ohao/gpu/vulkan/renderer.cpp "so restore the user's choice here (same pattern as aniso/SSS preservation)."}}

The denoiser gets a different mechanism: `setDenoiseMode()` raises a sticky
`m_denoiseModeOverridden` flag that blocks the per-frame resync, and the override is
pushed down into the tracer's own settings for exactly two modes, `Atrous` and
`DLSSRR`.

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings.denoiseMode = m_denoiseMode;"}}

`Atrous` needs it because the RT pipeline dispatches that denoiser itself, in place
on the beauty image. `DLSSRR` rides the same branch: `path_tracer_render.cpp` holds
a real NGX evaluate-and-tonemap block, compiled out unless the vendored NGX static
library is present.

{{cite ohao/render/rt/path_tracer_render.cpp "m_dlssRR->tonemap(cmd, m_dlssColorOutView, m_outputView"}}

OIDN is the one genuinely consumer-side mode — it runs on the CPU after readback.
NRD is dispatched inside the RT pipeline like `Atrous`, but off a different gate:
the tracer runs it whenever the denoiser initialized and the profile enables
auxiliary AOVs, and never reads `denoiseMode` to decide, so pushing the override
down would buy it nothing.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs) {"}}

:::key
Scene data reaches the GPU along one chain — spin, game hooks, instantiate,
`TransformSystem`, `RenderExtract`, `RenderScene::recordUpdates`, passes — and there is one authority
over RT behaviour (the active `IRTRenderPipeline`, reasserted every frame). Ordering
mistakes around the chain fail *silently* — an extract that was never installed, a
refused mode, a stale override — never with an error. When an OHAO driver renders an
empty frame or ignores a flag, suspect call order before suspecting the shaders.
:::

## Contracts

- `initialize()` must precede `renderScene()`. Before it, the `RenderScene` does not exist and the accessor returns a null reference.
- Install the render extract (`ecs::installRenderExtract`, or let `LiveHost` do it) before expecting entities on screen, and run `world.sync()` before every `render()` (never `update`, which would also step the simulation). Spawning uploads a mesh and its materials; only the extract makes an instance of it.
- The `RenderScene` must outlive the `ecs::World`. `LiveHost::shutdown` resets the session and then the world before the renderer; in `cornell_box` the `VulkanRenderer` is declared above the `ecs::World`, so reverse declaration order destroys the world first.
- `setRenderMode()` may refuse. Read back `getRenderMode()` rather than trusting the value you passed; the examples do not, and print the requested mode.
- Pixels come from one place: `requestReadback()` before the `render()` you want, `readback()` after it. A frame that was not requested copies nothing, and `readback()` after it returns an empty span.
- Any new per-run RT setting must be re-injected in `applyRTRenderSettings`, or the `getDefaultSettings()` reset inside `prepareRTSceneForFrame` silently reverts it before the next dispatch.
