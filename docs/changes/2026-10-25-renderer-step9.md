# Sub-project 2b, step 9: SSAO wired, SSR and SSS made cheap

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("SSAO", "Screen-space passes and order", "Frame cost", "Capture budgets", "Build order" step 9, as built; test plan test 5)
**Status:** Built and tested. Every budget the step gates measured under its budget, but every gated run at the step's end was flagged noisy by outside load, so the verdict is open (section 5).
- SSAO reaches the image for the first time: half resolution, fixed, applied to the ambient and environment terms only.
- SSR traces at half resolution; post's tonemap upsamples it against the depth buffer.
- SSS runs only on 16x16 tiles of materials with the new `subsurface` field, two indirect dispatches, in place.
- `engine_tests` 76/76 (3 new); `deferred_determinism_gpu_test` 65/65; render goldens 14/14, re-baselined in two commits of their own; `tools/check.py` 35/35.
- `serve_bench` `window_readback` 1.32 ms against its 2 ms budget (5.34 ms when the budget was set).
- No budget or tolerance was changed.

## 1. Summary

- **SSAO.** The pass ran when the `ssao` post effect was on, but `deferred_lighting.frag` declared no binding 10: its output never reached a pixel. Rewritten in Slang and wired into lighting.
- **SSR.** Half resolution, depth-aware upsample where it is added. Cornell 0.020 → 0.008 ms.
- **SSS.** No longer a full-screen pass keyed on an albedo guess. A material opts in with `subsurface`; the G-buffer pass flags the tiles; SSS dispatches over them. Cornell (no subsurface material) 0.042 → 0.004 ms.
- **Frame cost.** `cornell_720p` 0.85–0.87 ms, `helmet_game_720p` 0.94 ms, `helmet_game_1080p` 2.06–2.09 ms (SSAO there 0.817 → 0.165 ms).

## 2. What was built

- **`shaders/compute/ssao.slang`** (pass `ssao`, two dispatches).
  - `occlusion`, half resolution: pixel q shades full-resolution pixel 2q. Eight directions in the surface's tangent plane (from the G-buffer's world position and normal), rotated by the 4x4 noise texel (q.x mod 4, q.y mod 4), each marched four steps across the screen to the projection of a point `radius` away. A direction's occlusion is the highest elevation of its samples within the radius, faded by distance, seen from the point lifted `bias` along its normal. Writes (ao, distance from the camera).
  - `blur`: a 4x4 depth-aware window (one noise period, so each of the 16 rotations once).
  - **Noise:** `SSAOPass::noise(seed)` is a seeded Fisher-Yates permutation of the 16 strata (k + 0.5)/16, packed 4 bits a texel into the push constants (`SSAOPush`, 128 bytes): reseeding uploads nothing and idles nothing.
- **Lighting** (`deferred_lighting.frag`): `ssaoAt` upsamples the half-resolution AO (4 taps, bilinear x depth within 5%, else the closest), and multiplies only the constant ambient and the IBL terms; not direct light, not RT GI.
- **`shaders/postprocess/ssr.slang`** (pass `ssr`): the old march at half resolution; alpha carries the pixel's view depth. **`tonemapping.frag`**: `ssrAt` upsamples it against the depth buffer (binding 3, linearized with the projection's [2][2] and [3][2]), reading depth only where a neighbour reflects; `ssrOn` in the push constants.
- **`shaders/postprocess/sss.slang`** (pass `sss`): `blurH` (lit → scratch, over the skin tiles and 2 tile rows above and below) and `blurV` (scratch → lit, in place, over the skin tiles), each `vkCmdDispatchIndirect`. Depth weights compare distances from the camera.
- **Tile flags** (`core/gbuffer.frag`, `GBufferPass`): a fragment of a material with `subsurface` > 0 (and no emission) writes the strength, negated, into the normal target's w and flags its tile (`atomicOr` after a plain read; the first flagger appends the tile to the list and bumps the indirect count). `layout(early_fragment_tests)`: only visible fragments flag. The pass resets the buffer (header and this frame's mask) before drawing; `sssTilesFlagged()` reads the count back.
- **`subsurface`** on `MaterialOverride` (reflected, written, schema'd: 0..1) and `render::MaterialDesc`; the material table's row 2 z.
- `VulkanRenderer::setAmbientIntensity` (the constant ambient, for test 5's scenes). The lit image gained storage usage. SSAO, SSR and SSS use the `rtc` helpers; the unused `ssao_output` graph import is gone. `render_budgets.json` `currentStep` 9.

## 3. Bugs found and fixed

- **SSAO never reached the image** (no binding 10 in the lighting shader). Beyond that the old shader mixed a world-space normal with view-space positions, read its noise at `coord * width/4` (not `coord mod 4`), decoded [-1, 1] data as [0, 1], and never read `bias`.
- **SSS blurred Cornell's red wall** (and reddish parts of the helmet) every frame: its "skin" was any warm, non-metal albedo, and its depth weight compared distances from the world origin, not the camera. Visible as the red wall smeared over the ceiling edge in the old goldens.
- **A frame without SSR added the HDR input to itself**: the tonemap's SSR binding fell back to the HDR image and was added unconditionally. Now `ssrOn` gates it.
- **SSAO's push block was 192 bytes** in this step's first commit, over the 128 bytes every device guarantees (caught by the monograph review); packed to 128 and held by a `static_assert`.

## 4. Spec corrections

Recorded in the spec's step 9 as-built note and in "Screen-space passes and order":
- SSS tiles come from a material `subsurface` field flagged by the G-buffer pass, not from G-buffer material IDs (there are none) and a classification pass.
- "The noise covers 16 distinct texels" is checked on the generator (a permutation of the 16 strata) and by the shader's lookup.
- The `render.settings` switches for SSR and SSS stay with `render.settings` in step 15.

## 5. Results

- **Tests.** `engine_tests` 76/76; new: `sp2b_t05_ssao_darkens_only_the_ambient_term` (ambient-only frame: 3191 pixels darker with SSAO, 0 brighter; point-lit frame unchanged, max_abs 0), `sp2b_t05_ssao_twin_renders_are_identical`, `sp2b_s9_sss_runs_only_on_flagged_tiles` (0 tiles without `subsurface`, 16 with, the box changed). `deferred_determinism_gpu_test` 65/65 (`t5` the 16 strata; the seed-0 noise pin, now `fa4bd56a0ec70335`). `check_validation.py` 6/6 clean. `tools/check.py` 35/35.
- **Goldens** 14/14 after re-baselining, each looked at first: `cornell_box_deferred` max_abs 34 on 4.5% of pixels (the red wall's edge sharp again), `cornell_box_reference` 34 on 4.7%; `damaged_helmet_deferred` 65 on 29.3%, `helmet_game_deferred` 65 on 29.9%, `helmet_game_reference` 65 on 30.2% (SSAO for the first time, half-resolution SSR, no heuristic SSS). The catalogue, schema, CLI and serve protocol goldens took the new field (schema hash, snapshot sizes).
- **`render_bench --check`** (RTX 5070 Laptop GPU; noisy, other processes 1.3 cores):

  | scene / pass | budget | measured |
  |---|---|---|
  | `cornell_720p` frame | 1.50 | 0.853 |
  | `cornell_720p` sss | 0.02 | 0.004 |
  | `cornell_720p` ssr | 0.04 | 0.008 |
  | `cornell_720p` sky + particles | 0.03 | 0.011 |
  | `cornell_720p` post | 0.05 | 0.030 |
  | `cornell_720p` ssao | 0.00 | not run |
  | `helmet_game_720p` frame | 3.0 | 0.935 |
  | `helmet_game_1080p` frame | 5.0 | 2.055 |

- **`serve_bench --check`** (noisy, Spotify.exe at about a core): `window_readback` 1.32 ms (budget 2), `window_capture_inline` 10.2 ms (budget 20, step 10's gate), `window_capture_png` 6.3 ms (30), `window_step_present` 0.24 ms (8); every CPU op within its budget.

## 6. Known gaps

- The SSS tile buffer is one host-mapped buffer (device-local when resizable BAR exists): without BAR the G-buffer's atomics cross the bus, and `sssTilesFlagged()` reads whatever the newest submitted frame left.
- A skin fragment later covered by nearer geometry still flags its tile (wasted work, not wrong pixels).
- SSAO counts a direction whose end falls behind the camera as unoccluded.
- `setSSAORadius`, `setSSAOIntensity` and `setSSSWidth` have no callers; the defaults always run (`render.settings`, step 15).

## 7. How to run the new tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
$ build/Release/serve_bench.exe --check tests/bench/serve_budgets.json > /dev/null   # slow
```
