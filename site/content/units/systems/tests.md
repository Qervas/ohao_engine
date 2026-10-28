---
module: systems
id: tests
title: Tests and goldens
standard: v2
---

## The bug no assertion catches

A renderer's characteristic failure is not a crash. It is a frame three percent
too dark, a highlight that lost its anisotropy, a sampler that stopped
decorrelating. Nothing in the C++ is invalid, so nothing asserts; the only oracle
is the image. OHAO's top-level test is therefore a rendered PNG diffed against a
committed one, and every decision here lives inside *diffed*.

## Determinism had to be bought before it could be tested

You cannot diff renders that are not reproducible, and OHAO's were not. The
raygens' temporal reprojection sampled the accumulation buffer at a *reprojected
neighbour* pixel, and the realtime profile's in-shader spatial denoise read its
neighbours too — texels that sibling invocations of the same dispatch write at the
end of the very same shader. That is a cross-invocation read-after-write with no
defined ordering, so a static scene did not render the same way twice. The cure is
structural: the accumulation is a ping-pong pair, the trace writes this frame's and
reads only the previous frame's, and the spatial denoise is a compute pass after
the trace, when every texel it reads is finished.

{{cite shaders/rt/pt_raygen_realtime.rgen "layout(set = 0, binding = 3, rgba32f) uniform readonly image2D accumHistory;"}}

What remains is not the engine's. The driver's BVH builder lays out a mesh's nodes
differently run to run, and a ray that hits exactly on an edge two triangles share
takes whichever triangle the traversal reaches first: 1 LSB on a few pixels. The
helmet flipped in five renders of eighty-three; the Cornell box never did run to
run, but the same scene loaded from its file builds a different BVH than the
example's code does, and the two renders differ on ten pixels.

{{cite tests/golden/manifest.json "driver's BLAS builder lays out the BVH differently run to run (identical"}}

:::why
Bit-exact comparison holds wherever it is achievable — every deferred scene —
because any tolerance there hides a change. Where a floor exists outside the
engine's control, the tolerance is that measured floor and nothing more, the same
call PBRT and Mitsuba's suites make.
:::

## Two predicates, because one is not enough

A scene passes only if both conditions hold:

$$\max_{p,c} \bigl|A_{pc} - G_{pc}\bigr| \le \tau_{\text{abs}}
\qquad \wedge \qquad
\frac{\bigl|\{\, p : \exists c,\; A_{pc} \neq G_{pc} \,\}\bigr|}{|P|} \le \tau_{\text{frac}}$$

$A$ is the fresh render, $G$ the committed golden, both 8-bit sRGB after the
downscale below; $P$ is the pixel set of that reduced grid and $c$ the channel.
Defaults are $\tau_{\text{abs}} = 4$ LSB and $\tau_{\text{frac}} = 0.01$,
overridable per scene from the manifest.

{{cite tests/golden/render_golden.py "def verdict(s, max_abs_diff, max_diff_frac):"}}

The two fail on opposite regressions. The max catches a localized,
high-magnitude change — one light missing, one material black — which can touch
well under 1% of pixels. The fraction catches the inverse: an exposure or tonemap
constant shifting every pixel by one LSB, which sails under the max. Note the
membership test is $\neq$, not a threshold, so a pixel counts as differing on a
single LSB; the 1% budget absorbs that hair-trigger.

Both statistics are computed on `int16`. On the natural `uint8` the subtraction
wraps, and a golden one LSB *brighter* than the render reads as a difference of
255 instead of 1 — catastrophic failure on a perfect match.

{{cite tests/golden/render_golden.py "dtype=np.int16"}}

## The downscale that does two jobs

Before diffing, both images are bilinearly resized to 640 pixels wide; the
manifest scenes render at 1920×1080, so the goldens on disk are 640×360.

{{cite tests/golden/render_golden.py "DOWNSCALE_WIDTH = 640"}}

That step pays twice: it averages the floating-point ghost down to nothing at
compare resolution, and it shrinks a golden from a ~6 MB noisy 16-spp full-res
PNG to under half a megabyte, small enough to keep in git. `--update` stores the
*already-downscaled* image, so in check mode the resize is a no-op on the golden
side and the two paths agree by construction.

On failure the render is saved beside the golden as `<name>.actual.png` at
**full** resolution — a human eyeballing drift wants the real pixels. That is
also a footgun: ~6 MB next to a sub-500 KB golden, and `.gitignore` covers only
`*.diff.png`.

{{cite tests/golden/render_golden.py "drift = golden.replace("}}

## Which shader the goldens actually gate

The manifest is ten scenes, all with the denoiser explicitly off: the harness pins
raw deterministic beauty, treating denoised output as a separate, unpinned concern.
Five run the example binaries. Two of those are path traced on the offline profile,
two are the deferred hybrid on the same content (`cornell_box_deferred` and
`damaged_helmet_deferred`), and one, `cornell_box_rt_realtime`, runs the realtime
profile.

{{cite tests/golden/manifest.json "./build/cornell_box {out} 16 --denoise=none"}}

The other five render the checked-in scene files in `assets/scenes/` through the
`ohao` CLI, one per example scene, and compare against the *example's* golden
rather than a copy of it — two goldens would let the file and the code drift apart
unnoticed. Five PNGs back ten scenes.

{{cite tests/golden/manifest.json "./build/ohao run assets/scenes/cornell_box.scene.json --render {out} --mode rt_offline --spp 16 --denoise none"}}

The RT profile that command selects is not obvious. With no mode flag the
examples default to `RenderMode::RTOffline`, which instantiates
`RTOfflineRenderer` — and that profile overrides the `PathTracerShaderSet` with
the *offline reference* raygen, not the `pt_raygen.rgen` named in the shader-set
member initializer.

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_offline.rgen.spv"}}

That covers more than one file. `RTRealtimeRenderer` names the same miss,
closest-hit and any-hit SPIR-V modules as the offline profile and differs only in
the raygen entry, so three of the realtime profile's four stages are under the
offline goldens before `cornell_box_rt_realtime` adds the fourth.

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}

So is the include graph the offline raygen compiles in: `pbr_unpack.glsl`,
`mis.glsl`, `env_sampling.glsl`, `sampler_api.glsl`, `nrd_frontend.glsl` and
`ggx_aniso.glsl`, plus what those pull in turn. Those six are the same six the
realtime raygen includes.

{{cite shaders/rt/pt_raygen_offline.rgen "includes/rt/nrd_frontend.glsl"}}

The realtime scene was added after `rt_realtime` lost the device and wrote an
all-black frame — bindings 29–35 were never written into the descriptor set — and
it is looser than the offline pair (`max_abs_diff` 10, `max_diff_frac` 0.05),
because `pt_raygen_realtime.rgen` reads neighbour pixels of images the same
dispatch writes and is not bit-exact run to run. It also puts the PCG branch of
`sampler_api.glsl` under the gate: the sampler is a specialization constant baked at
pipeline creation, the offline profile bakes Sobol, and the realtime profile PCG.
What has no golden is every denoised path.

{{cite tests/golden/manifest.json "bindings 29-35 (reservoirs, spec hit-distance) were never written into the descriptor set"}}

The deferred pair covers the other half. They render the G-buffer, the cascaded
shadow pass, deferred lighting, the post chain and, where the device supports it, the
RT shadows and GI, and they check the render-world migration against the previous
renderer's own output: `cornell_box_deferred` was captured before that migration and
`damaged_helmet_deferred` serves the same purpose on the helmet. Both are exact
(max_abs 0): deferred rendering is bit-identical run to run, so any difference is a
change.

The `16` is spp, and `cornell_box` renders one frame per sample, so the golden pins
16 accumulation frames and reads the last. The deferred scenes pass `deferred` and a
small sample count, and the count is ignored: the same line fixes a deferred run at
ten frames.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : samples;"}}

All ten scenes run on a fresh clone, which was not true for two months and is
worth the paragraph.

The second scene used to invoke `model_viewer` on
`assets/showcase_objects/MetalRoughSpheres.glb`. That file was in no clone and on
no machine — not untracked, simply absent. What made it survive undetected is that
`model_viewer` printed the Assimp error, carried on, rendered an **empty scene**,
wrote a valid PNG and **exited 0**. The harness checks the exit code and whether
the file exists; both passed. So the corpus spent two months comparing a picture of
nothing against a golden full of geometry and reporting it as a large pixel
difference — a missing asset wearing the costume of a rendering regression.

Three changes close it. `model_viewer` and `turntable` now return non-zero when a
model fails to load, rather than rendering nothing successfully. The harness
captures the child's stderr instead of discarding it, so the cause reaches the
reader. And it rejects a frame with no spatial structure, on the principle that a
uniformly coloured image is not a render of a scene.

{{cite examples/model_viewer.cpp "model_viewer renders nothing without a model; refusing to "}}
{{cite tests/golden/render_golden.py "def degenerate(path):"}}

The scene itself is now `damaged_helmet`, on `assets/test_models/DamagedHelmet.glb`
— tracked, 3.7 MB, and the same kind of subject: a metal/rough glTF under an HDRI.

{{cite tests/golden/manifest.json "./build/model_viewer assets/test_models/DamagedHelmet.glb {out} 16 --denoise=none"}}

## Where the gate runs, and why not in CI

Enforcement is a committed pre-push hook, opt-in via
`git config core.hooksPath .githooks`. It soft-skips when the engine has not been
built, so a docs-only checkout is never blocked.

{{cite .githooks/pre-push "if [ ! -x build/cornell_box ]; then"}}

:::why
The gate sits on the developer's machine because the harness needs a GPU with
Vulkan ray tracing and hosted runners have none. The rejected alternative — a
self-hosted GPU runner — buys enforcement nobody can skip, at the price of owning
hardware. The only committed workflow builds the documentation site; the `ci.yml`
(build + ctest + lint) described in the Phase 0 design does not exist, so this
opt-in hook is the entire automated net.
:::

## What the unit tests pin

Three GoogleTest binaries live under `tests/renderer`. Each compiles the one or
two `.cpp` files it needs into its own target instead of linking `ohao_renderer`,
so they run with no Vulkan loader, no device and no GPU — which is what would
make them CI-able, where the goldens are not, if a CI workflow existed.

{{cite tests/renderer/CMakeLists.txt "add_executable(sobol_test sobol_test.cpp"}}

`sobol_test` pins the first eight points of dimensions 0–3, but only dims 0–1 are
externally anchored — against Joe-Kuo `new-joe-kuo-6.21201`. The dim-2 expectations
are, by the test's own comment, bit-expanded from the same `kDirectionNumbers`
table the test is checking, so dims 2–3 pin the XOR-accumulate loop and say nothing
about the table: a wrong direction number in those rows is invisible to this test.

{{cite tests/renderer/sobol_test.cpp "computed from the dim-2 direction numbers"}}

It then probes the one place a direct-bit-expansion generator breaks: at index
$2^{31}$ the dimension-1 direction number is `0xFFFFFFFF`, which a naive
`uint`→`float` cast rounds to exactly 1.0 and pushes out of $[0,1)$.

{{cite tests/renderer/sobol_test.cpp "float v = SobolGenerator::sample1D(2147483648u, 1);"}}

`SobolGenerator` and `owenScramble` have zero callers anywhere in the engine. The
sampler that ships is the GLSL port in `shaders/includes/rt/sampler_sobol.glsl`;
the C++ is a hand-maintained mirror kept as a testable oracle for it.

{{cite ohao/render/rt/owen_scramble.hpp "keeping the two implementations in lock-step"}}

Nothing automated enforces that mirroring — they do currently agree, hash
constants and 24-bit shift included. A divergence on the GLSL side would not fail
`sobol_test` at all; it would surface as a golden failure, because the offline
profile's settings select Sobol.

{{cite ohao/render/rt/rt_settings.hpp ".samplerType = SamplerType::Sobol,"}}

That field is what actually reaches the shader: the pipeline builder copies
`m_renderSettings.samplerType` into a `VkSpecializationInfo` for constant id 0,
which is the `SAMPLER_TYPE` the dispatch functions in `sampler_api.glsl` branch on.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "uint32_t samplerTypeVal = static_cast<uint32_t>(m_renderSettings.samplerType);"}}

`env_cdf_test` asserts the invariants the GPU inverse-CDF lookup depends on:
marginal and conditional rows monotone non-decreasing and terminating at 1.0, and
a hot texel pulling over half the mass into its row and column.
`denoise_parse_test` encodes a *removal* — with the OptiX backend deleted,
`--denoise=optix` must degrade to OIDN rather than to `None`, so old command
lines keep rendering denoised instead of silently returning noise.

{{cite tests/renderer/denoise_parse_test.cpp "TEST(DenoiseTypes, OptixRemovedFallsBackToOidn)"}}

A fourth binary, `renderer_test`, initializes Vulkan, uploads a red quad, renders
one RTOffline frame and writes a PNG under the build directory. It checks that a frame
came back and was written, but never inspects a pixel: an init-and-teardown canary,
not a claim about the image.

{{cite tests/renderer/renderer_test.cpp "check(pixels.size() == std::size_t{W} * H * 4, "a frame renders to pixels");"}}

Five more binaries in the same directory do inspect pixels, by comparing renders
with each other rather than with a committed image, and each ends with a one-line
JSON summary. The first two build a fresh renderer per render. `instantiate_gpu_test` renders the
deferred Cornell box once from hand-spawned meshes and once from `MeshSource`
entities through the instantiate system, and requires the two to be bit-identical.
`scene_view_gpu_test` renders the Cornell box and `model_viewer`'s helmet room three
ways — camera and environment set by hand, applied from the world's `Camera` and
`Environment` entities, and after a round trip through scene text — and requires
them to match: exactly for deferred, within the BVH floor (a few bytes off by 1) for
the path-traced helmet. `restore_render_gpu_test` drives the CLI's own session and render host on
the checked-in Cornell scene with one renderer per sequence: a capture, a move that
must change the image, a `world.restore` after which the capture must equal the
first, bit-identical with post effects off and on. `deferred_determinism_gpu_test`
renders each configuration (Cornell with no post effects, each of TAA, SSAO and bloom
alone, all three; the helmet room without and with them; Cornell path traced, offline
and realtime) in two fresh renderers, in a fresh renderer in a child process of
itself, and twice in one renderer, and requires the same bytes every time — for the
path-traced ones, the same up to the BVH floor (at most 16 bytes, each off by 1).
Validation is on for every run, path-traced ones included, and must report nothing.
`cull_parity_gpu_test` (the 2b spec's test 6) renders the same frames with GPU culling
on and off — a built scene with a wall hiding a row of boxes over 1, 2 and 4 frames,
Cornell at 10 frames, the helmet room at 30 frames with post — and requires identical
images; it also requires two fresh renderers to build byte-identical draw lists, the
hidden and off-screen boxes to be in no list, and one-sided materials to render closed
boxes exactly as double-sided ones.

{{cite tests/renderer/cull_parity_gpu_test.cpp "check(a.lists.commands == b.lists.commands, "t06: twin runs: the same indirect commands, byte for byte");"}}

{{cite tests/renderer/deferred_determinism_gpu_test.cpp "checks.push_back({p + "a fresh renderer in another process is bit-identical", hc == ha});"}}

{{cite tests/renderer/scene_view_gpu_test.cpp "{"cornell: the Camera entity renders exactly as the hand-set camera", c12.bytes == 0},"}}

`engine_tests` runs twelve sections. Two are the ones its header comment numbers first —
`EventBus` and `CommandHistory` — and a third, `Assets`, checks the loader's extension
list; scene entities are covered by `ecs_tests`. The fourth, `runMetaTests`, is the only
place the renderer's compile-time contracts are exercised at all:
fourteen cases covering RT profile traits (offline four bounces, realtime two),
`DenoiseModeTraits` against their runtime mirrors, `RTFeatureFlags` and
`makeFeatureSettings`, `applyDenoisePolicy` and the jitter / fresh-sample /
AOV-accumulation predicates, `as_span` and `span_covers_image`, `Result` and
`VoidResult`, render-graph handles and the camera, physics
handles with `ShapeInfo`, and a rigid body that exists and carries a name with no
scene component behind it.

{{cite tests/engine/engine_tests.cpp "void runMetaTests() {"}}

Four more stand up real Vulkan with no swapchain: the deferred pipeline, the
path tracer on an RT-capable device, a headless frame recorded, submitted and read
back, and the renderer's resize and refusal paths — two of those resize cases with
the validation layer on, failing on what the layer reports. Each reports itself
skipped, not passed, when the machine has no device or no layer.

{{cite tests/engine/engine_tests.cpp "static void runHeadlessDeferredRenderTests() {"}}

Three hold the renderer to sub-project 2b's claims, one test each: every executed
pass registered, timed once and in frame order; frame N's readback equal to a fresh
capture of step N, with only requested frames copying; and RT shadows cast from the
scene's own lights, with `castShadows` false casting none and the light table's
shadow slots handed out in light order. The RT shadow section also checks that the
sky reaches the final image in a frame that runs SSS.

{{cite tests/engine/engine_tests.cpp "static void runRTShadowTests() {"}}

The last section drives the persistent render world on a real device:

{{cite tests/engine/engine_tests.cpp "static void runRenderSceneTests() {"}}

thirteen cases covering mesh registration with its AABB, table entry and BLAS; static
instances uploading once while moving ones upload in proportion; missing material
references falling back to the default; instance-table and mesh-arena growth without
destroying a buffer a frame still reads; the raster shadow-caster rule; a material edit
reaching the packed table row; the path tracer tracing the render world; texture slots
past 1024 reaching the tracer; a moved instance refitting the TLAS; a slot released and
reused in one frame forcing a rebuild; the raster light table being one buffer per frame
slot; and RT accumulation restarting when an instance moves or a light changes.

Its `EXPECT` macro `return`s from the enclosing section function, so the first
failure silently skips the rest of that section; the exit code is still non-zero,
but the pass count understates how much was never attempted.

{{cite tests/engine/engine_tests.cpp "do { if (!(expr)) { TEST_FAIL(msg); return; } } while(0)"}}

The "GPU layout contracts" case is five assertions and they are not all the same
kind. Three are `static_assert`s that can only fail the build, and only one of
those — `sizeof(GPULight) == layout::kGPULightBytes` — restates an
`OHAO_ASSERT_GPU_LAYOUT` in the owning header; the two `MaterialGpuPack` lines
restate plain `static_assert`s in `layout_meta.hpp`. The remaining two go through
`EXPECT`, so a `MaterialGpuPack::vec4Count` or `GpuPod<GPULight>` regression is a
failing *run*, not a failing build.

{{cite tests/engine/engine_tests.cpp "EXPECT(layout::MaterialGpuPack::vec4Count(2) == 8"}}

Three more binaries build by default and never touch an image: `physics_backend_tests`
(59 cases over Jolt body lifecycle, CCD, the 16-layer collision system, queries,
contact callbacks, constraints and the character controller), `force_generator_tests`
(54 cases over force volumes, wind, buoyancy, Hooke's-law springs, the force registry
and AABB math) and `audio_system_tests` (13 cases over the miniaudio wrapper's volume
and handle state). They share `engine_tests`' hand-rolled `TEST_BEGIN`/`TEST_PASS`
harness rather than GoogleTest, and each is a plain `main` returning non-zero on
failure.

Two more binaries cover the entity layer and the windowed host. `ecs_tests` (26
cases) checks transform propagation and hierarchy rules — parenting, cyclic
`setParent` rejection, subtree destruction, system order — and, on a headless
renderer that it skips when no device exists, the render extract: slot creation and
release, lights following transforms in creation order, a model's materials becoming
submeshes, an emissive texture yielding a proxy light, and unknown mesh handles being
skipped. `runtime_tests` (29 cases) needs no GPU: it pins the fixed-step clock, the
timeline's lockstep, realtime and paused modes, the input action map, the capture's
image helpers (a lossless PNG round trip, crop, scale and the JPEG preview) and the
swapchain-selection helpers. The live host's loop itself is exercised by running
`ohao_game --frames N`, not by this suite.

{{cite CMakeLists.txt "option(BUILD_PHYSICS_TESTS"}}

The scene-file layer has its own CPU-only suites, each a plain `main` that ends with
a one-line JSON summary. `reflect_tests` covers the type registry, schema export,
the JSON writer and reader, float formatting, and the rule that the ECS system
sources may not include `reflect/` or `api/`. `scene_tests` covers the engine's
component registrations, entity ids, the scene codec with its error corpus and
migration, snapshots, and instantiate against a fake backend. `api_tests` covers the
agent API's methods, commands and their inverses, queries and summaries;
`systems_tests` the engine-provided spin system.

{{cite CMakeLists.txt "option(BUILD_API_TESTS"}}

Two Python harnesses sit beside them. `tests/cli/check_cli.py` runs each case in
`tests/cli/cases.json` twice from a fresh scratch directory, requiring the same
bytes both times and, where a case says so, stdout and written files equal to
checked-in expectations; `--cpu-only` skips the cases that create a device. Its last
two cases are what keep the checked-in scenes honest: each example's
`--save-scene` output must equal its file under `assets/scenes/` byte for byte, so
editing a builder without regenerating the file fails here.

{{cite tests/cli/cases.json ""files": {"build/cli_tmp/cornell_box.scene.json": "assets/scenes/cornell_box.scene.json"}},"}}

`tests/reflect/check_scene_schema.py` runs the generated JSON Schemas through a stock
validator and needs the `jsonschema` package.

## One command runs all of it

Every suite, harness and gate is a CTest test with exactly one label, registered
through one helper that runs it from the source root and gives every GPU test the
resource lock `gpu`, so two never share the device:

{{cite cmake/ohao_tests.cmake "set_property(TEST ${name} PROPERTY RESOURCE_LOCK ${locks})"}}

The labels are `quick` (CPU suites and harnesses), `gpu`, `docs`, `golden`, `bench`
(run alone: `RUN_SERIAL`), `slow` (the exhaustive float check) and `selftest`.
`python tools/check.py` configures if needed, builds the `shaders` target before
everything else, runs `ctest` on the chosen labels (`quick,gpu,docs` by default,
`--all` for every one but `selftest`) and writes one verdict, `build/check/check.json`,
also printed as its last line. Each test's entry carries the counts from the suite's own
JSON line; a failing one carries the last 40 lines of its output.

{{cite tools/check.py "entry["tail"] = tail(output)"}}

A benchmark that shared the machine with other busy processes, or ran slower than its
calibration loop, says so in its summary line (`noisy`, exit 2): its numbers are no
verdict on the code. The check reruns such a test alone, at most twice, and lists every
attempt; one still noisy after that has not passed and stays marked `noisy`:

{{cite tools/check.py "MAX_NOISY_RERUNS = 2"}}

Every C++ suite ends with the same summary line, from `tests/common/harness.hpp`; the
three GoogleTest suites get it from a main that replaces `gtest_main`. Tests that share
a scratch directory under `build/` hold a common lock as well: the CLI check writes
`build/cli_tmp` and runs as two tests and inside the doc checker.

`tests/check/check_tests.py` tests the check itself: against fixtures configured into
their own build tree, a failing test must fail the check and arrive with its summary and
tail, a noisy bench must be rerun at most twice, and every test executable the build
produces must be registered.

Before any test runs, the check asks `ohao doctor` whether the machine can run them at
all: the Vulkan device, stale or missing SPIR-V (following each shader's includes the way
glslc resolves them, and each Slang file's imports), missing assets, conda's cmake,
`slangc` at the version pinned in `shaders/slangc.version`, the harnesses' Python modules, and a
64x64 deferred frame that must render within 2 s, so a GPU wedged after a crash is named
as such rather than surfacing as slow renders everywhere:

{{cite tools/ohao/doctor.cpp "if (frameMs <= 2000.0 && image)"}}

A failing golden no longer says only that it failed. `render_golden.py --json` lists, per
scene, the statistics, the tolerance and up to four regions where the image changed, and
writes a heatmap and an expected | actual | heat crop of the worst region into
`build/golden_diff/`, which an agent can open.

{{cite tests/golden/render_golden.py "def regions(d, limit=MAX_REGIONS):"}}

## What only looks wired up

Two things under `tests/` look like tests and are not.

- `tests/renderer/renderer_pipeline_tests.cpp`, 939 lines advertising "Phase 1–5"
  coverage of the deferred pipeline, is in no `CMakeLists.txt` and could not
  compile if it were: five of its eighteen project includes name headers no longer
  in the tree — `volumetric_pass.hpp`, `motion_blur_pass.hpp`, `dof_pass.hpp`,
  `ssgi_pass.hpp`, `indirect_draw_buffer.hpp`. The other thirteen resolve.

{{cite tests/renderer/renderer_pipeline_tests.cpp "render/deferred/ssgi_pass.hpp"}}

- `tests/shadow_system/` is the same defect one step further along: the root
  `CMakeLists.txt` declares its option `OFF` with the reason written inline — it
  references `unified_light.hpp`, `shader_bindings.hpp` and
  `ohao_vk_descriptor_builder.hpp`, none of which exist under `ohao/`. It at least
  fails honestly, by never being configured.

{{cite CMakeLists.txt "# Shadow system tests — OFF until renderer/lighting/ headers are implemented"}}

Beside that, `tests/reference_scenes/` is a human-reviewed library — one scene so
far — logging each feature checked against a committed reference render. Nothing
scripts it; it is a review artifact, not a gate.

:::key
The whole automated image-quality net is five 640×360 PNGs backing ten scenes,
compared under two tolerances, by a hook a developer must opt into, on a machine with
a GPU. Every scene loads only tracked assets, so on a fresh clone all ten run — but
only where the hook is installed and the engine built. Judge this suite by what executes, not by what is
checked in.
:::

## Contracts

- Re-enabling temporal reprojection in `pt_raygen_offline.rgen` does not merely change the goldens, it makes them unreproducible: the race has no defined ordering, so `--selftest` starts failing against itself.
- The harness must run from the repository root — manifest commands and golden paths are repo-relative. The executable is resolved through the per-configuration subdirectories and the `.exe` suffix, so `./build/cornell_box` finds `build/Release/cornell_box.exe`; one that is genuinely absent is reported as a scene FAIL (`could not launch …`), not a traceback. The pre-push hook sidesteps the working-directory requirement by `cd`-ing to the toplevel first.
- Goldens must come from `--update`, which stores the downscaled image. One regenerated at full resolution is a shape mismatch, reported cleanly as `size mismatch vs golden` in check mode. `--selftest` never opens the golden at all — it renders twice and diffs actual against actual — but it dereferences its stats dict unguarded, so if the two renders of one command ever disagree in shape it raises instead of reporting.
- Adding a manifest scene is the only way a fixed bug becomes non-regressible; nothing else in the tree pins an image against a committed reference — the `*_gpu_test`s only compare fresh renders with each other.
