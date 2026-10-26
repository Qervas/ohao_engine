# Bloom toned down: light meshes no longer glare across the frame

**Spec:** none. Frank's ruling (2026-10-26), following `2026-10-26-visible-lights.md`:
- tone bloom down so visible light meshes don't glare across the frame;
- keep light energy unchanged;
- do not dim the meshes' radiance as the fix.

**Status:** built and tested.

## 1. Summary

- **Where the glare came from:**
  - Since lights became visible meshes, a mesh's pixels carry the light's full radiance:
    `I/(πr²)`, about 19,000 for a 0.1 m bulb of intensity 600.
  - The bloom threshold passed that through untouched: the soft-knee contribution is ≈ 1
    far above the threshold.
  - The mip chain then spread it all the way down to 1×1. Each upsample adds the lower mip
    at 0.5 on top of the level's own content, and the composite adds the result at strength
    1.0.
  - The Karis average on the first downsample cannot stop it: it renormalises within each
    4-tap group, so a sphere many pixels wide is bright in every tap and keeps its full
    average.
  - Result: glare over a third to a half of the helmet, Khronos and Sponza captures.
- **The fix:** clamp the bloom input's luminance at 4, before exposure
  (`BloomPass::kBloomMaxLuminance`, `postprocess/bloom_threshold.frag`). A brighter source
  blooms as if it were luminance 4, so its glare stays around it, like any other bright
  surface's.
- **What it leaves alone:**
  - Pixels whose extracted luminance is under 4 bloom exactly as before.
  - Lighting never reads the bloom chain, so no light energy changes.
  - The meshes keep their radiance.

## 2. What was built

- `postprocess/bloom_threshold.frag`: a 4th push constant, `maxLuminance` (the slot was
  padding). After the soft knee, the extracted colour is scaled down to that luminance when
  it is above it.
- `BloomPass`:
  - `m_maxLuminance` (default `kBloomMaxLuminance` = 4) and `setMaxLuminance`;
  - `ThresholdParams.padding` → `maxLuminance`.
- Docs: AGENTS.md (in the emitter gotcha), ARCHITECTURE.md (the post line), and monograph
  `deferred/post` (why the Karis average could not hold a bright area, and the clamp).

## 3. Bugs found and fixed

- Bloom had no bound on its input, so a visible light's energy reached every pixel of the
  frame through the mip chain.

## 4. Spec corrections

None.

## 5. Results

**Choosing the clamp.** I rendered the helmet room as its golden does: deferred, 30
frames, TAA + SSAO + bloom, exposure 1. Glare coverage is the share of pixels changed by
more than 4 levels against the golden from before the light meshes (`1c5ce70`). It
includes the meshes themselves, about 2%.

| Clamp | Coverage |
|---|---|
| None (the golden at `3dc3fe6`) | 15.5% |
| 10 | 11.2% (a sphere at 10 blooms like the old wall hotspot at about 9.5) |
| 4 | 8.1% |

At 4 the difference map is the spheres plus a tight halo each; the image otherwise matches
the pre-mesh one.

**All bloom captures, coverage before → after** (more than 4 levels changed; more than 16
in brackets):

| Capture | Before | After |
|---|---|---|
| helmet_game deferred | 15.5% (9.6%) | 8.1% (3.2%) |
| helmet_game reference | 15.2% (9.5%) | 8.0% (3.1%) |
| damaged_helmet_deferred | 15.5% | 8.1% |
| Khronos metal-rough spheres deferred | 14.0% (7.4%) | 5.9% (2.1%) |
| Khronos metal-rough spheres reference | 13.7% | 5.7% |
| Khronos flight helmet deferred | 24.8% (13.1%) | 7.5% (3.8%) |
| Khronos flight helmet reference | 24.4% | 7.6% |
| Sponza deferred | 16.5% (8.6%) | 9.3% (0.9%) |
| Sponza reference | 16.5% | 9.8% |

**Lighting unchanged.**
- Every golden without bloom passed untouched with the change: Cornell deferred and
  reference, max_abs 0.
- The path-traced goldens passed within their floor (`cornell_box` 2 px and
  `cornell_box_rt_realtime` 3 px at 1 LSB).

**Goldens re-baselined.** Only the 9 bloom goldens (12 manifest entries), each looked at
first, with manifest notes:

| Golden | max_abs vs previous | Pixels changed |
|---|---|---|
| helmet_game / damaged_helmet deferred | 138 | 35.3% |
| helmet_game_reference | 131 | 35.2% |
| metal-rough spheres deferred / reference | 128 / 120 | 30.8% / 30.4% |
| flight helmet deferred / reference | 137 / 127 | 49.0% / 48.8% |
| Sponza deferred / reference | 197 | 38.2% |

Those percentages are the old glare going away.

**Look shots.** `C:/Users/djmax/ohao_refs/look_shots/bloom_tone/` holds:
- helmet deferred and reference, after (and the previous goldens as before, 640×360);
- Cornell deferred and reference with TAA, SSAO and bloom;
- `light_mesh` deferred (with bloom) and path traced (64 spp): `build/light_mesh.scene.json`,
  the room of `light_emitter_gpu_test` with its point, spot and area light in view.

The light meshes are visible in all of them, each with a halo of a few times its size.

**Gates.**
- `tools/check.py` (quick, gpu, docs): 38/38.
- Render goldens: 22/22 against the new baselines.
- Benchmarks were not rerun: the change is one clamp in a full-screen pass that already ran,
  so no budget is touched.

## 6. Known gaps

- **Fixed threshold.** The clamp is an absolute luminance before exposure. A scene exposed
  far darker or brighter than exposure 1 would want it scaled with the threshold; nothing
  scales either today.
- **Bloom strength.** The chain still adds roughly the full above-threshold energy (mip
  weights summing to about 2, strength 1.0), far more than a real lens scatters. The clamp
  bounds it per source; the overall strength is unchanged.
- **Path tracer.** Its cinematic bloom (`rt/cinematic_bloom_*`, used in the NRD cinematic
  composite) is a separate chain and is not clamped. The path-traced captures here have no
  bloom.

## 7. How to run

```
python tests/golden/render_golden.py tests/golden/manifest.json
build/Release/ohao run assets/scenes/helmet_game.scene.json --render out.png --mode deferred --frames 30 --post taa,ssao,bloom --exposure 1
```
