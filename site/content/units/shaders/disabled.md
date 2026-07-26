---
module: shaders
id: disabled
title: Disabled shader archive
standard: v2
---

## The directory that nothing excludes

`shaders/_disabled/` and its C++ twin were created by the same commit — 851092d
(2026-03-20), the refactor that rewrote the renderer around ray tracing and cut
`src/renderer/passes/` from 34 pass `.cpp` files to 11. Thirty-five shader files
were `git mv`'d out of `compute/`, `water/`, `terrain/`, `foliage/` and `decal/`
into `shaders/_disabled/`; 48 pass sources and headers were `git mv`'d into
`src/renderer/passes/_disabled/` in the same breath. Neither archive lost a file
that day.

The two halves then diverged. The C++ archive travelled intact through three
renames and was deleted twenty days later, in e8eb9d1 (2026-04-09). The shader
archive is still here. What survives in the working tree is a shaders-only fossil: no
`WaterPass`, `TerrainPass`, `FoliagePass`, `DecalPass` or cloud pass class exists
anywhere under `ohao/`, and the nearest thing to a survivor is a comment telling
you to initialise one of them first.

{{cite ohao/render/deferred/sky_pass.hpp "Must be called after CloudPass is initialized"}}

The C++ build still carries the line that switched the archive off: a
`list(FILTER ...)` applied to a recursive source glob.

{{cite ohao/render/CMakeLists.txt "list(FILTER RENDERER_SOURCES EXCLUDE REGEX"}}

That line matches nothing today — `find ohao -type d -name _disabled` comes back
empty. It is the last vestige of the C++ archive, not a maintained convention.
The shader build never had an equivalent. It globs recursively from the `shaders/`
root and stops there:

{{cite shaders/CMakeLists.txt "file(GLOB_RECURSE SHADER_SOURCES"}}

So the 35 archived shaders enter the build graph alongside the 56 live ones. The
glob runs at *configure* time, and carries no `CONFIGURE_DEPENDS`; the per-shader
`add_custom_command` runs at *build* time and only when its `.spv` is older than
its inputs. Configuring compiles nothing — a clean
`cmake --build build --target shaders` compiles all 91, an incremental one
compiles whatever went stale.

In this tree the archive is 296 KB of the 1.16 MB of SPIR-V the shader target
currently produces. (`build/shaders/` holds 96 `.spv` for those 91 sources; the
five extras — 490 KB, led by three path-tracer raygen modules — have no source
file under `shaders/` any more and are never rebuilt.) Nothing loads the archive:
every `loadShaderModule` / `readFile` call site in `ohao/` names a live module,
and no `_disabled_*.spv` string appears anywhere in the C++ tree. The reason it
cannot be loaded *by accident* is the flat output naming — the relative path is
underscore-joined into the SPIR-V filename, so everything from the archive is
namespaced behind a `_disabled_` prefix that no loader ever spells:

{{cite shaders/CMakeLists.txt "string(REPLACE"}}

That is also why the archived `ssr.comp` and the live `postprocess/ssr.comp` — the
only basename collision between the two halves of the tree — do not overwrite each
other's output.

## The accident is doing real work

Compiling dead code sounds like pure waste, and the obvious cleanup is a
`list(FILTER ...)` line in `shaders/CMakeLists.txt`. Before doing that, notice
what the current setup buys. Because the archive is in the build graph, every
archived shader is *compile-gated*: four months after the strip, all 35 still
compile clean under `glslc --target-env=vulkan1.3 -Ishaders -Ishaders/includes`
against the current include tree — not because anyone maintained them, but
because nobody was allowed to break them.

The gate is not uniform, though, and it matters which grade a given module gets.
Nine of the 35 `#include` anything at all. Seven of those nine pull only headers
the dependency list tracks, so breaking one turns the build red on the next
*incremental* compile; `common/encoding.glsl` is the sharpest, shared by four
archived modules and five live ones.

{{cite shaders/_disabled/ssr.comp "includes/common/encoding.glsl"}}
{{cite shaders/core/deferred_lighting.frag "includes/common/encoding.glsl"}}

The other 26 archived shaders `#include` nothing, so for them the gate fires only
on a clean build or a toolchain change — real, but slower. And the two remaining,
`terrain.frag` and `terrain_gen.comp`, get neither: their only `#include` is
`includes/noise.glsl`, which the dependency glob misses entirely (see below), so
editing `noise.glsl` leaves both `.spv` files valid and the build green.

:::why
Excluding the archive from the glob would drop 35 of the 91 `glslc` invocations a
clean shader build performs. It would also remove the only thing keeping the
archive syntactically alive. The engine currently pays a small, bounded cost for a
compile gate it did not know it was buying. Removing that gate should be a
decision, not a tidy-up.
:::

## What a green build does not prove

`glslc` compiles each module in isolation. It checks GLSL syntax, types and
`#include` resolution. It does not check that a descriptor set layout exists, that
a push-constant block fits `maxPushConstantsSize`, that two shaders in the same
chain agree on a constant, or that a value a shader *declares* as a push constant
actually reaches the code that consumes it. The archive's most substantial asset —
a four-stage Tessendorf FFT ocean under `water/waves/` — carries one defect of each
of the last two kinds.

The chain is textbook: `spectrum_init.comp` draws a Phillips-weighted Gaussian
field $h_0(\mathbf{k})$ once per wind change, `spectrum_hkt.comp` animates it in
the frequency domain each frame, `fft_butterfly.comp` runs a 2-D inverse FFT as
two 1-D passes, and `fft_normal.comp` differentiates the result into a normal and
a foam mask. The per-frame step is the Hermitian-symmetric height field

$$h(\mathbf{k},t) = h_0(\mathbf{k})\,e^{\,i\omega t} + h_0^{*}(-\mathbf{k})\,e^{-i\omega t},
\qquad \omega(\mathbf{k}) = \sqrt{g\,\lVert\mathbf{k}\rVert}$$

where $\mathbf{k}$ is the 2-D wave vector on the $N\times N$ frequency grid,
$h_0^{*}(-\mathbf{k})$ is the conjugate partner packed into `.ba` by
`spectrum_init` (which is what makes the inverse transform real-valued), $\omega$
is the deep-water linear dispersion relation, and $g$ is gravity.

That last symbol is the first defect. `spectrum_init` treats gravity as a
push-constant, because the Phillips length scale $L = \lVert w \rVert^2 / g$ needs
it:

{{cite shaders/_disabled/water/waves/spectrum_init.comp "float L  = windLen * windLen / pc.gravity;"}}

`spectrum_hkt`, one dispatch later in the same chain, hard-codes it as a
file-scope constant:

{{cite shaders/_disabled/water/waves/spectrum_hkt.comp "float omega = sqrt(g * kLen);"}}

Any host that passed a non-9.81 gravity would get a spectrum and a dispersion
relation describing different planets. The second defect is in the butterfly pass:
`N` is a push-constant, but the bit-reversal permutation and the stage count are
both frozen at 256, matching the fixed `local_size_x = 256` and the 4 KB shared
array:

{{cite shaders/_disabled/water/waves/fft_butterfly.comp "// Bit-reversal for log2(256) = 8 bits"}}

`pc.N` reaches only the final $1/N$ normalisation. A resolution other than 256
would produce a silently wrong transform, not a validation error.

Neither defect was ever exercised, because the only host this chain ever had
pinned both values. `FFTOceanSim` declared `static constexpr uint32_t N = 256;`
with the comment "must be power-of-2, ≤ local_size_x", and pushed a literal
`9.81f` into its `SpectrumInitPC`, from the day it landed to the day it was
deleted. So these are not latent rot nobody noticed — they were host-side
contract, of the kind `glslc` cannot see and the working tree no longer states
anywhere. The shader survived the strip; the invariant did not.

## The archive holds four live headers hostage

`shaders/includes/` is the live tree, but ten of its 32 `.glsl` headers have no
live consumer. Four of the ten are the archive's hostages — their only `#include`
sites are inside `_disabled/`: `noise.glsl` (terrain), `lighting/phase.glsl` (cloud
and volumetric scattering), `cloud/cloud_density.glsl`, and `water/gerstner.glsl`.

{{cite shaders/_disabled/terrain/terrain_gen.comp "../includes/noise.glsl"}}
{{cite shaders/_disabled/water/water.vert "water/gerstner.glsl"}}

The other six are dead outright, with zero `#include` sites anywhere in `shaders/`,
live or archived: `lighting/ibl.glsl`, `lighting/light_types.glsl`,
`material/advanced_brdf.glsl`, `material/material_sampling.glsl`,
`shadow/shadow_csm.glsl` and `shadow/shadow_types.glsl`. Delete the archive and its
four hostages join that group in the same instant — which means a future "remove
unused shader includes" sweep and a future "remove the archive" commit have to be
ordered, or the second one invalidates the first one's premise.

There is a sharper edge underneath. The include dependency list is a separate
glob, and every shader's compile command depends on *all* of it, so touching any
header the list contains invalidates all 91 modules — the 35 archived ones
included:

{{cite shaders/CMakeLists.txt "DEPENDS ${SHADER} ${SHADER_INCLUDES}"}}

The list is not all of `includes/`, though. It is written with a `**` segment that
CMake does not treat as "zero or more directories":

{{cite shaders/CMakeLists.txt "/includes/**/*.glsl"}}

It matches 30 of the 32 `.glsl` files under `includes/`. The two it misses are the
ones sitting directly in `includes/` rather than a subdirectory — `noise.glsl`,
whose only consumers are archived, and `pbr_unpack.glsl`, which is very much live
and `#include`d by all three path-tracer raygen shaders. Editing either triggers no
rebuild at all.

## Sometimes the archived version is the better one

The word "disabled" invites the reading that this is worse code. For screen-space
reflections it is the opposite. The live `SSRPass` loads the 103-line
`postprocess/ssr.comp`:

{{cite ohao/render/deferred/ssr_pass.cpp "VkShaderModule shader = loadShaderModule("}}

which marches a fixed number of uniform steps along the reflection ray:

{{cite shaders/postprocess/ssr.comp "int maxSteps = 64;"}}

The archived `_disabled/ssr.comp` is 205 lines and does hierarchical Hi-Z
traversal — descend a mip on a potential hit, ascend one on a miss:

{{cite shaders/_disabled/ssr.comp "float sampleHiZ(vec2 uv, int mipLevel) {"}}

The corresponding pyramid generator, `shaders/compute/hiz_generate.comp`, is still
in the *live* tree and has zero C++ consumers — no `hiz` symbol appears anywhere
under `ohao/`. So the engine builds a Hi-Z generator nothing runs, for a traversal
only the archive implements, while shipping the linear fallback.

A second residue points the same way: device creation still requests
`tessellationShader`, though the only tessellation stages left in the tree are
`_disabled/terrain/terrain.tesc` and `.tese`.

{{cite ohao/gpu/vulkan/device_setup.cpp "deviceFeatures2.features.tessellationShader = VK_TRUE;"}}

:::key
`_disabled` marks *unbound*, not *unbuilt*. These 35 modules sit in the shader
build graph and are bound by no pipeline in `ohao/` — so what a green build
certifies about them is syntax and `#include` resolution, and nothing else. Where
a host once existed it exists only in git history; the current tree holds no
descriptor layout, pipeline or push-constant struct for any of them.
:::

## Contracts

- Adding an exclusion filter to `shaders/CMakeLists.txt` silently removes the compile gate that has kept all 35 archived shaders building. Do it deliberately, or accept that the archive rots from that commit onward.
- Removing the archive strips the last consumer of `includes/noise.glsl`, `includes/lighting/phase.glsl`, `includes/cloud/cloud_density.glsl` and `includes/water/gerstner.glsl`. Order archive removal *before* any "delete unused includes" sweep.
- Fixing the `includes/**/*.glsl` glob to cover top-level headers is a live-tree correctness fix, not an archive one: `pbr_unpack.glsl` is currently outside shader dependency tracking and is included by all three raygen shaders.
- No descriptor layout, pipeline or push-constant struct for any archived shader exists in the current tree — but the hosts are not gone, only untracked. The 48 pass files archived by 851092d (23 `.cpp`, 25 `.hpp`), `waves/fft_ocean_sim.cpp` among them, are recoverable with `git show 851092d:src/renderer/passes/_disabled/…`. Reviving the FFT ocean is a `git show`, not a re-derivation from `layout(...)` declarations, and the recovered host already pins the `N = 256` and `g = 9.81` the chain requires.
- Every behavioural statement on this page is read from the working tree or from git history, and none of it is a runtime measurement. This page makes no claim about whether the archived modules ever executed on a GPU: their hosts shipped in this repo — `FFTOceanSim` landed in 7355243 (2026-03-02) and dispatched all four `water/waves/` shaders — until e8eb9d1 deleted them.
