---
module: systems
id: examples
title: Examples map
standard: v2
---

## A scene is a `main()` — or a file the `main()` can write

The examples build their scenes two ways. The older way is still how `turntable`
and `env_demo` work: the scene exists only as the C++ that constructs it —
an `ecs::World`, then `ecs::spawnModel` for each mesh (which registers the geometry
and materials with the renderer's `RenderScene` and creates an entity carrying a
`MeshRenderer`) and `ecs::spawnLight` for each light.

`cornell_box` and `model_viewer` build theirs as data. Their worlds come from
GPU-free builders in `examples/example_scenes.hpp` — `buildCornellBox` and
`buildModelViewerScene` — that create only authoring components: a `Camera`, an
`Environment`, `Light`s, and `MeshSource` entities naming a primitive (`QuadPrim`,
`BoxPrim`, `SpherePrim`) or a model asset, each with a `MaterialOverride`:

{{cite examples/example_scenes.hpp "addMesh(world, name, ecs::MeshSource{ecs::QuadPrim{a, b, c, d, normal, color}}, surface(color, 0.95f));"}}

The instantiate system turns each `MeshSource` into a `MeshRenderer`, and
`runtime::applySceneView` (`ohao/runtime/scene_view_apply.hpp`) pushes the camera and the
environment into the renderer. Because nothing in the builder touches the GPU, both
examples take `--save-scene <path>`, which writes the world as a canonical scene file
and exits before a renderer exists:

{{cite examples/cornell_box.cpp "// The world is authoring data only; --save-scene writes it without a renderer."}}

The two files that flag produced are checked in as `assets/scenes/cornell_box.scene.json`
and `assets/scenes/damaged_helmet.scene.json`, and they are what the `ohao` CLI
(`tools/ohao`) loads: `schema`, `validate`, `fmt`, `apply`, `query`, `summary`, and
`run`, whose `--render` renders a scene file through the same instantiate and
scene-view path. A third file, `assets/scenes/helmet_game.scene.json`, is the helmet
scene plus a spinning cube (a `Spin`), a `FlyController` on the camera and an
`InputActions` entity binding the keys; it is what `ohao_game` plays. In
every driver the render extract is installed on the world once, and from then on
`world.sync()` is what turns entity edits into GPU state.

`examples/` builds four offline binaries and one windowed host. The four
offline ones are half scene description, half harness. `cornell_box` authors its
five walls as `QuadPrim`s and stacks twelve coloured sphere lights in the box as a
many-light stress case. `model_viewer` builds a bedroom — bed, headboard,
nightstand, as `BoxPrim`s — around whatever model you pass, sized from the model's
own bounds by `SceneFramer`. `turntable` picks its room from a mode string and
orbits. `env_demo` drops a 50 m procedural ground quad under the model so it stops
floating in void.

The fifth, `ohao_game`, is not a harness and builds no scene. It loads a scene file
(by default `helmet_game.scene.json`) with a `scene.load` call on the session of
`ohao_runtime`'s `LiveHost`, turns bloom, TAA and SSAO on at exposure 1, and runs the
host's realtime loop: deferred into a swapchain, a fixed-step simulation, the scene's
`FlyController` flying the camera and its `Spin` turning the cube. Its own code only
quits on the `quit` action and captures the cursor while `look` is held. Nothing is
read back to the CPU unasked, and its `--frames N` mode fails the run if anything was.

{{cite examples/ohao_game.cpp "const std::string loaded = host.session().call("scene.load", params);"}}
{{cite examples/ohao_game.cpp "if (r.readbackCopies != 0) return 3;"}}

Everything below (the render loop, the shared argv parser, the destruction
order) describes these four inline drivers.

## The last frame is the one saved

Every inline driver's RT path renders `samples` frames and saves the last; the
deferred path renders a fixed count and saves the last of those.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : samples;"}}

The loop asks for its last frame before rendering it, and reads it back after:

{{cite examples/cornell_box.cpp "if (i == frames - 1) renderer.requestReadback();"}}

Only that frame copies its image into its ring slot's staging buffer, and
`readback()` waits for that frame's fence alone, so the PNG is the frame just
rendered. Until sub-project 2b step 2 every headless frame copied, a frame's
copy reached the CPU three `render()` calls later, and these loops ran
`samples + 3` frames to land on the right one.

What accumulates is not the push-constant sample index. `pc.params.z` carries
`m_sampleIndex`, which seeds the sampler and nothing else. The accumulation
weight is `pc.control.y`, holding `m_historyFrameCount`, and the running sample
count lives in the alpha channel of the accumulation image, incremented by the
raygen itself. The two counters advance together at the bottom of every
dispatch, which is why the distinction stays invisible until something resets
one of them.

{{cite shaders/rt/pt_raygen.rgen "imageStore(accumBuffer, pixel, vec4(acc, count));"}}

Take `DenoiseMode::None` for the moment. Call $k$ (zero-based) leaves an image
carrying $k+1$ accumulated samples. With $N$ `render()` calls and the last one
read back, the saved image's sample count $s$ is that of call $N-1$:

$$s(N) \;=\; (N-1) + 1 \;=\; N$$

So $N = \text{spp}$ saves exactly the spp the user asked for. The deferred branch
accumulates nothing, and its counts are about history instead: `cornell_box`
saves the tenth deferred frame and `model_viewer` the thirtieth, long enough for
RT GI's and TAA's histories. Both ignore the spp argument entirely.

{{cite examples/model_viewer.cpp "const int frames = cli.useDeferred ? 30 : samples;"}}

The `ohao` CLI follows the same rule through the shared capture: it renders `spp`
RT frames, and one deferred frame (ten with TAA) unless `--frames` says otherwise,
and keeps the last. So a scene rendered by `ohao run` with the example's count
gives the example's pixels: the golden manifest passes `--frames 10` for the
Cornell box and `--frames 30` for the helmet.

{{cite ohao/runtime/frame_capture.cpp "if (r.mode != "deferred") return r.spp;   // the path tracer: one sample per frame"}}

## What each denoiser makes of it

`DenoiseMode` has five values: `None`, `OIDN`, `NRD`, `Atrous` and `DLSSRR`.
`readback()` returns the requested frame's staging copy for `None`, `Atrous` and
`DLSSRR`. For `OIDN` and `NRD` it does its own readback of that frame's images,
a command buffer submitted and waited on once the frame's fence has signalled.
OIDN's readback takes the accumulation image itself, whose RGB is the running
mean of every sample so far.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {"}}

Accumulation splits the set along a different seam. `Atrous` and `DLSSRR` are
flagged `wants_fresh_sample`: the host forces `pc.control.y = 0` every frame,
the raygen takes its no-history branch, and the accumulation image is stored
with `count = 1.0` forever. Those denoisers own temporal accumulation
themselves, so handing them a converged image would double-filter it.

{{cite ohao/render/rt/rt_meta.hpp "static constexpr bool wants_fresh_sample ="}}

{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.y = freshSample ? 0u : m_historyFrameCount;"}}

{{cite shaders/rt/pt_raygen.rgen "if (historyFrameCount == 0u) {"}}

So under `None` and under `OIDN`, the shipped offline default, the saved frame
holds exactly `samples` samples. Under `Atrous` or `DLSSRR` the saved PNG is
1 spp no matter what number you typed: those denoisers carry the history
themselves. `NRD` returns its own GPU-tonemapped composite of the last frame.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

## What the shared flag parser will not tell you

`parseRenderFlags` takes the trailing argv slice — from index 3 in
`cornell_box`, 4 in `model_viewer`, 5 in `turntable` — and matches tokens
order-independently. Unrecognised tokens fall off the end of the
`if`/`else if` chain with no diagnostic, so a typo'd `--denosie=none` renders
happily with the default denoiser.

`deferred` and `rt_realtime` are stored in different fields, and `resolveMode`
resolves the conflict by precedence rather than by order: `deferred` wins no
matter where it appears.

{{cite examples/example_cli.hpp "if (opts.useDeferred) return RenderMode::Deferred;"}}

The sample-count parser swallows anything `atoi` cannot turn into a positive
integer and returns the default, so `cornell_box out.png 0` quietly renders 1024
spp; it is also reused as a general positive-integer parser — `turntable` parses
its orbit frame count with it.

{{cite examples/example_cli.hpp "return v > 0 ? v : fallback;"}}

Nothing in the shared parser reports an unrecognised token, so a typo degrades
silently to the default rather than failing the run.

## env_demo imports the parser, then ignores it

`env_demo` carries `using`-declarations for all five shared helpers and calls
two of them. From `argv[5]` onward it runs its own parser.

{{cite examples/env_demo.cpp "for (int i = 5; i < argc; i++) {"}}

The trade is deliberate: it drops `deferred`, `video` and `outdoor`, re-implements
`flip` under a different spelling, and buys twelve `--dump-*` AOV taps, `--seq=K`
consecutive-frame capture, `--lighting=`, `--ground=`, `--aniso=`, `--sss=` and
`--crop=face`. The re-implementation is not quite a copy: the shared `flip`
rotates the mesh actors 180° about Y after framing, whatever the up-axis;
`env_demo`'s `--flip` folds the same 180° into the initial rotation and only in
the Y-up branch.

{{cite examples/env_demo.cpp "flipModel ? 180.0f : 0.0f"}}

That makes it the NRD/guide-buffer debugging instrument rather than a demo. It
is the only place the motion-vector, ViewZ, roughness, diffuse/specular radiance,
hit-distance and NRD-composed images become PNGs.

{{cite examples/env_demo.cpp "if (!renderer.readbackMotionVector(mvRaw, mw, mh)) {"}}

The surprise is the default. Lighting mode defaults to `Studio`, and studio mode
skips loading the HDR that `argv[2]` mandates:

{{cite examples/env_demo.cpp "if (lightingMode != LightingMode::Studio && lightingMode != LightingMode::Cinema) {"}}

So `env_demo model.glb env.hdr out.png 256` renders four neutral sphere lights
against a black background and never opens the `.hdr`. Only `--lighting=hdr-only`
and `--lighting=none` load it.

:::why
The note attached to the skip says the environment at full strength overwhelmed
the three-point rig and tinted surfaces with the env's own colour — "the
pink-ground problem in 4.H v1-v6". The choice made instead was to model a closed
studio: explicit lights, black background, which is what product photography
actually is. The cost is a mandatory argument that the default mode ignores.

The tree records a second, incompatible fix for the same symptom 130 lines
further down. The v8 lighting note blames the *warm key*, not the environment,
and fixes the pink ground by moving all four studio lights to D65 neutral white.
Both fixes shipped; nothing reconciles them. The env explanation is the rationale
attached to the skip, not a settled root cause.

{{cite examples/env_demo.cpp "// v8: D65 neutral white lights"}}
:::

One more artefact of that instrument role: `readbackMotionVector` hands back raw
`RG16F` halves while `readbackRoughnessAOV` decodes in the helper, so `env_demo`
carries a hand-written IEEE-754 half-to-float — denormal renormalisation loop
included — used by nothing but the motion-vector PNG.

{{cite examples/env_demo.cpp "while ((mant & 0x400) == 0) { mant <<= 1; exp--; }"}}

## The orbit reset, and what breaks if you tighten the loop

`turntable` moves the camera to the next orbit position, resets accumulation,
renders `spp` frames, writes the last, repeats.

{{cite examples/turntable.cpp "renderer.resetAccumulation();"}}

The reset must sit after the camera move and before the inner loop, or the
accumulation buffer keeps radiance from the previous viewpoint. Each position's
PNG is that position's last frame, read back on request, so nothing of the
previous position can reach it.

{{cite examples/turntable.cpp "if (s == spp - 1) renderer.requestReadback();"}}

Of the three drivers that can resolve to `Deferred`, `turntable` is the only one
whose loop does not special-case it. It renders `spp` frames per position
whatever `resolveMode` returned, and its banner prints `RTOffline` for any mode
that is not `RTRealtime`, including `Deferred`.

{{cite examples/turntable.cpp "(rtMode == RenderMode::RTRealtime ? "}}

## One model, one entity, N submeshes

`model_viewer` gives a multi-material model a single entity. A model inside the
project becomes a `MeshSource` naming the asset, which the instantiate system loads
and uploads; one outside it is attached in memory with `ecs::attachModel`. Both
paths reach `uploadModelMesh`, which does the splitting that the renderer needs, on
the index buffer rather than the geometry:
it walks the per-triangle material list and emits one submesh per contiguous run of
equal materials, so the vertex data stays welded and the mesh keeps one BLAS.

{{cite ohao/render/scene/model_upload.cpp "std::vector<SubmeshDesc> materialRuns(const Model& model, uint32_t triangleCount) {"}}

The entity's `MeshRenderer` lists one material handle per submesh, in submesh order,
and the builder creates the whole model as one entity, whichever path then gives it
a mesh:

{{cite examples/example_scenes.hpp "out.model = world.create("Model", out.modelTransform);"}}

Both render paths the driver can select read that one description. The G-buffer pass
draws one call per submesh, and the path tracer finds a triangle's material through
the per-triangle slot stream. Neither needs the model pre-split. `render()` runs the
RT pipeline when the mode has one, its renderer exists and the render world reports
ray tracing, and `renderDeferred()` otherwise — including on a device without ray
tracing, where `setRenderMode` refuses the RT switch and the mode stays `Deferred`.

{{cite ohao/gpu/vulkan/renderer.cpp "renderDeferred();"}}

The cost to keep in mind runs the other way: a model whose triangles alternate
between materials produces one submesh per alternation, and each is a draw call in the
raster passes.

:::key
The frame you last rendered is the frame you saved. Every inline driver asks for
its last frame before rendering it and reads it back after, so `spp` frames give
an `spp`-sample image under `None` and `OIDN`, and a deferred run's PNG equals
`ohao run --frames` with the same count, bit for bit.
:::

## Contracts

- A driver calls `requestReadback()` before the `render()` it saves and `readback()` after; `spp` calls then land `spp` accumulated samples under `None` and `OIDN`, and 1 under `Atrous`/`DLSSRR`.
- Camera move → `resetAccumulation()` → render. Reversing the last two mixes viewpoints in the accumulation buffer; skipping the reset entirely does the same.
- The `ecs::World` must be destroyed before the `VulkanRenderer`. All four inline drivers declare the renderer first and the world second, so reverse-order destruction of locals gets it right; the render extract releases its instances and lights from the `RenderScene` as the world goes, and in `cornell_box` and `model_viewer` the instantiate system releases its meshes. Swapping those two declarations would make the extract release into a render world that has already been shut down.
- `deferred` overrides `rt_realtime`/`rt_offline` regardless of argument order in the three drivers that use the shared parser. What follows from that is per-driver: `cornell_box` and `model_viewer` then ignore the spp argument for a fixed 10 and 30 frames, `turntable` keeps rendering `spp` per orbit position, and `env_demo` has no `deferred` token at all.
- An unknown trailing flag is silently ignored by `parseRenderFlags` and by `env_demo`'s own parser — typos degrade to defaults rather than failing the run.
