---
module: architecture
id: invariants
title: Cross-module invariants
standard: v2
figures: [architecture-invariants-enforcement-reach]
---

## What a violation actually looks like

Nothing here segfaults when two modules disagree. The index buffer still has
entries at the offset you computed, the material row still parses as three
`vec4`s, the descriptor set still binds, the frame still presents. You get a
picture — the wrong one, off by an amount that reads as a lighting decision. The
module chapters explain each mechanism in isolation; the question none of them
can answer is the cross-cutting one: *what would have caught this?*

Four things in the repository could, in principle. This page audits their reach.

## The compile-time tier stops at the language boundary

`OHAO_ASSERT_GPU_LAYOUT(Type, Bytes)` is the engine's one *named* structural
contract primitive. It pairs a `GpuPod` requirement — trivially copyable **and**
standard layout, so that `memcpy` into mapped device memory is defined and the
first member is guaranteed to sit at offset 0 — with a `sizeof` check against a
canonical constant.

{{cite ohao/core/concepts.hpp "must be trivially copyable standard-layout for GPU"}}

Three call sites carry the macro, covering `LightData` (128 bytes),
`PTPushConstants` (256) and `GPULight` (80). The same contract is asserted twelve
more times without it: each of the four push-constant structs of
`NrdCinematicPost`, which compiles only with NRD enabled, gets a bare
`static_assert` on its byte size,

{{cite ohao/render/rt/denoise/nrd_cinematic.cpp "sizeof(CompositePC) == 176"}}

the deferred pass structs (`ShadowPushConstant`, `CameraUBO`, `DrawPushConstant`)
get one each, the `RenderScene` table rows (`InstanceGpu` 160, `MeshGpu` 16, the
material `Pack` against `MaterialGpuPack::kBytes`) and `Vertex` (100) get one each,
and `PBRMaterialParams` gets a `% 16 == 0` alignment assert beside its `GpuPod`
check. Fifteen host-side size or alignment assertions, then — and all fifteen are
assertions about C++ types, evaluated by the C++ compiler. No build step reflects
the compiled SPIR-V. Set against the surface they guard — the path tracer alone
declares a thirty-seven-entry descriptor set layout whose binding numbers run to
37, and the buffers behind those entries are re-declared independently in the
raygen, closest-hit, any-hit and miss stages —

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(37, BUFFER, HIT);"}}

fifteen host-side checks are still a thin instrument. They catch *someone added
a float to the struct*. They are blind to *someone reordered the GLSL block*, and
blind by construction rather than by omission.

`engine_tests` sits in the same tier and inherits the same ceiling: its GPU-layout
case is a handful of `static_assert`s and constant comparisons over
`layout_meta.hpp`, checking hand-computed answers and never touching a pipeline.

## The tier that does cross the wall is off by default

The Vulkan validation layers are the only mechanism in the stack that reads both
sides: they compare the pipeline layout against the SPIR-V interface, check
descriptor types and counts, and enforce the structural rules of descriptor
indexing. Instance creation requests `VK_LAYER_KHRONOS_validation` only when an
environment variable is set.

{{cite ohao/gpu/vulkan/device_setup.cpp "enable with OHAO_VALIDATION=1 env var"}}

So the default for every example binary and every golden render is a run with no
cross-language checking at all. Two things do turn it on. `check_validation.py`
runs real scenes with the variable set and fails on any message, warning or error:

{{cite tests/validation/check_validation.py "run real scenes under VK_LAYER_KHRONOS_validation and fail on"}}

and two `engine_tests` cases set it around `initialize()` and assert that a
deferred resize — SSAO, TAA and bloom included — leaves the layer silent. A third
route is `ohao_game` itself: `LiveHost::shutdown` returns the validation
messages seen over the whole run including teardown, so a windowed run with the
variable set exits nonzero on any report.

{{cite tests/engine/engine_tests.cpp "the validation layer reports nothing across the resize"}}

Both read the deferred pipeline, and on it they pass: the checker's two scenes,
`cornell_box` and the damaged helmet, both report clean. Their reach ends at the
raster path. The checker's scene list is those two runs, each passed the
`deferred` token,

{{cite tests/validation/check_validation.py "SCENES = ["}}

and the path tracer runs under validation only when a person types the command.
Do so — `OHAO_VALIDATION=1 cornell_box out.png 1 --denoise=none`, the default
offline mode — and the layer reports exactly one VUID, and it is the failure class
this page is about. The path tracer's layout flags binding 12 — the bindless texture table — as the
variable-count binding,

{{cite ohao/render/rt/path_tracer_descriptors.cpp "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |"}}

while bindings up to 37 are declared behind it, and Vulkan permits the flag only
on the highest-numbered binding
(`VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004`). The
layer names binding 37 as the one that should carry it. The error has survived
because the one automated validation run never builds this layout.

## The pixel tier is real and coarse

The golden corpus renders four scenes with the denoiser disabled, so what is
pinned is raw deterministic beauty rather than a filter's output: the Cornell box
and the damaged helmet, each once as an offline path trace and once through the
deferred hybrid pipeline.

{{cite tests/golden/manifest.json "./build/cornell_box {out} 16 --denoise=none"}}

{{cite tests/golden/manifest.json "./build/cornell_box {out} 4 deferred --denoise=none"}}

Each is compared after downscaling to 640 px wide,

{{cite tests/golden/render_golden.py "DOWNSCALE_WIDTH = 640"}}

against two thresholds that must **both** hold: a per-channel maximum and an
allowance of differing pixels. The defaults are 4 LSB and 1 %; the deferred scenes
are held to 0 and 0, because deferred rendering is bit-exact from run to run (since
sub-project 4 step 1; the helmet used to carry a looser 6 LSB and 5 %).

{{cite tests/golden/render_golden.py "return s is not None and s["}}

The conjunction is stricter than the percentage suggests. `max_abs` is a single
maximum over the whole frame, so a change that moves one compare-resolution
channel past the limit fails regardless of how little area it touches. Sparseness
alone saves nothing; only a change that is simultaneously sub-threshold in
magnitude *and* sparse gets through.

Where the tier is genuinely coarse is resolution and coverage. The 1920→640
reduction is a bilinear resample, so a difference confined to a handful of
full-resolution pixels is averaged against its neighbours before the maximum is
taken. And the corpus is two scenes in two pipelines, with no denoiser output and
no scene that exercises the ECS beyond spawning meshes and lights. The manifest's
own comment concedes the shape of that problem, asking that every fixed bug add a
scene so it cannot silently regress.

The 4-LSB tolerance is justified in the harness as absorbing a floating-point
floor from non-associative GPU reduction order,

{{cite tests/golden/render_golden.py "DEFAULT_MAX_ABS_DIFF = 4"}}

which is the standard argument. `--selftest` renders each scene twice and diffs
the two renders against each other: both path-traced scenes and the deferred
Cornell box come back at `max_abs=0 diff_px=0`, bit-identical. The deferred helmet
does not — it reports `max_abs=2` over about three percent of compare-resolution
pixels against itself, which is why its limits are wider. Against the stored
goldens all four pass.

The two path-traced goldens are a hash of an unchanged sampler rather than a
tolerance the image has slack inside — the manifest says so at length, and says why
no sample count fixes it. See `systems/tests` for how a golden can go red for a
reason that is not a renderer change, and what closes it.

## And nothing starts any of them

The repository carries one CI workflow and it publishes this site. There is no
build job, no golden run, no installed git hook. Since sub-project 4 every tier is one
command away — `python tools/check.py` builds and runs every suite, harness and gate
through CTest and answers with one JSON verdict — but `engine_tests`,
`check_validation.py` and `render_golden.py` still run only when a person, or an agent,
types that command.

The audit closes on an honest total, one line per mechanism. The asserts are the
only tier that fires by itself, on every compile, and they stop at the language
boundary. `engine_tests` inherits that same ceiling *and* has to be launched. The
validation layers are the only thing in the stack that reads both sides; they
are off unless an environment variable is set, and the script that sets it renders
only the raster path. The golden corpus is the only tier
that judges a finished image, and it is coarse and manual.

{{figure architecture-invariants-enforcement-reach "Conceptual: how far each of the four mechanisms reaches across the C++/SPIR-V boundary. Bar lengths are schematic; the tolerances and the opt-in flag are read from render_golden.py and device_setup.cpp, not measured."}}

## What the unguarded contracts have in common

Two shapes account for nearly all of them.

**Slot joins.** The ray-tracing path no longer joins arrays by a running walk of the
scene. Each TLAS instance's custom index is the instance's slot in the
`RenderScene` table,

{{cite ohao/render/scene/render_scene.cpp "vi.instanceCustomIndex = slot & 0xFFFFFFu;"}}

and the hit shaders use that single number to fetch the instance row, which names
the mesh, its index range and its material refs.

{{cite shaders/rt/pt_closesthit.rchit "InstanceGpu inst = instances[gl_InstanceCustomIndexEXT];"}}

That makes the instance key explicit — the same slot addresses the TLAS entry, the
instance row and the draw list — but two positional joins survive underneath it.
The per-triangle material slot is read at `triangleSlotOffset + gl_PrimitiveID`,
so the slot stream must run parallel to the mesh's index order:

{{cite shaders/rt/pt_closesthit.rchit "uint slot = triSlot[mesh.triangleSlotOffset + uint(gl_PrimitiveID)];"}}

and the slot indexes the instance's material refs, with anything past the ref count
falling back to material 0 rather than failing:

{{cite shaders/includes/scene/scene_data.glsl "((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)"}}

Registration checks the first (a stream of the wrong length is logged and padded or
truncated) and clamps the second to 64 refs per instance. Neither mismatch is
visible in the picture except as a wrong material.

**Hand-copied constants.** The frame-ring depth is named once,

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

and then re-typed as a bare `+ 3` wherever a caller drives the render loop itself,
because the pixel buffer it reads back is the frame submitted three renders ago:

{{cite examples/turntable.cpp "for (int s = 0; s < spp + 3; s++) {"}}

Four offline examples spell it that way. Change the named constant and four
call sites quietly render the wrong sample count. The no-texture sentinel shows the
same pattern with the polarity reversed. The producers use a named constant,
`render::kNoTexture`, but there are now two more names for the same value —

{{cite ohao/gpu/layout_meta.hpp "kNoTexture = 0xFFFFFFFFu;"}}

in the layout header, and `NO_TEXTURE` in `scene_data.glsl` — and neither has a
caller. The shaders that test the value re-type `0xFFFFFFFFu` instead. The value is
load-bearing in a dozen places and spelled four ways; two of the named constants
are decoration.

Two ordering rules round out the set, and both are repaired in code rather than
asserted. A new HDRI joins the one bindless texture table, so frames in flight must
finish before it is registered:

{{cite ohao/gpu/vulkan/renderer.cpp "new HDRI joins the bindless table, so frames in flight must finish"}}

and the RT light buffer's 16-byte header — count, environment index, intensity
scale — is rebuilt whole from the light table's own members on every upload, so the
index and scale have to be held there, not written into the buffer:

{{cite ohao/render/scene/light_table.cpp "const uint32_t header[4] = {static_cast<uint32_t>(m_rt.size()), m_envMapTexIdx,"}}

:::why
The per-triangle slot stream is positional on purpose and should not be "fixed".
Giving each triangle a key — a mesh id or material handle stored beside the index —
buys a second SSBO fetch in the closest-hit shader, on the hottest path in the
engine, to defend against a class of bug that a human catches the first time they
look at the render. The instance level already has its key (the slot); the
triangle level does not need one. What the choice really costs is that the
ordering contract lives in the registration check and in prose instead of in a
type; the cheap way to buy it back is a debug-only comparison at registration, not
a key column in the hot path.
:::

:::key
Every invariant in this engine that spans the C++/GLSL boundary is enforced by a
person reading two files. The compile-time asserts stop at the boundary,
validation is opt-in, the golden corpus is coarse, and no
automation starts any of them.
Read every cross-module claim on these pages as something to re-verify against the
code, not as something the build guarantees.
:::

## Contracts

- A mesh's triangle material-slot stream must hold exactly one entry per triangle, in index order. Registration logs a length mismatch and pads or truncates; the picture is the only other symptom. An instance holds at most 64 material refs, and a slot beyond its ref count resolves to material 0.
- `render()` must be called at least `MAX_FRAMES_IN_FLIGHT` times before the pixel buffer holds real content. Four examples encode this as a literal `+ 3`; changing the constant without changing all four silently changes sample counts.
- The RT light buffer header (count, environment index, intensity scale) is rebuilt from the light table's members on every upload. Set the index and scale through `RenderScene::setEnvMapTextureIndex` and `setEnvIntensity`; writing the buffer directly is overwritten on the next upload.
- The variable-count flag on binding 12 of the path tracer's layout already violates the highest-binding rule, because bindings run to 37. Any fix moves the texture table to the highest-numbered binding and updates the `binding = 12` declarations in the shaders with it. Only the validation layer reports it, and only if `OHAO_VALIDATION` is set in the environment.
