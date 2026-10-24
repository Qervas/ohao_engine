# Sub-project 2b, step 4: correctness of what renders

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("RT shadows", "RT GI", "Determinism", "Why", "Test plan" tests 9 and 12, "Build order" step 4, as built)
**Status:** Built. Test 9 (lights part) and test 12 (seed part) pass. `tools/check.py` quick + gpu + docs 34/34. Render goldens 11/11 after three re-baselines, each in its own commit. Every deferred scene is exact, and every example matches its CLI twin. Validation is clean. `render_bench` ran clean (not noisy); no budget is gated before step 7. No budget or tolerance was changed.

## 1. Summary

The frame now shows what the scene says:

- **Shadows.** RT shadows trace the scene's lights. Each light that casts shadows gets its own channel of the shadow mask, and `Light.castShadows` turns a light's shadows off. Before, one hard-coded light's visibility darkened every light.
- **Sky.** The sky reaches the image again.
- **Seeds.** RT shadow and GI noise is seeded by the render seed and the frame index since the last reset.
- **Base color.** The path tracer applies an untextured material's base color once instead of twice.

Two dead inputs are gone: the path tracer's unread light argument and RT GI's placeholder binding 5. Moving particles in front of SSS exposed three particle bugs, now fixed.

## 2. What was built

### Scene lights in RT shadows

- **`Light.castShadows`** (default true) is reflected, saved and documented, and carried on `render::LightDesc`.
- **Shadow channels.** The light table gives the first 4 raster lights that cast shadows, in light order, an RT shadow channel each. `LightData::params.w` records the result:
  - -1 (`kShadowNever`): `castShadows` false, or an emissive proxy;
  - 0..3: the light's mask channel;
  - 4 (`kShadowNoSlot`): casts shadows but no channel was left.
  `RenderScene::shadowLights()` lists the channel owners in order.
- **The shadow pass** (`rt_shadow.rgen`) traces up to 4 lights from push constants into an RGBA8 mask:
  - **Sampling:** a sphere of the light's `radius`; a rectangle's area (its position a corner, as the path tracer samples it); or a directional light's angular radius.
  - **Range:** a point beyond a light's range is left lit.
  - **Ray budget:** the 8 rays a pixel are shared, `max(1, 8 / lights)` each.
  - **No casters:** with no light that casts shadows, the pass does not run.
- **Lighting** (`deferred_lighting.frag`) reads the mask once and takes each light's own channel. A light without a channel is unshadowed under RT shadows; with RT shadows off, a directional light still uses CSM.
- **Lights from the scene.** The deferred renderer receives the shadow lights each frame (`setShadowLights`). With no scene lights, the fallback sun is channel 0, at the real sun's angular radius (0.00465 rad).
- **The path tracer** took a light position, intensity, colour and radius in `render()` and never read them; it lights from the scene's light table. Those parameters are deleted from `PathTracer::render` and `IRTRendererProfile::render`, and the hard-coded values with them.

### Sky and particles before SSS

- **Pass order:** the lit image now gets sky, then particles, then a barrier (colour writes to compute and fragment reads), then SSS, SSR and post. The pass registry follows (`sky`, `particles`, `sss`, `ssr`).
- **Particles:**
  - the buffer is zeroed before its first use (`ParticleSystem::clearOnce`);
  - the update shader sets `instanceCount` to the highest live slot + 1 (`atomicMax`, independent of thread order), and the vertex shader takes the slot from `gl_InstanceIndex`;
  - the pass runs only while a particle can be alive, from an emit until its longest lifetime has passed. The frame clock advances every frame regardless.

### Smaller fixes

- **SSAO import format:** the render graph imports SSAO's output as `SSAOPass::kOutputFormat` (R16_SFLOAT), the format the image has; it was imported as R8_UNORM.
- **RT GI binding 5** is deleted. It held the depth buffer as a placeholder for "the current frame's direct light"; GI traces before lighting and the shader never read it.

### Seeds

- `DeferredRenderer` counts frames since `resetTemporalState` and takes the renderer's seed, both at creation and from `setRenderSeed`.
- `rt_shadow.rgen` and `rt_gi.rgen` draw every sample from a PCG hash of (render seed, frame index, pixel, light/sample, dimension). GI's private frame counter and its `m_totalTime * 60` input are gone.
- A fresh path tracer starts its sample index at a seed set before its first frame.

### The double tint

The 1x1 legacy tint texture, which re-applied an untextured material's base color in `pt_closesthit` as `pow(texel, 2.2)`, is deleted with `MaterialDesc::legacyRtTintTex` and `render::legacyRtTintTexture`. The material pack's third row is now (roughMetalTex, emissiveStrength, unused, unused).

### Tests

- **Test 9, lights part:**
  - `engine_tests` `sp2b_t09_scene_lights_cast_rt_shadows_and_cast_shadows_false_does_not`:
    - a box over a floor, lit by one point light: with `castShadows` true, about 945 px fall in the box's shadow, centred at x 0.69 of the width when the light is on the left and 0.30 when it is on the right;
    - `castShadows: false` darkens nothing;
    - the light table assigns channels 0, 1, -1, 2, 3, 4 to six lights, the third of which does not cast.
  - `reflect_tests` `sp2b_t09_lint_finds_hard_coded_light_positions` and `sp2b_t09_no_hard_coded_light_in_render_or_gpu`: no light position is written as numbers in `ohao/render` or `ohao/gpu`.
- **Test 12, seed part:** `deferred_determinism_gpu_test` `t12`. The helmet room with seed 7 differs from seed 0 in 905,779 bytes, and seed 0 again is bit-identical.
- **Sky:** `engine_tests` `sp2b_s4_the_sky_reaches_the_final_image_through_sss`. A frame that runs SSS shows the sky across the whole top row; it was black before.

## 3. Bugs found and fixed

1. **One shadow for every light.** The RT mask was traced toward a hard-coded point at (0, 4.5, 0), and lighting multiplied every light by it.
2. **Sky and particles never reached the image.** They drew into the lighting output after SSS had copied it, and post reads SSS's copy. SSS runs every frame.
3. **Particles drew uninitialized memory.** The device-local particle buffer was never cleared, so leftover memory read as live particles. When particles moved in front of SSS, the helmet golden changed by max_abs 237, and the example and CLI images stopped matching. Two more particle bugs came with it: every instance drew particle 0, and the pass ran with nothing emitted.
4. **SSAO imported in the wrong format** into the render graph (R8_UNORM for an R16_SFLOAT image).
5. **RT GI's binding 5** was a placeholder no shader read, holding the depth buffer.
6. **The RT seeds ignored the render seed.** RT GI counted frames itself, partly from `m_totalTime`.
7. **The double tint.** An untextured material's base color was applied twice in the path tracer, so a 0.73 grey wall reflected about 0.53.
8. **A fresh path tracer's first frames ignored a seed set before them.** The seed applied only at the next reset.
9. **Two stale comments:** `types.glsl` said `params.w` was unused, and the bindless manager's name lookup mentioned the tint textures.

## 4. Spec corrections

- **The path tracer's light:** it had no hard-coded light in use, only an unread argument, which is deleted. `castShadows` controls the raster frame; the path tracer, the ground truth, always traces visibility.
- **RT GI binding 5** is deleted rather than given "the real resource": there is no direct light of the frame when GI traces.
- **RT shadows as built in step 4:**
  - the 4 channels go to the first 4 casting lights in light order;
  - the 8 rays a pixel are shared among them;
  - the fallback sun is channel 0.
  Step 7 still delivers 1 ray a light, per-pixel importance, the filter and the temporal pass.
- **"Why":** SSS runs every frame, so the sky and particles never reached an image (the spec said "while SSS is active").

## 5. Results

### Goldens

Each change was re-baselined in its own commit after looking at it. The CLI twins match their examples throughout.

| Change | Scene | Against the previous golden |
|---|---|---|
| Scene lights | `cornell_box_deferred` | max_abs 2 on 0.94% (2,177 px) |
| | `damaged_helmet_deferred` | max_abs 84 on 6.87% (15,825 px) |
| | `helmet_game_deferred` | max_abs 84 on 6.77% (15,605 px) |
| Seeds | `cornell_box_deferred` | max_abs 3 on 39.8% (91,607 px) |
| | `damaged_helmet_deferred` | max_abs 18 on 59.0% (136,024 px) |
| | `helmet_game_deferred` | max_abs 18 on 58.0% (133,605 px) |
| Base color once | `cornell_box` | max_abs 70 on 100% (rmse 23.4) |
| | `damaged_helmet` | max_abs 91 on 99.8% (rmse 38.9) |
| | `cornell_box_rt_realtime` | max_abs 41 on 99.7% (rmse 10.6) |

What I looked at in each:

- **Scene lights:** in the helmet's worst region, a highlight on the visor that the old mask had hidden; the Cornell box's ceiling light sits near the old point, so that scene barely moves.
- **Seeds:** scattered per-pixel noise of the same amplitude.
- **Base color once:** brighter, less saturated walls; the helmet's textured surfaces keep their look.

The sky, particle, SSAO and binding-5 changes moved no golden.

The three golden commits use the subject prefix `test(golden):`, the same as steps 2 and 3. The spec's "Determinism" section names it `golden(render):`.

### Tests

- `engine_tests` 62/62, `reflect_tests` 40/40, `scene_tests` 62/62, `deferred_determinism_gpu_test` 45/45.
- `tools/check.py` quick + gpu + docs 34/34, including:
  - `check_cli` 29/29, `check_serve` 166, `check_live` 26/26, `e2e_mcp` 20/20;
  - `check_validation` 2/2 clean, and the `ohao_game` presentation gate;
  - `check_docs` 65 commands, monograph OK.
- `render_golden` 11/11.
- Goldens updated for the new field: the catalogue and scene schema; the CLI and serve protocol goldens (schema hash, snapshot sizes, world hashes); and the two guide outputs that print them.

### Benchmarks

`render_bench --check`, clean (not noisy). Calibration: CPU ×0.93, GPU ×1.03. No budget is gated yet.

| Scene | Frame (GPU) | `rt.shadows` | `rt.gi` |
|---|---|---|---|
| `cornell_720p` | 4.881 ms | 0.464 ms | 4.019 ms |
| `helmet_game_720p` | 5.503 ms | 0.113 ms | 4.370 ms |
| `helmet_game_1080p` | 13.145 ms | 0.236 ms | 10.592 ms |

The helmet room traces four lights at 2 rays each, and the range check skips lights that cannot reach a pixel, so its shadow pass is cheaper than Cornell's single light at 8 rays.

## 6. Known gaps

- **Shadow ray budget:** RT shadows share 8 rays a pixel among up to 4 lights, so each of four lights gets 2 rays and a noisier penumbra. Step 7 replaces this with 1 ray a light plus the filter and temporal accumulation.
- **GI after a resize:** `DeferredRenderer::onResize` does not reset the temporal state, so the first RT GI frame after a resize blends at 0.3 against a cleared history.
- **Particle slots run out:** the emitter takes slots from a counter that never decreases, so slots are not reused once `maxParticles` have been emitted. That system's rework is outside 2b.
- **`registerName`** on the bindless manager has no caller left.

## 7. Running the tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
```
