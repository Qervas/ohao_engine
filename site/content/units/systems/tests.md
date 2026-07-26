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
offline reference raygen's temporal reprojection sampled the accumulation buffer
at a *reprojected neighbour* pixel — a texel that sibling invocations of the same
dispatch write at the end of the very same shader. That is a cross-invocation
read-after-write with no defined ordering, so a static scene did not render the
same way twice.

{{cite shaders/rt/pt_raygen_offline.rgen "// Phase 0 determinism contract: temporal reprojection reads accumBuffer at a"}}

The fix is one assignment forcing the own-pixel accumulation branch; the offline
camera never moves, so reprojection bought nothing. The block below it was left
in place, merely unreachable. With it off, repeat renders of one frame differ by
at most ~6 pixels at 1 LSB on a 1920×1080 frame: the irreducible floor from
non-associative reduction order in the multi-light NEE sum.

{{cite docs/superpowers/specs/2026-06-24-renovation-phase0-foundation-design.md "deterministic up to ~6 pixels at 1 LSB on a 1920"}}

:::why
Bit-exact hashing was rejected because it would flag that six-pixel ghost forever.
True bit-exactness means forcing reduction order and disabling fast-math — real
effort and real performance cost to close a six-pixel gap. A tolerance compare
absorbs the floor while still catching anything real, the same call PBRT and
Mitsuba's suites make.
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

The manifest is two scenes invoked through the example binaries with the denoiser
explicitly off: the harness pins raw deterministic beauty, treating denoised
output as a separate, unpinned concern.

{{cite tests/golden/manifest.json "./build/cornell_box {out} 16 --denoise=none"}}

The RT profile that command selects is not obvious. With no mode flag the
examples default to `RenderMode::RTOffline`, which instantiates
`RTOfflineRenderer` — and that profile overrides the `PathTracerShaderSet` with
the *offline reference* raygen, not the `pt_raygen.rgen` named in the shader-set
member initializer.

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_offline.rgen.spv"}}

That covers more than one file. `RTRealtimeRenderer` names the same miss,
closest-hit and any-hit SPIR-V modules as the offline profile and differs only in
the raygen entry, so three of the realtime profile's four stages are already under
the golden.

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}

So is the include graph the offline raygen compiles in: `pbr_unpack.glsl`,
`mis.glsl`, `env_sampling.glsl`, `sampler_api.glsl`, `nrd_frontend.glsl` and
`ggx_aniso.glsl`, plus what those pull in turn. Those six are the same six the
realtime raygen includes.

{{cite shaders/rt/pt_raygen_offline.rgen "includes/rt/nrd_frontend.glsl"}}

What has no golden is `pt_raygen_realtime.rgen` itself, the deferred rasteriser,
every denoised path, and the PCG branch of `sampler_api.glsl` — the sampler is a
specialization constant baked at pipeline creation, and the offline profile bakes
Sobol, so the branch the realtime settings ask for is never executed under the
gate.

The `16` is spp, not frames — `cornell_box` renders `samples + 3`, so the golden
pins 19 accumulation frames.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : (samples + 3);"}}

Only one of the two scenes runs on a fresh clone. The second invokes
`model_viewer` on `assets/showcase_objects/MetalRoughSpheres.glb`, and nothing
under `assets/showcase_objects/` is tracked by git — it is not ignored either, so
it survives as an untracked local file on the machine that authored the golden and
exists nowhere else.

{{cite tests/golden/manifest.json "assets/showcase_objects/MetalRoughSpheres.glb"}}

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
one RTOffline frame and writes a PNG — but never inspects a pixel, returning 0
even when `getPixels()` hands back null. An init-and-teardown canary, fine to
have provided nobody reads its `ALL PASS` as a claim about the image.

{{cite tests/renderer/renderer_test.cpp "=== ALL PASS ==="}}

`engine_tests` runs four sections. Three are the ones its header comment names —
`EventBus`, `CommandHistory`, `Scene`. The fourth, `runMetaTests`, is the largest
and the only place the renderer's compile-time contracts are exercised at all:
fifteen cases covering RT profile traits (offline four bounces, realtime two),
`DenoiseModeTraits` against their runtime mirrors, `RTFeatureFlags` and
`makeFeatureSettings`, `applyDenoisePolicy` and the jitter / fresh-sample /
AOV-accumulation predicates, `as_span` and `span_covers_image`, `Result` and
`VoidResult`, render-graph handles with culling AABBs and the camera, and physics
handles with `ShapeInfo`.

{{cite tests/engine/engine_tests.cpp "void runMetaTests() {"}}

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

{{cite tests/engine/engine_tests.cpp "EXPECT(layout::MaterialGpuPack::vec4Count(2) == 6"}}

Three more binaries build by default and never touch an image: `physics_backend_tests`
(59 cases over Jolt body lifecycle, CCD, the 16-layer collision system, queries,
contact callbacks, constraints and the character controller), `force_generator_tests`
(54 cases over force volumes, wind, buoyancy, Hooke's-law springs, the force registry
and AABB math) and `audio_system_tests` (13 cases over the miniaudio wrapper's volume
and handle state). They share `engine_tests`' hand-rolled `TEST_BEGIN`/`TEST_PASS`
harness rather than GoogleTest, and each is a plain `main` returning non-zero on
failure.

{{cite CMakeLists.txt "option(BUILD_PHYSICS_TESTS"}}

## What only looks wired up

Four things under `tests/` look like tests and are not.

- `enable_testing()` is called nowhere in the repository, so CMake emits no
  `CTestTestfile.cmake` and `ctest -N` reports zero tests. Every `add_test` and
  `gtest_discover_tests` call under `tests/` is inert; each suite runs by
  executing its binary.
- `tests/CMakeLists.txt` is orphaned — the root adds `tests/renderer`,
  `tests/engine` and the rest individually, never `tests` itself. Fortunate: that
  file declares a second `engine_tests` target and re-adds `tests/renderer`, so
  wiring it in would fail configuration outright.

{{cite tests/CMakeLists.txt "gtest_discover_tests(engine_tests)"}}

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
The whole automated image-quality net is two 640×360 PNGs, compared under two
tolerances, by a hook a developer must opt into, on a machine with a GPU — and one
of the two scenes has no committed input asset, so on a fresh clone it is one PNG.
Judge this suite by what executes, not by what is checked in.
:::

## Contracts

- Re-enabling temporal reprojection in `pt_raygen_offline.rgen` does not merely change the goldens, it makes them unreproducible: the race has no defined ordering, so `--selftest` starts failing against itself.
- The harness must run from the repository root — manifest commands and golden paths are repo-relative, and `subprocess.run` is given `./build/cornell_box` verbatim. From anywhere else it raises an uncaught `FileNotFoundError` and the harness dies with a traceback; the "render produced no output" branch never fires, because it presumes the command ran and exited 0. The pre-push hook sidesteps this by `cd`-ing to the toplevel first.
- Goldens must come from `--update`, which stores the downscaled image. One regenerated at full resolution is a shape mismatch, reported cleanly as `size mismatch vs golden` in check mode. `--selftest` never opens the golden at all — it renders twice and diffs actual against actual — but it dereferences its stats dict unguarded, so if the two renders of one command ever disagree in shape it raises instead of reporting.
- Adding a manifest scene is the only way a fixed bug becomes non-regressible; nothing else in the tree gates an image.
