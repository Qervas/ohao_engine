# Sub-project 2b — Renderer upgrade: measured, GPU-driven, cheaper, correct

Status: written 2026-10-24 on Frank's go-ahead ("sub-project 4 is done; go ahead with 2b",
16:45 Berlin), with the brief to widen 2b from GPU culling into a real renderer upgrade.
Frank delegated the technical choices; this document makes them. It is the binding spec
once Frank accepts it. What only Frank can do is listed under "Frank actions"; the few
questions that need him are under "Open decisions".

Roadmap: row 2b of `docs/ROADMAP.md`. Before it: 4 (agent control surface, done
2026-10-24). After it: the vertical slice (5–7).

Builds on sub-project 4 (`docs/specs/2026-10-23-agent-control-surface-design.md`, "spec 4"
below): `ohao serve` and MCP, the live host, lockstep time, captures on request,
`tools/check.py`, the golden and bench machinery, and Frank's ruling of 2026-10-24 that the
two window GPU budgets stay as they are and are 2b's to meet.

## Thesis: an engine agents can measure

OHAO is an AI-native, general-purpose engine: humans and agents are both its users, and
agents drive it through `ohao serve` and MCP. Sub-project 4 gave agents the live engine.
2b gives them a renderer they can *measure and trust*: every pass timed and budgeted, every
frame deterministic, every setting reachable through the protocol, and a frame cheap enough
that "look at what I just did" costs an agent milliseconds, not tens of them.

Performance comes first, and it is measured before it is changed: the first step profiles
every pass with GPU timestamps, and every later step is gated by a checked-in benchmark.
Structure comes second: GPU-driven geometry, meshlets and clustered lighting change how
geometry, lights and frames flow, so they go in now, while the flow is being rebuilt
anyway (the roadmap's "structure before effects" rule).

## Goal

The hybrid deferred renderer becomes GPU-driven (GPU culling, indirect and mesh-shader
draws, clustered lights), its frame becomes cheap enough that the two capture budgets
Frank kept are met by the code, its known defects are fixed, every pass reports its GPU
time to agents, and its output stays bit-deterministic.

**Done when:**
- `render_bench` measures every pass of the frame with GPU timestamps and gates the
  per-pass and per-frame budgets below; `serve_bench`'s `window_readback_ms` (2 ms) and
  `window_capture_inline_ms` (20 ms) pass, unchanged. Each budget passes five consecutive
  clean (non-noisy) runs on the reference machine.
- GPU frustum and two-pass Hi-Z occlusion culling feed indirect draws (vertex path) and
  mesh shaders (meshlet path) for the G-buffer and the shadow passes; CPU submission cost
  stays flat from 10k to 100k instances.
- Lights are clustered: the 8-light cap is gone, and a 100,000-instance, 1,000-light scene
  renders within its GPU frame budget.
- SSAO darkens what lighting draws; a window host captures and presents path-traced
  frames; examples write the frame they just rendered; glTF node transforms are applied.
- Every deferred golden and every hybrid-RT golden is exact (`max_abs_diff` 0); the
  path-traced goldens are exact or keep today's tolerance, never wider. Every intentional
  image change was re-baselined in its own commit with its reason.
- An agent reads per-pass GPU timings (`render.stats`) and reads and changes renderer
  settings (`render.settings`) over MCP, within the 24 KB `tools/list` budget.

## Agents as users, in 2b

Spec 4's ten properties carry over. 2b adds three:

11. **Measurable.** Every pass has a name, a GPU time and a budget, in the protocol and in
    the bench output. An agent asked "why is this slow?" reads the answer instead of
    guessing; an agent changing the renderer sees its pass's cost in the gate.
12. **Configurable through the protocol.** Quality knobs (RT ray counts, GI resolution,
    culling, AO) are one reflected settings struct with limits and docs, set by
    `render.settings`, so an agent trades quality for speed explicitly and reproducibly.
13. **Cheap to look.** A lockstep inline capture costs a frame plus an encode. The faster
    the frame, the more often an agent can afford to check its work.

## Scope: in 2b vs later

The roadmap's 2b row named GPU culling, meshlets, clustered lighting, Slang, RT
determinism and the open renderer findings, and a render benchmark; it scheduled
upscalers, virtual shadow maps and ReSTIR/probe GI "later, by need". Frank's widening and
the frame-cost ruling change two things: the frame's cost becomes 2b's first goal, and the
RT shadow and RT GI passes are rebuilt for it (denoised low-sample RT, not more samples).

| In 2b | Later (designed for here, built in its own sub-project) | Why later |
|---|---|---|
| Per-pass GPU timing, `render_bench`, render budgets | A GPU frame-time gate across vendors | One reference GPU today |
| GPU frustum + two-pass Hi-Z occlusion culling, indirect draws, deterministic compaction | — | |
| Meshlets with cluster culling; mesh/task shaders where supported, a vertex-pulling fallback over the same meshlets | — | |
| Clustered lighting (froxels), the light table uncapped | Many-light *shadowing* (ReSTIR DI) | 2b shadows up to 4 lights a pixel; ReSTIR DI earns its complexity when the slice's levels need hundreds of shadowed lights |
| RT shadows from scene lights, 1 ray per light, spatially filtered; CSM as the non-RT fallback | Virtual shadow maps | VSMs upgrade the raster fallback; on RT hardware RT shadows are primary. After 5 shows where raster shadows fall short |
| RT GI at half resolution, 2 rays a pixel, temporal + spatial denoise | ReSTIR GI, probe GI | The rework meets the budget; ReSTIR/probes are quality upgrades that reuse 2b's clusters, meshlets and denoiser |
| SSAO fixed and wired into lighting; SSR at half resolution; SSS on classified tiles | — | |
| Path-traced frames presented and captured in a window host | — | |
| Readback on request everywhere (the lagged ring deleted) | — | |
| glTF node transforms applied on load | Level pipeline (`extras` → components) | Sub-project 5 |
| RT determinism, validation-clean RT, the open findings | — | |
| Slang toolchain for new shaders | Porting existing GLSL | Only when a pass is rewritten anyway |
| `render.stats`, `render.settings`, timings in captures | — | |
| — | Upscalers (DLSS SR / FSR / XeSS) | Captures are at native resolution and the budgets are met without one; FSR and DLSS need new dependencies (network, licences); `external/DLSS` is absent here |

## Why (what the current code does)

Each point is a fact about the tree on 2026-10-24.

**Timing.**
- The render graph (CSM, G-buffer, SSAO, RT shadows, RT GI, lighting) is timed as one entry,
  `"RenderGraph"` (`ohao/render/deferred/deferred_renderer.cpp:485-502`). Only the direct
  passes after it (SSS, SSR, sky, particles, post, gizmo) have their own timers.
- `getPerfStats()["gpu_timings_ms"]` has no caller. Agents see one number, `gpuFrameMs`.
- The 5.0–5.6 ms Cornell frame (1280x720) has never been split by pass.

**The frame's cost knobs.**
- RT shadows are forced to 8 rays a pixel (`deferred_renderer.cpp:410`).
- RT GI is forced to 32 rays a pixel at full resolution, "quality over speed"
  (`deferred_renderer.cpp:455`).
- CSM renders 4 cascades of 2048² every frame, with no culling, even when RT shadows
  replace it in shading (`csm_pass.cpp:153-169`; `deferred_lighting_pass.cpp:168-171`).
- SSS and SSR run every frame, at full resolution, whatever the scene.

**Correctness.**
- **RT shadows trace a hard-coded point light** at (0, 4.5, 0), range 15, not the scene's
  lights (`deferred_renderer.cpp:405-408`). The path tracer's light is also hard-coded, a
  sphere at (0, 4, 0) with intensity 30 (`render_dispatch.cpp:307-309`).
- **RT GI history:**
  - It is blended at the same UV with no reprojection (`rt_gi.rgen:130-132`).
  - Its frame index is the technique's own counter (`rt_gi_technique.cpp:651, 705`); the
    seeded index the renderer sets is ignored.
  - Its binding 5 is the depth buffer standing in as a placeholder
    (`rt_gi_technique.cpp:514`).
- **SSAO is computed and never sampled.** `deferred_lighting.frag` declares no binding 10
  and never tests flag 2 (`deferred_lighting_pass.cpp:167, 361-363`). Its noise samples
  one texel for the whole screen, its decode puts components in [-3, 1], its bias is inert
  (`site/content/units/deferred/ssao.md`). The graph imports its R16F output as R8_UNORM
  (`deferred_renderer.cpp:952`).
- **Sky and particles are lost while SSS is active.** They write the lighting output after
  SSS has copied it, and post reads the SSS copy (`deferred_renderer.cpp:614-617`).
  SSS runs every frame, so they never reached an image.
- **A window host cannot present or capture path-traced frames.** `renderRTPipeline` never
  acquires or presents a swapchain image (`render_dispatch.cpp:262-345`), and
  `LiveHost::capture` refuses non-deferred modes (`live_host.cpp:141-144`).
- **Examples read the frame from 3 frames earlier.** `getPixelSpan()` returns the headless
  ring's copy from `MAX_FRAMES_IN_FLIGHT` frames back (`render_dispatch.cpp:71-76`).
  `cornell_box`, `model_viewer`, `turntable`, `env_demo` and `doctor` read it. Every
  headless frame pays for a ring copy whether anyone reads it or not.
- **glTF node transforms are dropped.** The Assimp path reads `scene->mMeshes` and never
  walks `mRootNode` or reads `mTransformation` (`ohao/scene/asset/model_loader.cpp:386-414`).
  The tinygltf path iterates meshes without nodes (`model_gltf.cpp:163-164`). The
  DamagedHelmet, whose root node rotates it upright, lies on its back in every helmet scene
  and golden.

**Structure.**
- **Draws.** The G-buffer culls on the CPU and issues one `vkCmdDrawIndexed` per draw
  (`gbuffer_pass.cpp:116-150`), with hardware face culling off (`gbuffer_pass.cpp:652`).
  `gpu_cull.comp`, `hiz_generate.comp` and `light_culling.comp` compile but are never
  loaded (`site/content/units/graph/culling.md`).
- **Lights.** Lighting loops over at most 8 lights a pixel (`MAX_LIGHTS 8`,
  `deferred_lighting.frag:29, 217-251`; `render_scene_types.hpp:32`).
- **Toolchain.** Shaders are GLSL through glslc. Slang's compiler ships with the installed
  Vulkan SDK (`C:/VulkanSDK/1.4.321.1/Bin/slangc.exe`) but is unused. No mesh-shader code
  exists.

**Open findings carried from earlier sub-projects** (`docs/ROADMAP.md`, "2b scope"; the
`docs/changes/2026-10-03-ecs-gpu-scene.md` notes):
- RT offline's rare 1-LSB flips;
- `rt_realtime` run-to-run noise (a race in `pt_raygen_realtime.rgen`);
- VUID 03004 and the NRD push-descriptor abort that block validation-clean RT;
- the double tint on untextured materials in RT.

**Benches.** No render benchmark exists. `scene_bench` gates the codec and `serve_bench`
the protocol and the window capture path.

## The frame after 2b

```
per frame (GPU; every pass named, timed, budgeted)
  cull.instances      compute  frustum + previous-frame Hi-Z per instance -> visible list   (scan-compacted, deterministic order)
  gbuffer.early       mesh/indirect  draw last frame's visible set
  hiz.build           compute  depth pyramid of the early pass
  cull.late           compute  instances (and meshlets) occluded early but visible now
  gbuffer.late        mesh/indirect  draw the newly visible
  hiz.final           compute  pyramid for the next frame
  lights.cluster      compute  froxel light lists (16x9 tiles x 24 slices)
  rt.shadows          RT       1 ray per shadowed light (<= 4 a pixel), full res
  rt.shadows.filter   compute  depth/normal-aware spatial filter
  rt.gi               RT       half res, 2 rays a pixel, 1 bounce
  rt.gi.denoise       compute  reprojected temporal + 2 a-trous + joint-bilateral upsample
  ssao                compute  half res, when enabled (multiplies ambient/environment only)
  lighting            fullscreen  clustered lights, RT shadow slots, GI, AO
  sss                 compute  classified tiles only (skin materials)
  ssr                 compute  half res
  sky, particles      raster   into the lit image, before SSS reads it
  post                raster   bloom -> TAA -> tonemap (each when enabled)
  capture.preview     compute  only during an inline capture: box-downscale to <= maxSize
  csm                 raster   only without RT shadows (the fallback), GPU-culled
```

Every pass registers in one **pass registry** (name, kind, enabled predicate, budget key):
the graph passes and the direct passes alike. Timing, `getPipelineInfo`, `render.stats`
and `render_bench` all read the registry, so a pass cannot run unmeasured.

## Profiling first: per-pass timestamps and `render_bench`

**Timestamps.** Each registered pass writes a begin and an end timestamp
(`vkCmdWriteTimestamp`, both at `BOTTOM_OF_PIPE`: each marks the moment all earlier work
has finished, so passes never overlap and their sum is at most the frame's span; a
`TOP_OF_PIPE` begin would let a pass's time include the tail of the pass before it) into a
per-frame-slot query pool sized for 64 passes (one pool per frame in flight, so reading
frame N-3's results never waits). Results are read when the slot's fence has signalled,
which the renderer already waits on, so timing adds no stall. `timestampPeriod` converts
ticks to ms, masked to the queue's `timestampValidBits`. A frame's timings are kept in a
120-frame ring for `render.stats`. Timing is always on; its cost (2 queries a pass) is
part of the frame budget's margin. Names come from the pass registry
(`ohao/render/frame/pass_registry.hpp`, in frame order); a name outside it is not timed
and is logged once as an error.

**`render_bench`** (`tools/bench/render_bench.cpp`, CTest label `bench`, GPU) renders fixed
scenes headless for 5 warm-up and 100 timed frames. Each frame has its own timestamps,
read synchronously after its fence (the bench may wait; the engine never does). It reports
per-pass and per-frame median and p95 to `build/render_bench.json` in the shared summary
shape (`{suite, passed, failed, failures, noisy}` plus `scenes: [{name, frame: {median,
p95, budget}, span, cpu, passes: [{name, median, p95, frames, budget}]}]` and `budgets:
[{scene, name, measure, passes?, budget, gatedFrom, measured, status}]`). `frame` is the
frame's rendering on the GPU, first pass to the start of the final copy out (`present` /
`readback`), the quantity the frame budgets bound; `span` includes the copy. A budget
measures the frame, the CPU, a CPU ratio, or a group of passes (the sum of their medians;
`x.*` is every pass named `x.<something>`). With `--check tests/bench/render_budgets.json`
it fails on any gated budget missed; `--evaluate RESULTS` judges recorded results without
a GPU (test 2).

| Scene | Content | Settings |
|---|---|---|
| `cornell_720p` | `assets/scenes/cornell_box.scene.json` at 1280x720 | Hybrid deferred: RT shadows + RT GI on, post off. The configuration `serve_bench`'s window budgets measure. |
| `helmet_game_720p` | `assets/scenes/helmet_game.scene.json` at 1280x720 | `ohao_game`'s settings: TAA, SSAO, bloom on, exposure 1 |
| `helmet_game_1080p` | the same at 1920x1080 | the same |
| `bench100k_1080p` | 100,002 entities: 20 primitive meshes instanced 100,000 times in a 400 m grid, 1,000 point lights (4 casting shadows), a sun | Post on. Built by `tools/bench/bench_world.hpp`'s builder, extended with meshes and lights. |
| `bench10k_1080p` | the same at 10,002 entities | For the CPU-submission scaling ratio |

**Noise.** The bench follows spec 4's measurement rules (calibration, a load record,
`noisy` exit 2, `check.py` reruns). It adds a **GPU calibration kernel**
(`shaders/bench/gpu_calibrate.slang` since step 6): a fixed compute dispatch (an integer hash over 1 MiB,
8,192 rounds per element, 2^31 in all: about 1.6 ms on the reference GPU), timed by
timestamps before and after the timed frames (best of five after two warm-ups), against
`build/render_bench_reference.json` written by `--calibrate`. Either calibration more than
25% slower than its reference, or other processes over half a core, makes the run `noisy`.
Other processes' GPU use is not sampled: `nvidia-smi --query-compute-apps` reports their
memory, not their utilisation, so the calibration kernel is the GPU contention check.

**Machine quiet.** `tools/bench/wait_quiet.py` waits until the other processes together
use less than half a core for a minute (spec 4's step 11 used a scratch version). A step
whose acceptance needs clean runs calls it first, as a background command, and records the
load it saw.

## Frame cost: the deferred budget

The target follows from Frank's two rulings: `window_readback_ms` ≤ 2 ms, which is the
capture frame's fence wait plus the copy out, and `window_capture_inline_ms` ≤ 20 ms. The
copy out of host-cached staging memory measured about 0.3 ms in sub-project 4. So the
`cornell_720p` frame must take **≤ 1.50 ms of GPU time**, a 3.6x cut from today's
5.0–5.6 ms. A 0.10 ms reserve leaves the readback at ≤ 1.9 ms.

Per-pass budgets (`render_bench`, `cornell_720p`, median GPU ms). They sum to the frame
budget:

| Pass | Budget | Today | How it gets there |
|---|---|---|---|
| `cull.*` + `hiz.*` (all four) | 0.10 | — (CPU) | One dispatch per pass over a few hundred instances |
| `gbuffer.early` + `gbuffer.late` | 0.10 (was 0.15) | in "RenderGraph" | Indirect / mesh draws, face culling on |
| `csm` | 0.00 | in "RenderGraph" | Not rendered when RT shadows are on |
| `rt.shadows` | 0.30 (was 0.20) | in "RenderGraph" | 1 ray per shadowed light, not 8; scene lights, not a hard-coded one; Cornell's 8 shadowed lights trace 4 a pixel |
| `rt.shadows.filter` | 0.08 (was 0.05) | — | 5x5 depth/normal-aware, one pass; memory-bound over 8 light channels |
| `rt.gi` | 0.30 | in "RenderGraph" | Half resolution (1/4 the pixels), 2 rays, not 32 (1/64 of the rays) |
| `rt.gi.denoise` | 0.20 | — | Temporal (reprojected), 2 à-trous iterations at half res, joint-bilateral upsample |
| `ssao` | 0.00 (off) / 0.08 (on) | in "RenderGraph" | Half res; off in this configuration |
| `lights.cluster` | 0.03 | — | 3,456 froxels, brute force over few lights |
| `lighting` | 0.12 | in "RenderGraph" | Cluster lookup instead of a capped loop |
| `sss` | 0.02 | timed | Classified tiles; Cornell has no skin, so zero tiles |
| `ssr` | 0.04 (was 0.10) | timed | Half resolution |
| `sky` + `particles` | 0.03 (was 0.05) | timed | Unchanged work |
| `post` (tonemap only here) | 0.05 | timed | Unchanged |
| reserve (barriers, clears, timestamps) | 0.13 | — | |
| **frame** | **1.50** | 5.0–5.6 | |

*Frank's ruling, 2026-10-25 02:40 (2b step 7):* `rt.shadows` 0.20 → 0.30 and
`rt.shadows.filter` 0.05 → 0.08, taken from `ssr` (0.10 → 0.04), the G-buffer
(0.15 → 0.10) and sky + particles (0.05 → 0.03). The frame stays 1.50 ms; no light cut,
no half-resolution shadows. Step 7 measured `cornell_720p` at 0.252 and 0.061 ms.

Other scenes (frame median GPU ms):

| Scene | Frame budget | Note |
|---|---|---|
| `helmet_game_720p` | 3.0 | Post on (TAA, SSAO, bloom) |
| `helmet_game_1080p` | 5.0 | The game at its common resolution |
| `bench100k_1080p` | 8.0 | 100k instances, 1,000 lights, 4 shadowed |
| CPU record + submit, `bench100k_1080p` / `bench10k_1080p` | ≤ 1.2x | Flat submission: the GPU decides what to draw |
| CPU record + submit, `bench100k_1080p` | 0.5 ms | |

**How budgets are gated.** `tests/bench/render_budgets.json` holds every budget above,
each tagged with the step that delivers it (`"gatedFrom": 7`, for example). `render_bench
--check` gates the budgets whose step is done, and reports the rest as `pending` with
their measurements. A step is done when its budgets pass five consecutive clean runs. If a
step's measurement shows a per-pass allocation is infeasible while the scene's frame
budget holds, the step may move budget between passes of that scene, in the same commit,
saying so in its change record. It never raises a frame budget, a capture budget or a
total. A missed frame budget goes to Frank with the measurements and the options (spec 4's
rule).

## Capture budgets

Unchanged from spec 4. Frank ruled on 2026-10-24 that they stay; they are 2b's to meet.

| `serve_bench` op | Budget | Clean run, 2026-10-24 | 2b's plan |
|---|---|---|---|
| `window_readback_ms` | 2.0 | 5.34 | The frame at ≤ 1.50 ms, plus the copy |
| `window_capture_inline_ms` | 20.0 | 34.3 | See below |
| `window_capture_png_ms` | 30.0 | 22.7 | Follows the frame |
| `window_step_present_ms` | 8.0 | 0.48 | Unchanged |

The inline capture, budgeted step by step (median ms). The bench issues it right after
five `world.step` calls, whose frames are still in flight:

| Part | Budget | Today | How |
|---|---|---|---|
| Waiting for the frames already queued (at most `MAX_FRAMES_IN_FLIGHT` = 3) | 4.5 | 10–15 | 3 frames at ≤ 1.5 ms |
| Temporal reset | 0.1 | `vkDeviceWaitIdle` | Reset history images with clears in the capture's own command buffer: no idle wait beyond queue order |
| The capture frame and its readback | 1.9 | 5.3 + | As `window_readback` |
| Preview downscale | 0.1 | 3 (CPU) | `capture.preview`: a GPU box filter to ≤ `maxSize`, in the capture frame |
| Preview readback (768x432 RGBA8, 1.3 MB) | 0.2 | — | Only the preview is copied when no PNG is asked for |
| JPEG encode, 768x432 | 4.0 | ~3.5 | Unchanged (stb, quality 85) |
| base64, reply, pipe | 0.5 | | |
| **total** | **≤ 11.3** (budget 20) | 34.3 | |

## GPU-driven geometry

**Instance culling.** One compute pass reads `RenderScene`'s instance table: world
matrix, local AABB, mesh, visibility and render-layer bits. It tests each instance against
the frustum and, in the late pass, against the Hi-Z pyramid (the AABB's screen rectangle
at the mip level where it covers at most 4 texels, compared conservatively). Visible
instances are written to a compacted list.

**Deterministic compaction.** An atomic-append list orders visible instances by thread
timing. Equal-depth fragments then win in a run-dependent order, and goldens flip. 2b
compacts with a **prefix sum** instead: one pass marks visibility, a work-efficient scan
(Blelloch, `subgroupExclusiveAdd` within a subgroup) gives each visible instance its
output slot, and a third pass scatters. Output order is instance order, every run. The
same rule holds for every list the GPU builds (meshlets, cluster light lists): no
order-dependent atomics in any output path.

**Two-pass occlusion** (the standard formulation):
- *Early pass:* draw the instances visible last frame, build the Hi-Z pyramid from that
  depth (`hiz.build`).
- *Late pass:* cull everything against the new pyramid; draw what was occluded last frame
  but is visible now.
- *Final:* rebuild the pyramid for the next frame (`hiz.final`).

A temporal reset (every lockstep capture) empties "visible last frame", so the first frame
draws the frustum-visible set in the late pass. The result is correct, and deterministic
because it depends only on the frame sequence since the reset.

**Indirect draws.** The vertex path emits `VkDrawIndexedIndirectCommand`s per visible
instance and submesh (one per material slot), drawn with
`vkCmdDrawIndexedIndirectCount` per pipeline. Draw IDs index the instance table, so
materials and transforms come from the same tables as today. The CPU records the same
handful of commands whatever the instance count: that is the flat-submission budget.

**Shadows.** The CSM fallback culls per cascade with the same instance pass (frustum only)
and draws indirectly. RT shadows need no raster culling: the TLAS holds every instance.

**Face culling** is enabled for opaque materials marked one-sided. Double-sided materials
keep it off.

## Meshlets and mesh shaders

**Build.** At mesh registration, every mesh is split into meshlets of at most 64 vertices
and 124 triangles. The greedy builder lives in-repo (`ohao/render/scene/meshlet_builder.*`):
it walks triangles in index order, adds the neighbour that shares the most vertices, and
starts a new meshlet at the limits. Each meshlet stores a bounding sphere and a normal cone
for backface culling. The builder is deterministic and adds no dependency; meshoptimizer
would cluster better but needs the network and a new dependency (spec 4's rule), and the
builder sits behind one function so it can be swapped. Meshlet data (vertex indices,
packed local triangle indices, bounds) lives in two more arenas beside the vertex and
index arenas. BLAS building keeps using the original triangles.

**Draw.**
- **With `VK_EXT_mesh_shader`** (the reference RTX 5070 Laptop GPU has it): a task shader
  per visible instance tests its meshlets (frustum, cone, Hi-Z) and launches mesh shader
  groups for the survivors.
- **Without it:** a compute pass does the same per-meshlet culling and writes one indirect
  draw per surviving meshlet; a vertex shader pulls vertices through the meshlet's index
  list.
- Both paths read the same meshlet data and share the culling code (one Slang module).
  A test requires their images to be identical.

**When.** Meshes under 256 triangles skip meshlets and stay on the instance path; their
per-meshlet overhead would cost more than it saves. Settings select
`geometry: auto | meshlets | instances`; the default is `auto`.

## Clustered lighting

**Froxels.** 16x9 screen tiles × 24 exponential depth slices (3,456 clusters at any
resolution; near 0.1 m, far from the camera), rebuilt every frame by `lights.cluster`.

**Build.** One workgroup per cluster tests every light's bounding sphere (spot: cone
bounds) against the cluster's AABB, in light-index order, and writes up to 256 light
indices per cluster. Ordered loops, no atomics across workgroups, so the output is
deterministic. Brute force up to 4,096 lights, measured inside the 0.03 ms budget at
`cornell_720p` and gated at 1,000 lights in `bench100k_1080p`. A hierarchical pre-pass for
larger counts is later work.

**Lighting.** Lighting, particles and the RT GI hit shading read the lists. The light table
loses its 8-light cap: `MAX_LIGHTS` becomes the table's capacity, 65,536, in a storage
buffer. A cluster that overflows 256 lights keeps the first 256 and counts the overflow in
`render.stats`, so it is visible, not silent.

**Exactness.** A scene with ≤ 8 lights renders identically to today's loop (`max_abs` 0),
because each pixel sums the same lights in the same order.

## RT shadows

**Lights.**
- Shadows come from the scene's lights. The hard-coded point light at (0, 4.5, 0) is
  deleted.
- `Light` gains `castShadows` (bool, default true: today every light uses the RT mask).
- The path tracer's hard-coded sphere light is replaced by the scene's lights the same way.
- *As built in step 4:* the light table gives the first 4 casting raster lights, in light
  order, a channel of an RGBA8 mask (`LightData::params.w`: -1 never, 0..3 the channel, 4
  casts but no channel; `RenderScene::shadowLights()`); lighting reads each light's own
  channel. A scene without lights shadows the fallback sun in channel 0. The 8 rays a
  pixel are shared by the lights (`max(1, 8 / n)` each) until step 7 replaces them with 1
  ray a light and the filter; per-pixel importance and clusters are step 7's and 13's.
  *Correction:* the path tracer had no hard-coded light in use — `render()` took a light it
  never read and the shaders already lit from the scene's light table; the dead
  parameters are deleted. `castShadows` is a raster-frame control: the path tracer, the
  ground truth, always traces visibility.

**Per pixel.**
- Up to 4 shadowed lights from the pixel's cluster, chosen by a deterministic importance
  (intensity over squared distance; ties by light index).
- One ray each toward a point sampled on the light's area (`radius`; directional lights
  by angular size).
- The sample is a hash of (render seed, frame index since reset, pixel, light slot). It
  changes per frame so temporal accumulation converges, and repeats exactly after a reset.
- Results go to an RGBA8 image, one channel per light slot. *As built (step 7):* one
  RG32UI image of eight 8-bit channels, one per shadow-casting light (see "Build order",
  step 7).
- Lights beyond 4 a pixel shade unshadowed. A deliberate limit, and `render.stats`
  reports how many pixels hit it.

**Filter.** `rt.shadows.filter` is one 5x5 depth- and normal-aware pass. In realtime and
over multi-frame captures, a reprojected temporal accumulation (velocity buffer,
disocclusion by depth and normal) converges the penumbra.

**CSM** renders only when RT shadows are off or unsupported. It is the raster fallback,
GPU-culled per cascade.

## RT GI

**Trace.** Half resolution. Two cosine-distributed rays a pixel, one bounce (as today).

**Hit shading.** Unchanged in kind:
- the brightest 4 lights by intensity, no shadow ray at the hit;
- the environment on a miss.

Quality upgrades (shadowed hit lighting, ReSTIR GI, probes) are later; they reuse this
pass.

**Denoise.**
- *Temporal accumulation* with reprojection by the velocity buffer and a depth/normal
  disocclusion test, history clamped to the neighbourhood's range. This replaces today's
  same-UV blend.
- *Two à-trous iterations* at half resolution, edge-stopped by depth and normal.
- *A joint-bilateral upsample* to full resolution.

**History** resets on every temporal reset (lockstep captures), so frame N after a reset
is identical run to run. The frame index is the renderer's frame count since the reset;
the technique's private counter is deleted. Binding 5 gets the real resource it was meant
to have, not the depth placeholder. *Correction (step 4):* binding 5 ("the current
frame's direct light") cannot have one — GI traces before lighting, and no shader read
it — so it is deleted; step 8 chooses the rebuilt pass's inputs.

**Image quality.** A one-frame lockstep capture (the default for deferred without TAA) is
smoother and less converged than today's 32-ray image. A capture with `frames: 8`
converges through the temporal accumulation. That is the trade the budget asks for, and it
is the right default for an agent's frequent looks. The goldens are re-baselined in their
own commit, with the before/after statistics.

## Reference quality

*Frank's ruling, 2026-10-24 18:01:* the live frame takes the industry-standard approach
above (1 shadow ray a light, filtered; half-resolution low-ray GI, denoised and
accumulated), and a high-sample **reference mode** stands beside it for goldens and
ground-truth images. Budgets unchanged.

**What it is.** One switch, `quality: realtime | reference` (default `realtime`), on the
hybrid renderer:
- *RT shadows:* every light with `castShadows` (no 4-light cap), 8 rays each a frame
  toward points on its area; no spatial filter, no reprojection.
- *RT GI:* full resolution, 32 cosine rays a pixel a frame, one bounce as in realtime;
  no à-trous, no upsample, no reprojection.
- *Accumulation:* after the capture's temporal reset, frame k's shadow mask and GI are
  the plain running mean of frames 1..k (no clamp, no history rejection: the camera does
  not move within a capture). A `frames: N` reference capture is N×8 shadow rays a light
  and N×32 GI rays a pixel.
- Everything else (raster primary visibility, materials, SSAO, post) is the realtime
  frame's, so a realtime capture's difference from the reference isolates the shadow and
  GI approximations.

The path tracer (`rt_offline`) stays the full ground truth for light transport; the
reference mode is the hybrid frame's own converged answer, which the realtime frame is
judged against.

**Reached through** `render.settings {set: {quality: "reference"}}`, a capture's own
`quality` param (which overrides it for that capture), and the CLI:
`ohao run SCENE --render OUT --quality reference`. The examples take a `reference` flag
beside `deferred`.

**Cost.** Unbudgeted, like `rtGI.rays: 32`: the frame costs what it costs, and
`render_bench` measures the realtime frame only. A reference capture of `cornell_720p` at
`frames: 8` is expected in the tens of milliseconds a frame; the change record measures
it.

**Goldens.** Each hybrid scene with RT shadows or GI gets a reference golden beside its
realtime one (`cornell_box_reference`, `helmet_game_reference` through `ohao run`), exact
(max_abs 0), since the accumulation is a deterministic mean of seeded samples.

## SSAO

Wire it in, do not remove it. The raster fallback has no other ambient occlusion, and the
hybrid path still needs AO on the environment/ambient term that RT GI does not cover.

**Fixes.**
- Interleaved 4x4 noise: each pixel reads texel `(x mod 4, y mod 4)`, not (0, 0).
- The noise decode into [-1, 1] with z = 0.
- The bias wired to the shader.
- The graph import at the right format (R16F).
- Half resolution, upsampled with depth awareness.
- The noise is seeded from the render seed, so it stays deterministic.

**Application.** Lighting samples AO (binding 10, flag 2) and multiplies only the
ambient/environment term:
- not direct light, which RT shadows or CSM occlude;
- not RT GI, whose rays already see occluders.

The `ssao` post toggle keeps its meaning (off by default). The goldens that enable it
change, and are re-baselined in their own commit.

## Screen-space passes and order

- **Sky and particles move before SSS.** They draw into the lit image before SSS reads it.
  This fixes the lost sky and particles.
- **SSS** runs only on 16x16 tiles that contain skin-flagged materials. A classification
  pass from the G-buffer's material IDs writes an indirect dispatch. No skin, no work.
  *As built (step 9):* the flag is a material's `subsurface` strength, and the G-buffer
  pass itself flags the tiles (no classification pass; the G-buffer has no material IDs).
- **SSR** runs at half resolution with a depth-aware upsample.
- `render.settings` can turn each of the two off.

## Path-traced frames in a window

**Present.** `renderRTPipeline` gains the deferred path's swapchain handling:
- acquire an image;
- blit the tonemapped PT output (RGBA8) into it, the same blit and layout transitions the
  deferred path uses, with the requested-readback copy inside it;
- present, with the same per-slot semaphores.

**Live host.**
- `rt_realtime` presents every frame.
- `rt_offline` presents its progressive accumulation: each sample pass is a frame, so a
  human watches it converge.

**Capture.**
- `LiveHost::capture` accepts RT modes.
- In lockstep it runs `captureFrame` with `spp` frames, presenting each. The capture is
  cancellable between frames (spec 4's cancel flag) and sends progress every 250 ms.
- It is deterministic, equal to a headless capture of the same world and settings.
- In realtime it returns the next presented frame (`deterministic: false`), as deferred
  does.
- **Window size only,** as for deferred. The CPU denoisers (OIDN when built) run on the
  readback as they do headless.

**Measured, not gated** in 2b: `serve_bench` records `window_capture_rt_inline_ms`
(Cornell, 1280x720, `rt_offline`, 16 spp, `denoise: none`). It becomes a budget by Frank's
ruling once measured.

## Readback: the frame just rendered

The headless ring (every frame copied into its slot's staging buffer, read
`MAX_FRAMES_IN_FLIGHT` frames later) is deleted.

**One way to read.** Every reader uses `requestReadback()` before the `render()` it wants
and `readback()` after, which waits on that frame's fence only:
- the examples (`cornell_box`, `model_viewer`, `turntable`, `env_demo`);
- `ohao doctor`'s frame check;
- the CPU-denoiser capture path: OIDN and NRD run inside `readback()`, on the requested
  frame.

`getPixelSpan()`, `getPixels()` and `readbackCopyCount()` are removed.

**Cost.** Headless frames stop paying for an unread copy every frame.

**Goldens.** The examples then write their last frame instead of frame N-3:
- `cornell_box` deferred: frame 10, not 7;
- `model_viewer` deferred: frame 30, not 27;
- the RT examples' last sample pass: `spp` frames, the last read, which is the frame the
  ring delivered after `spp + 3` (so the path-traced goldens do not move).

The deferred goldens are re-baselined in one commit. The `cli_*_deferred` manifest entries move
to `--frames 10` and `--frames 30`, so the CLI and the examples keep sharing one golden
each.

## glTF node transforms

**Assimp path.** The loader walks `mRootNode` depth-first, accumulating `mTransformation`,
and emits each (node, mesh) pair into the model's one vertex and index list (submeshes
still come from material runs at upload):
- positions transformed by the node's matrix;
- normals by the inverse transpose of its upper 3x3, renormalised;
- tangents by the upper 3x3 itself, renormalised: a tangent lies in the surface, so it
  transforms as a direction, not as a normal (*correction*: this section said the inverse
  transpose for both); the tangent's handedness `w` is negated where the determinant is
  negative;
- triangle winding flipped where the determinant is negative. `repairMeshNormals`
  recomputes normals from the winding after every load, so a mirrored mesh without the
  flip would come out lit from inside.

**Instancing.** A mesh referenced by two nodes is emitted twice; instancing within a
model is later work (5's importer).

**tinygltf.** The second glTF loader (`Model::loadFromGLTF`, used only by `env_demo` and
`turntable`) is deleted with its dependency (`external/cmake/tinygltf.cmake`) and the
`MaterialData` glTF fields (`baseColorFactor` … `alphaMode`, `doubleSided`) only it wrote.
Those examples load through `ModelLoader` like everything else: one glTF path, one place to
be right.

**Up axis.** `SceneFramer` used to guess the up axis from the bounds (Y when the Y extent
is at least the Z extent). The upright helmet is 1.80 tall and 2.00 deep, so the guess
tipped it onto its back again. A glTF model is Y up by its specification: the framer's
`computeFraming(const Model&)` forces Y for a `.gltf`/`.glb` source, and `isYUp(const
Model&)` gives `env_demo` and `turntable` (which place the model themselves) the same rule
instead of their inline copies of the guess.

**Test.** A checked-in fixture `tests/assets/node_transforms.gltf` (a rotated and scaled
root, a translated child, a mirrored node, all three sharing one mesh) pins the result.

**Image changes.** The helmet stands upright. Every helmet golden is re-baselined in its
own commit (`damaged_helmet*`, `cli_damaged_helmet*`, `cli_helmet_game_deferred`). Each
helmet scene's camera is checked by looking at the new image. If the upright helmet leaves
the frame, the scene file's camera moves, and the commit says so. *As built:* it did
leave the frame (the chin was cut at the bottom of the deferred images): the framer's
fixed distance of 1.7 S cannot fit a model whose height is its full 18 units with its
face 9 units in front of the centre. The framer now backs the camera off until the
model's height plus 10% fits the vertical field of view at its nearest face (never
nearer than 1.7 S, so every model that fit keeps its camera), and both helmet scene
files' cameras move to the framed `[0, -6.891139, 30.534174]` with the code that moves
them, so every commit's gates stay green; the golden commit records it.

## Determinism

Spec 4's contract holds and extends to everything 2b adds.

**No order-dependent output.**
- Every GPU-built list (visible instances, meshlets, cluster lights, tile classes) is
  compacted by prefix sum or built by ordered loops.
- No floating-point atomics write any image or list that output depends on.
- A shader lint (`reflect_tests`, beside the nondeterminism lint) rejects
  `atomicAdd`/`InterlockedAdd` on floats under `shaders/`.

**Seeded RT.**
- The hybrid RT passes and the path tracer seed from (render seed, frame index since the
  last temporal reset, pixel, sample).
- `m_totalTime` and private counters seed nothing.
- *As built (step 4):* RT shadows and GI hash (render seed, `DeferredRenderer`'s frames
  since `resetTemporalState`, pixel, light/sample, dimension) with PCG; the path tracer's
  sample index already started at the render seed after a reset, and now does for a fresh
  tracer too.

**Exactness.**
- The deferred goldens stay exact, and the hybrid-RT configurations join them:
  `deferred_determinism_gpu_test` extends to RT shadows, RT GI and every new pass, across
  fresh renderers, processes and repeated captures.
- The `rt_realtime` race and RT offline's 1-LSB flips are fixed. Their goldens then become
  exact: tolerances are only ever tightened.
- *As built (step 5), corrected:* the race is fixed. RT offline's flips are not an engine
  defect: the driver's BLAS builder lays out a mesh's BVH differently run to run (identical
  input, different serialized bytes), and a ray that hits exactly on an edge two triangles
  share takes whichever the traversal reaches first. Every mesh has such edges (Cornell's
  walls are two-triangle quads). Vulkan has no deterministic-build flag, so path-traced
  output keeps that floor — 1 LSB on a few pixels: the helmet flipped in 5 of 83 renders;
  Cornell never flipped run to run (78 renders), but its scene loaded from the file by
  `ohao run` builds another BVH than the example's and renders 10 pixels 1 off — and the
  RT goldens are tightened to it (max_abs 1, diff_frac 0.0005), not to exact.
  Cross-process and cross-host comparisons of path-traced output (test 11's
  window-vs-headless capture included) hold to that floor.

**Intentional image changes.**
- Each is its own commit (`golden(render): <what changed and why>`), with the old→new
  statistics from `render_golden.py --json` (max_abs, diff fraction, regions) in the commit
  body and the change record, after looking at the heatmap and crop.
- A golden is never updated in the same commit as an unrelated change, and a tolerance is
  never widened.

## The agent-facing surface

**`render.stats`** `{frames = 1, passes = true}` →
`{frame, frames, gpuMs, cpuMs, passes: [["gbuffer.early", 0.071], ...], over?: ["rt.gi"], lightsOverflow?, shadowLightsDropped?}`.

- `frames` (1–120) gives medians over the last frames the host rendered (`frames` in the
  result: how many there were). A pass's median is over the frames that ran it.
- Passes are `[name, ms]` pairs, in frame order, in ms rounded to the microsecond: about
  20 bytes a pass, so a whole frame report is under 1 KB.
- Built in step 1 without `over` (no host knows its scene's budget class until step 15),
  `lightsOverflow` (step 13) and `shadowLightsDropped` (step 7).
- `over` lists the passes over their `render_budgets.json` budget for the current scene
  class, when the host knows it (the bench scenes and any scene with a `budget` hint).
- Over MCP it is `render_stats`. GPU hosts only; the CLI host is `E_STATE`.

**`render.settings`** `{set?: RenderSettings}` → the current `RenderSettings`. One reflected
params struct, so its JSON Schema, limits and docs reach agents in `tools/list` and the
catalogue:
- `quality: realtime | reference` ("Reference quality");
- `rtShadows {enabled, maxLights 0–4}`;
- `rtGI {enabled, resolution: half | full, rays 1–32}`;
- `ssao`, `ssr`, `sss` (bools);
- `geometry: auto | meshlets | instances`;
- `occlusionCulling` (bool);
- `csm {cascades 1–4, size 512–4096}`;
- `exposure`.

Settings changes:
- are host state: not saved in scene files (scene files hold no render settings, spec 3);
- count as mutating, so a lockstep window redraws;
- apply to captures as their defaults; a capture's own params still override.

**Limits.** The defaults are the budgeted configuration. Raising `rtGI.rays` to 32 is
allowed, and the frame then costs what it costs. Limits keep a setting from making a frame
unbounded.

**Captures.** `render.capture` results gain `gpuMs`. `timings: true` adds the capture
frame's pass list.

**`engine.info`** gains `gpu: {name, driver, rayTracing, meshShaders}`.

**Budgets as data.**
- `render_bench` and `serve_bench` write their budgets and verdicts to JSON.
- `check.json` carries each bench's failures, named by pass.
- An agent working on the renderer reads one file to know which pass is over.

**Token cost.** `tools/list` grows by two tools (about 1.6 KB). It is gated at 24 KB and
measured at 14,543 B today.

## Slang

**Where.**
- New shaders (culling, Hi-Z, meshlets, clustering, denoisers, the preview downscale,
  the GPU calibration kernel) are written in Slang.
- Existing GLSL moves to Slang only when its pass is rewritten anyway: RT shadows and RT
  GI are rewritten in 2b, so they move.

**Build.** `shaders/CMakeLists.txt` finds `slangc` (the Vulkan SDK's `Bin`, then `PATH`)
and compiles each `*.slang` to SPIR-V for Vulkan 1.3 (`-target spirv -profile
spirv_1_6`). The naming rule is the flattened path, as for GLSL:
`cull/instances.slang` → `cull_instances.slang.spv`. A module with several entry points
emits `<flattened>.<entry>.spv`.

**Doctor.** `ohao doctor` learns `.slang` and its `import`/`#include` dependencies for
`shaders.fresh`. `tools.slangc` joins `tools.glslc`.

**Shared code.** GLSL includes and Slang modules do not share source. Constants shared
with C++ live in one header-like file generated for both, `shaders/shared/layout.json`
→ `.glsl`, `.slang` and `.hpp`, so a struct layout cannot diverge.

## Performance budgets (summary)

| Bench | Budget | Gated from step |
|---|---|---|
| `render_bench` `cornell_720p` frame | ≤ 1.50 ms GPU (per-pass table above) | 9 |
| `render_bench` `helmet_game_720p` frame | ≤ 3.0 ms GPU | 9 |
| `render_bench` `helmet_game_1080p` frame | ≤ 5.0 ms GPU | 9 |
| `render_bench` `bench100k_1080p` frame | ≤ 8.0 ms GPU | 13 |
| `render_bench` CPU record + submit, 100k | ≤ 0.5 ms; ≤ 1.2x of 10k | 11 |
| `render_bench` `lights.cluster`, 1,000 lights, 1080p | ≤ 0.10 ms | 13 |
| `render_bench` meshlet build, 1M triangles (CPU) | ≤ 60 ms | 12 |
| `serve_bench` `window_readback_ms` | ≤ 2 ms (unchanged) | 9 |
| `serve_bench` `window_capture_inline_ms` | ≤ 20 ms (unchanged) | 10 |
| `serve_bench` `window_capture_png_ms`, `window_step_present_ms`, CPU ops | unchanged | — |
| `serve_bench` `window_capture_rt_inline_ms` | recorded, not gated | 14 |
| `scene_bench` | unchanged | — |

Reference machine: Frank's laptop, NVIDIA GeForce RTX 5070 Laptop GPU (driver 621.1,
Vulkan 1.4.357), Windows 11.

## Rules for the step chain (lessons from sub-project 4)

The steps run unattended, one after another. These rules come from what cost time in 4:

1. **One plain command per tool call.**
   - The harness refuses chained commands (`&&`, `;`) and some pipes.
   - Run each command alone, from the repo root.
   - Write every gate so it is one argv command that writes its own outputs under
     `build/`.
2. **No background subagents.** The background wait ceiling (about 10 minutes) killed
   step 11's first attempt mid-work.
   - Do the work in the foreground.
   - A long command (`check.py --all`, a bench wait) runs as a background shell command
     whose completion notifies the session.
   - Foreground subagents are fine for search and for change records. Read what they write
     against the code before committing it.
3. **Noisy benches are invalid, not failures.**
   - A noisy run says nothing about the code. Rerun it when the machine is quiet
     (`tools/bench/wait_quiet.py`), and record the load it saw.
   - Unreal Editor's memory traffic alone made `scene_bench`'s memory probe flag most
     rounds in 4.
   - Never relax a budget or a contention rule to get a pass.
4. **`-j6` at most, no parallel builds, GPU tests one at a time.** Heavier load has
   blue-screened this machine.
5. **Files.**
   - Delete with `rm <exact path>`, one file per command; `mv` and `git clean` are refused.
   - Never put throwaway code under a globbed source directory (`ohao/`, `shaders/`): the
     build compiles it.
   - `.claude/` and `.mcp.json` cannot be written, and the `claude` CLI cannot be run;
     those go to Frank.
6. **Python rewrites** read a file into a variable before opening it for writing. A
   one-liner once emptied two test files.
7. **Goldens are looked at before they are updated.** An intentional change gets its own
   commit with its statistics.
8. **A wedged GPU after a crash** (device lost `-4`, 30 s renders) is cleared by a reboot,
   not by code; `ohao doctor`'s `gpu.wedged` says so. Stop and report it.
9. **Every step ends the same way:** build; the step's tests; `python tools/check.py
   --all` with every functional test green; the benches the step gates, clean or reported
   noisy; a change record `docs/changes/YYYY-MM-DD-renderer-stepN.md`; local commits.
   Never push, merge, rebase, reset or checkout.

## Test plan

Numbered as the build steps reference them.

1. **Pass timing coverage** (`engine_tests`, GPU):
   - every executed pass is in the registry and has a timer, with unique names;
   - the sum of pass times is at most the frame time plus 5%;
   - disabling a pass removes it from the timings.
2. **`render_bench` mechanics** (`check_tests` fixtures, CPU):
   - a noisy fixture is rerun and never counted as a pass;
   - a `pending` budget is reported, not gated;
   - a missed gated budget names its pass.
3. **Readback latency** (`engine_tests`, GPU):
   - in a scene whose spinning cube's angle is a known function of the step, frame N's
     readback equals a fresh renderer's single capture of step N (max_abs 0);
   - no headless frame copies unless asked (`requestedReadbackCount` equals the requests,
     `readbackCopyCount` is deleted).
4. **Node transforms** (`scene_tests`, CPU): the fixture's vertices, normals and winding
   match the expected values within 1e-6; the helmet's bounding box is upright.
5. **SSAO** (`deferred_determinism_gpu_test` + a focused GPU test):
   - AO on vs off differ only where the ambient term is occluded (a scene lit only by
     ambient light vs only by a point light: the latter is unchanged);
   - the noise covers 16 distinct texels;
   - deterministic twin renders.
6. **Culling parity** (`cull_parity_gpu_test`):
   - with culling on vs off, scenes with off-screen and fully occluded objects give
     identical images (max_abs 0);
   - two runs give byte-identical visible lists and indirect buffers;
   - occluded objects are not drawn (draw counts).
7. **Meshlets** (`scene_tests` + `cull_parity_gpu_test`):
   - the builder covers every triangle exactly once, within the vertex and triangle
     limits, with bounds containing their vertices and cones containing their normals;
   - the mesh-shader and fallback paths render identically (max_abs 0).
8. **Clustered lighting:**
   - ≤ 8 lights render identically to the pre-2b loop (max_abs 0, against a golden made
     before the step);
   - 1,000 lights render, with overflow reported;
   - deterministic cluster lists.
9. **RT shadows:**
   - scene lights cast them and `castShadows: false` lights do not;
   - no hard-coded light remains (a lint on the source);
   - the capture after a reset is exact run to run;
   - the CSM fallback renders when RT shadows are off;
   - reference mode: every `castShadows` light is shadowed past the realtime cap of 4, and
     the realtime shadow mask converges toward the reference (mean absolute difference
     from a `frames: 16` reference capture is smaller at realtime frame 16 than at frame 1).
10. **RT GI:**
    - deterministic across fresh renderers and processes;
    - history resets on capture;
    - multi-frame captures converge (the difference between frame 8 and frame 16 is
      under the difference between frame 1 and frame 8);
    - reference mode: full resolution and 32 rays a frame (from `render.stats`' pass list
      and the GI image's extent), and the realtime GI converges toward a `frames: 16`
      reference capture as the shadows do in test 9.
11. **Path tracer in a window** (`check_live.py`):
    - a lockstep RT window capture equals a headless RT capture of the same state
      (max_abs 0 after step 5's fix);
    - `rt_offline` presents progressively;
    - cancel mid-capture leaves the host serving.
12. **Determinism gates:**
    - the five deferred and the hybrid-RT goldens are exact, and so are the reference
      goldens (`cornell_box_reference`, `helmet_game_reference`);
    - RT offline and `rt_realtime` are exact after step 5 (*as built:* to the BVH floor,
      see "Determinism");
    - the float-atomics shader lint.
13. **Validation:** `check_validation.py` adds the hybrid-RT and path-traced scenes, zero
    messages through teardown (VUID 03004 and the NRD push-descriptor path fixed).
14. **Agent surface:**
    - `render.stats` and `render.settings` in `api_tests` (limits, `E_STATE` on the CLI
      host, mutation counting), the serve goldens, `jsonschema` in `check_serve.py`;
    - `tools/list` ≤ 24 KB;
    - `e2e_mcp.py` gains a settings change and a stats read.
15. **Budgets:** `render_bench --check` and `serve_bench --check` pass every gated budget,
    five consecutive clean runs.
16. **Docs:** `check_docs.py` runs the new commands in `docs/agents/ohao-serve.md`
    ("Measure the frame") and the updated recipes; the monograph's renderer units cite the
    new passes.

## Done criteria

All verified by running, none estimated.
- Tests 1–16 pass; every existing suite, golden and gate stays green.
- `python tools/check.py --all` exits 0. With the window budgets met, nothing stands in
  its way.
- Every budget in "Performance budgets" passes five consecutive clean runs, results in the
  change records.
- The roadmap row is marked done; each step has a change record in `docs/changes/`.

## Build order

Each step:
1. implement;
2. build (`C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6`, the
   `shaders` target if shaders changed);
3. run its tests and `python tools/check.py --all`;
4. write `docs/changes/YYYY-MM-DD-renderer-stepN.md`;
5. commit locally.

A step that misses a budget it gates, sees a golden change it cannot explain, or needs
`.claude/`, the network or a dependency not named here, stops and reports instead of
improvising. Intentional image changes are separate commits inside their step.

1. **Profile and gate.**
   - The pass registry; per-pass timestamps for every graph and direct pass.
   - `render_bench` with the `cornell_720p` and `helmet_game_*` scenes, GPU calibration,
     `tests/bench/render_budgets.json` (every budget tagged `gatedFrom`) and
     `tools/bench/wait_quiet.py`.
   - `render.stats` (test 14, stats part).
   - The baseline profile of today's frame, split by pass, recorded in the change record.
     Every later step is judged against it.
   - Tests 1, 2.
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step1.md`):* timestamps both
     at `BOTTOM_OF_PIPE` and `render.stats` without `over` (see the sections above); the
     GPU calibration kernel is GLSL (Slang arrives in step 6, which moves it) and 2^31
     hash rounds, not 2^24 (about 0.01 ms, too short to time); no per-process GPU
     sampling. Test 1 is `engine_tests`' `sp2b_t01`, test 2 `check_tests`' `sp2b_t02_*`
     through `render_bench --evaluate`; `check.py`'s rerun of a noisy bench was already
     covered by `t3_a_noisy_bench_is_rerun_at_most_twice`. Baseline, `cornell_720p`:
     frame 4.87 ms GPU, `rt.gi` 3.95 ms of it.
2. **Readback on request everywhere.**
   - The headless ring deleted; examples, doctor and the CPU-denoiser path on
     `requestReadback`/`readback`.
   - Example goldens re-baselined (own commit); the CLI deferred entries move to
     `--frames 10`/`30`.
   - Test 3.
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step2.md`):* the CPU
     denoisers run inside `readback()`, on the requested frame, so there is no separate
     path for them. `readbackCopyCount` is deleted; the window host counts its own
     requests and reports copies beyond them as unrequested (the gates' 0). The RT
     examples render `spp` frames and read the last, the same sample count as before
     under `--denoise=none`, so the path-traced goldens did not move; only the two
     deferred example goldens were re-baselined. Test 3 resets the temporal history
     before frame N, since RT GI's history otherwise makes frame N of a run differ from
     a fresh renderer's first frame, and checks that step N−3 differs, so a lagged
     readback cannot pass.
3. **glTF node transforms.**
   - The Assimp loader walks nodes; the tinygltf loader deleted, with `env_demo` and
     `turntable` moved to `ModelLoader`.
   - The fixture; the helmet goldens re-baselined, cameras checked (own commit).
   - Test 4.
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step3.md`):* tangents by
     the upper 3x3, not the inverse transpose; the framer forces Y up for glTF
     (`isYUp` for the examples that place models themselves) and fits a deep model's
     height by distance, which moved both helmet scenes' cameras to z 30.53 (see "glTF
     node transforms"). The tinygltf loader took the `MaterialData` glTF fields with it.
     Test 4 is `scene_tests`' two `sp2b_t04_*` tests; the second also checks that the
     framed camera sees all eight corners of the helmet's bounds.
4. **Correctness of what renders.**
   - RT shadows and the path tracer use the scene's lights (`Light.castShadows` added).
   - Sky and particles before SSS; the SSAO graph-import format; RT GI's binding 5; RT
     seeds from the render seed and the frame index since reset; the RT double tint.
   - Goldens re-baselined per change (own commits).
   - Tests 9 (lights part), 12 (seed part).
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step4.md`):* three golden
     re-baselines — scene lights (deferred), seeds (deferred), the double tint
     (path-traced). Moving particles before SSS exposed three particle bugs, fixed: an
     uncleared buffer drawn as live particles (run to run), every instance drawing particle
     0, and a pass that ran with nothing emitted. RT GI's binding 5 is deleted rather than
     filled (see "RT GI"). Test 9: `engine_tests` `sp2b_t09` (a box's shadow falls away from
     the scene light and mirrors with it; `castShadows: false` casts none; slot assignment)
     and `reflect_tests` `sp2b_t09_*` (a lint on literal light positions in `ohao/render`
     and `ohao/gpu`); test 12: `deferred_determinism_gpu_test` `t12` (another seed moves the
     RT noise; the same seed reproduces it). The sky reaching an SSS frame is
     `engine_tests` `sp2b_s4`.
5. **Validation-clean, deterministic RT.**
   - VUID 03004; the NRD push-descriptor abort; the `rt_realtime` race; RT offline's
     1-LSB flips.
   - The float-atomics lint.
   - RT goldens tightened to exact where achieved.
   - Tests 12, 13.
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step5.md`):* VUID 03004 was
     binding 12's variable count (not the layout's highest binding; every set was allocated
     at full capacity anyway); the NRD abort was `VK_KHR_push_descriptor` never enabled
     (NRI resolves `vkCmdPushDescriptorSet` as KHR), then `synchronization2`, NRD's
     `queuedFrameNum` of 1 against 3 frames in flight, NRD's images lacking `SAMPLED`, and
     the compose/cinematic chain's single descriptor sets (now one per frame in flight). The
     chain now runs only in NRD mode, and an `--denoise=nrd` override now reaches the path
     tracer (it never did: the chain ran ungated and the readback copied what it wrote).
     The `rt_realtime` race was the raygen reading `accumBuffer` and `normalAOV` at
     neighbour pixels in the dispatch writing them (temporal reprojection and the in-shader
     spatial denoise): the accumulation is now a ping-pong pair (binding 1 written, binding
     3 the previous frame's, read-only) and the spatial denoise a compute pass after the
     trace (`shaders/rt/pt_spatial_denoise.comp`). RT offline's flips are the BVH floor
     above. `OHAO_VALIDATION=sync` adds the layer's synchronization validation (zero hazards
     on the path tracer). Test 12: `reflect_tests` `sp2b_t12_*` (the lint) and
     `deferred_determinism_gpu_test` `t12 cornell_rt_offline` / `cornell_rt_realtime`
     (within the BVH floor: at most 16 bytes, each off by 1);
     test 13: `check_validation.py`'s four path-traced scenes (offline, realtime, realtime
     with NRD, the helmet) and `scene_view_gpu_test`'s RT runs under validation.
6. **Slang.**
   - `slangc` in `shaders/CMakeLists.txt`; the doctor's `.slang` freshness and
     `tools.slangc`; `shaders/shared/layout.json`.
   - Proof: the GPU calibration kernel is ported to Slang (`render_bench --calibrate`
     rerun).
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step6.md`):* `slangc` is found
     in `$VULKAN_SDK/Bin`, the newest `C:/VulkanSDK/*/Bin`, then `PATH`; its version is
     pinned in `shaders/slangc.version` (2025.11; a mismatch warns at configure and fails
     `tools.slangc` as a warning). Entry points are found by the `[shader("…")]` attribute
     (the CMake rule and the doctor scan the same way) and keep their names in the SPIR-V
     (`-fvk-use-entrypoint-name`); matrices are column-major (`-matrix-layout-column-major`),
     as in GLSL and glm. The shared layout is generated by `tools/gen_layout.py` into
     checked-in files (`shaders/shared/layout.slang`, `layout.glsl`,
     `ohao/gpu/shared_layout.hpp`), kept current by the `check_layout` suite rather than
     generated during the build: the C++ header must exist before any target compiles, and
     the build needs no Python. The calibration kernel is
     `shaders/bench/gpu_calibrate.slang` (entry `calibrate`, sizes and push constants from
     the layout); the GLSL kernel is deleted. Tests: `check_doctor` `s6_*`, `check_layout`.
7. **RT shadows rebuilt.**
   - 1 ray per light, ≤ 4 lights a pixel, the spatial filter, temporal accumulation in
     realtime; CSM only as the fallback.
   - Reference quality, shadow half: the renderer's `quality` switch, the `--quality`
     flag on `ohao run` and the examples' `reference` flag, the capture's `quality`
     param; `cornell_box_reference` and `helmet_game_reference` goldens (own commit).
   - Goldens re-baselined (own commit).
   - Gates `rt.shadows`, `rt.shadows.filter`, `csm`.
   - Test 9.
   - *As built (2026-10-24, `docs/changes/2026-10-24-renderer-step7.md`):* both passes are
     Slang compute (`shaders/rt/shadow_trace.slang`, `shadow_filter.slang`): the trace uses
     inline ray queries, not a ray tracing pipeline (no shader binding table, measured
     cheaper; without `VK_KHR_ray_query` the frame keeps CSM). *Correction:* the output is
     not an RGBA8 image of 4 per-pixel slots but one RG32UI image of eight 8-bit channels,
     one per shadow-casting light of the light buffer (every caster, up to 8, owns a
     channel in `params.w`), dithered so the temporal mean stays unbiased: lighting reads
     one texel, and no per-pixel slot table is needed. Importance is intensity over squared
     distance with ties by index, 0 out of range, wholly behind the surface or (a spot)
     outside its cone. The filter skips the 5x5 kernel for a light the neighbourhood
     agrees on; the temporal weight is 1/(n+1) down to 1/16 with no neighbourhood clamp
     (a moving light trails for up to 16 frames). Reference quality's GI is the realtime
     GI until step 8. Bugs fixed on the way: CSM never shadowed (an OpenGL-depth ortho
     and a comparison sampler read as a depth texture), and a scene with exactly one light
     was treated as the fallback sun. Test 9: `engine_tests` `sp2b_t09_*` (exact after a
     reset, CSM fallback, reference past the cap with `shadowLightsDropped`, a spot outside
     its cone, convergence toward the reference); goldens `cornell_box_reference`,
     `cli_cornell_box_reference`, `cli_helmet_game_reference`. **Budgets:** Cornell's 8
     shadow-casting lights make every pixel trace 4 rays at about 0.048 ms each, plus
     0.06 ms of fixed work, over the original 0.20 / 0.05 ms. Frank's ruling (2026-10-25)
     moved budget inside `cornell_720p` (see "Frame cost"): `rt.shadows` 0.30,
     `rt.shadows.filter` 0.08. A clean run: 0.252 and 0.061 ms, `csm` not run (0): pass.
8. **RT GI rebuilt.**
   - Half resolution, 2 rays, the reprojected temporal and à-trous denoiser, the upsample.
   - Reference quality, GI half: full resolution, 32 rays, the plain running mean; the
     reference goldens re-baselined with it (own commit).
   - Goldens re-baselined (own commit).
   - Gates `rt.gi`, `rt.gi.denoise`.
   - Test 10.
   - *As built (2026-10-25, `docs/changes/2026-10-25-renderer-step8.md`):* Slang compute with
     inline ray queries, as RT shadows (`shaders/rt/gi_trace.slang`, `gi_denoise.slang`);
     the ray tracing pipeline and its three GLSL shaders are deleted, and without
     `VK_KHR_ray_query` the frame has no RT GI. The 2 rays are stratified in azimuth.
     *Corrections:* (1) Hit shading keeps its kind (the 4 brightest lights by intensity, no
     shadow ray, the hit's normal taken as facing the ray) but is made consistent with the
     lighting pass: light colour, the lighting pass's attenuation (inverse square windowed
     by range, a spot's cone) and the 1/pi of a diffuse bounce, which the old shader
     omitted (its bounce was about 3x too bright). The environment a miss reads is level 0:
     the uploaded environment has one mip. (2) With RT GI in the frame, lighting drops its
     diffuse IBL term: GI rays that leave the scene carry the environment, occluded, and
     the unoccluded term counted it twice. (3) "History clamped to the neighbourhood's
     range": the long accumulation (to 32 frames) is clamped to the 3x3 range of a short one
     (to 4 frames), not of the 2-ray samples, whose range a dark corner's zeros would
     collapse. (4) A reference running mean (GI's and RT shadows') holds the reference
     frames in an unbroken run, not every frame since the reset: a realtime frame or new
     images start it again (until then a realtime-to-reference switch without a reset
     blended into a stale mean, and the first reference frame read undefined memory).
     Test 10: `engine_tests` `sp2b_t10_*` (exact across fresh renderers, history reset on
     capture, |8-16| 0.10 < |1-8| 0.34, reference 160x120 at 32 rays without
     `rt.gi.denoise` and realtime 80x60 at 2 with it, realtime toward the reference 0.40 ->
     0.19, a red wall's bleed, the mean restart), and `deferred_determinism_gpu_test`'s
     Cornell 8-frame and reference configs across processes. **Budgets:** `cornell_720p`
     `rt.gi` 0.064-0.066 ms (0.30) and `rt.gi.denoise` 0.143-0.148 ms (0.20), frame
     0.87-0.90 ms (was 4.64); both runs at the step's end were noisy (outside load), so
     the gate's verdict is open.
9. **SSAO wired; SSR and SSS made cheap.**
   - SSAO fixed and applied to the ambient term; SSR at half resolution; SSS on
     classified tiles.
   - Goldens with `ssao` re-baselined (own commit).
   - Gates the `cornell_720p` and `helmet_game_*` frame budgets, and `serve_bench`'s
     `window_readback_ms`.
   - Test 5.
   - *As built (2026-10-25, `docs/changes/2026-10-25-renderer-step9.md`):* SSAO, SSR and SSS
     are Slang compute (`shaders/compute/ssao.slang`, `postprocess/ssr.slang`,
     `postprocess/sss.slang`); their GLSL shaders are deleted. SSAO had never reached a pixel
     (the lighting shader declared no binding 10); it now does, half resolution, horizon
     search in the tangent plane from the G-buffer's world position and normal, the 16
     rotations a seeded permutation of 16 strata packed in the push constants, a 4x4
     depth-aware blur, and lighting's depth-aware upsample multiplying the constant ambient
     and IBL terms. SSR's depth-aware upsample happens where post adds it (the tonemap).
     *Corrections:* (1) "SSS from the G-buffer's material IDs": the G-buffer has none. A
     material gets a `subsurface` strength (MaterialOverride, MaterialDesc, the material
     table's row 2 z, the G-buffer normal's w as a negative), and the G-buffer pass flags a
     fragment's 16x16 tile with atomics into an indirect-dispatch buffer (early fragment
     tests on); no classification pass. The old full-screen SSS keyed on an albedo heuristic
     that took Cornell's red wall for skin. SSS writes the lit image in place. (2) "The
     noise covers 16 distinct texels" is checked on the generator (16 strata, permuted)
     and by the shader's (x mod 4, y mod 4) lookup. (3) Test 5's scenes set the ambient
     with a new `VulkanRenderer::setAmbientIntensity`. The `render.settings` switches for
     SSR and SSS stay in step 15. Test 5: `engine_tests` `sp2b_t05_*` (ambient-only frame
     3191 px darker with SSAO, none brighter; point-lit frame unchanged, max_abs 0; twin
     renders identical) and `deferred_determinism_gpu_test` (`t5` strata, the noise pin);
     an SSS test (0 tiles without `subsurface`, 16 with). **Budgets:** `cornell_720p`
     frame 0.85-0.87 ms (1.50), `sss` 0.004 (0.02), `ssr` 0.008 (0.04), `sky`+`particles`
     0.011 (0.03), `post` 0.030 (0.05), `ssao` not run (0); `helmet_game_720p` 0.94 (3.0),
     `_1080p` 2.06 (5.0); `serve_bench` `window_readback` 1.32 ms (2.0). Every run at the
     step's end was flagged noisy (outside load), so the gate's verdict is open.
10. **The capture path.**
    - The temporal reset by clears, not device idle; the GPU preview downscale; the
      preview-only readback.
    - Gates `window_capture_inline_ms`.
    - *As built (2026-10-25, `docs/changes/2026-10-25-renderer-step10.md`):* the deferred
      `resetAccumulation()` waits for nothing: it marks the next frame to start without
      history (RT shadows, RT GI, TAA) and returns; TAA clears both history images in that
      frame's command buffer, its barrier waiting on earlier frames' TAA writes and reads by
      queue order. The idle existed because TAA's jitter was indexed by the frame-in-flight
      slot (3 of its 16 Halton samples), so a capture had to restart the slots; the jitter is
      now indexed by the frames since the reset, and no frame depends on its slot (the four
      TAA goldens re-baselined for it, own commit). `capture.preview`
      (`shaders/compute/capture_preview.slang`, `CapturePreviewPass`) box-filters the region
      or the whole final image to `render::previewExtent` (one size rule with the CPU's
      `fitWithin`) and writes it packed straight into the frame's host-cached staging buffer:
      the preview readback is that write, with no copy of its own. `requestPreviewReadback`
      / `readbackPreview`; a capture uses it when an inline image is all it needs from a
      deferred frame. *Corrections:* (1) the preview-only path also excludes captures that
      keep the whole frame (tests) and path-traced modes (no `capture.preview`); a capture
      with a PNG `path` keeps the CPU preview from the full frame, so its inline image is the
      same bytes either way (the filters match exactly in the tests). (2) "Waiting for the
      frames already queued" is not inside the reset any more but before the read frame's
      fence: the capture waits for the frames submitted before it (`waitForSubmitted`,
      which queue order imposes anyway) and `readbackMs` times the read frame's fence and copy
      alone, as `window_readback` was defined. **Budgets:** reset 0.0001 ms with 3 frames in
      flight (0.1); `capture.preview` 0.056 ms at 768x432 (0.1, covering the 0.2 ms preview
      readback, which is the same write); `serve_bench` `window_capture_inline` 8.78 ms (20),
      `window_readback` 1.18 ms (2), every run at the step's end flagged noisy (outside
      load), so the gate's verdict is open.
11. **GPU-driven geometry.**
    - Instance culling with prefix-sum compaction, two-pass Hi-Z, indirect G-buffer and
      CSM draws, face culling.
    - The `bench10k_1080p`/`bench100k_1080p` scenes join `render_bench` (lights still
      capped until step 13: they use 8).
    - Gates the CPU-submission budgets.
    - Test 6.
12. **Meshlets.**
    - The builder; the meshlet arenas; task/mesh shaders with the fallback; `geometry`
      setting.
    - Gates the meshlet-build budget.
    - Test 7.
13. **Clustered lighting.**
    - `lights.cluster`; the uncapped light table; lighting, particles and GI on clusters.
    - The 1,000-light `bench100k_1080p`.
    - Gates `lights.cluster` and the `bench100k_1080p` frame.
    - Test 8.
14. **Path-traced frames in a window.**
    - The RT present path; RT captures in the window host; `window_capture_rt_inline_ms`
      recorded.
    - Test 11.
15. **The agent surface and acceptance.**
    - `render.settings` (with `quality`); capture `gpuMs`/`timings`; `engine.info.gpu`; the serve guide's
      "Measure the frame" section; `docs/ARCHITECTURE.md` (the frame diagram, a recipe for
      adding a pass to the registry with its budget); the monograph's renderer units.
    - Five consecutive clean runs of every budget; the roadmap row done.
    - Tests 14–16.

The order follows the money. Measurement first (Frank's instruction). Then the defects
that change images, so later steps re-baseline once against correct images. Then the
frame cost, which meets Frank's two budgets by step 10. Then the structural work, which
the 100k-instance scene measures. Then the window path tracer and the acceptance.

## Risks

- **The frame budget may not be reachable by sample counts alone.** Step 1's profile
  tells. If RT GI at 2 rays and half resolution is still over 0.30 ms, quarter-resolution
  GI with the same denoiser is the next lever, and it stays within this spec's design. If
  the frame as a whole cannot reach 1.50 ms, that goes to Frank with the profile: the
  budgets are his, not the step's.
- **Denoised low-sample GI looks different.** Single-frame captures are smoother, and
  their detail softer, than today's 32-ray images. The goldens record it, and the change
  record shows before/after crops.
- **GPU culling can drop visible objects.** A conservative Hi-Z test bug hides geometry.
  Test 6's parity images catch it, exactly.
- **Mesh shaders are a second code path.** Test 7's parity keeps the two honest. The
  fallback is the one non-NVIDIA hardware will use.
- **Determinism under GPU-built lists.** One atomic in the wrong place makes goldens
  flaky. The prefix-sum rule, the shader lint and the twin-render tests are the guard.
- **The machine.** Clean bench runs need it quiet. Unreal Editor and other bots' work
  made every bench run noisy for hours in 4. The acceptance runs need a quiet window (see
  "Open decisions").
- **Slang toolchain.** `slangc` comes from the installed SDK (1.4.321.1). A different SDK
  on another machine may emit different SPIR-V, so the doctor checks the version.

## Review focus

Inputs no step's tests exercise by default, most likely to bite first:
1. A capture requested while a resize is pending: the Hi-Z pyramid, history images and
   clusters must follow the new extent before the capture frame.
2. An instance whose mesh changes between the early and late occlusion passes (a
   `world.apply` between steps in lockstep): no stale draw IDs.
3. A light added or removed in the frame a capture resets: cluster lists and shadow slots
   rebuilt before use.
4. `render.settings` changed in realtime mid-frame: applied at the next frame boundary,
   never mid-recording.
5. A model whose node matrix has negative scale and shares a mesh with an unmirrored
   node: two copies with opposite winding, both lit correctly (covered by test 4's
   fixture since step 3).

## Decisions record

| # | Decision | Choice |
|---|---|---|
| 1 | First step | Profiling: a pass registry, per-pass timestamps, `render_bench`, budgets tagged by step |
| 2 | Frame budget | `cornell_720p` ≤ 1.50 ms GPU, from Frank's 2 ms readback ruling; per-pass budgets sum to it |
| 3 | RT shadows | 1 ray per light, ≤ 4 shadowed lights a pixel by deterministic importance, spatial filter + temporal in realtime; scene lights; `Light.castShadows`; CSM only as fallback |
| 4 | RT GI | Half resolution, 2 rays, reprojected temporal + 2 à-trous + joint-bilateral upsample; history reset on capture |
| 5 | SSAO | Wired, not removed; fixed; half resolution; ambient term only |
| 6 | Culling | GPU frustum + two-pass Hi-Z; prefix-sum compaction (no order-dependent atomics); indirect draws |
| 7 | Meshlets | In-repo greedy builder (64 v / 124 t, sphere + cone), no new dependency; mesh shaders where supported, vertex-pulling fallback; meshes < 256 triangles stay instanced |
| 8 | Lights | Froxels 16x9x24, ≤ 256 lights a cluster, table of 65,536; ordered build |
| 9 | Upscalers, VSM, ReSTIR, probes | Later (reasons in "Scope") |
| 10 | Slang | For new and rewritten shaders; GLSL elsewhere; one shared layout file |
| 11 | RT in a window | Present path for both RT modes; lockstep RT captures; RT capture time recorded, not gated |
| 12 | Readback | Ring deleted; request/readback everywhere |
| 13 | glTF | Node transforms baked per (node, mesh); one glTF loader (Assimp); glTF is Y up in the framer |
| 14 | Goldens | Each intentional change its own commit with statistics; tolerances only tightened |
| 15 | Agent surface | `render.stats` (compact pairs), `render.settings` (one reflected struct, host state), capture `gpuMs`/`timings`, `engine.info.gpu` |
| 16 | Bench noise | Spec 4's rules plus a GPU calibration kernel and `wait_quiet.py` |
| 17 | Reference quality (Frank, 2026-10-24 18:01) | `quality: reference`: 8 shadow rays a light (all lights), 32 GI rays a pixel at full resolution, plain running mean, unbudgeted; `render.settings`, capture param, `--quality`; reference goldens; steps 7, 8 and 15 |

## Frank actions

1. Accept this spec (it is binding once accepted).
2. Still from sub-project 4: create and approve `.mcp.json`, run the manual Claude Code
   session, and install the `verify` skill draft (`docs/agents/skills/README.md`).
3. Rule on any budget a step measures and misses (none expected; spec 4's process).

## Open decisions

1. **A quiet window for bench acceptance.** Five consecutive clean runs need the machine
   free of Unreal Editor and the other bots' heavy jobs (in 4, every bench run for hours
   was noisy, and `scene_bench` never got a clean run with Unreal Editor open). Can the
   step chain have scheduled quiet windows, or may it run the acceptance benches when
   `wait_quiet.py` finds one, however long that takes?
