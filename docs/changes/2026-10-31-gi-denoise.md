# RT GI's denoiser: no blotches, no darkening

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, step F5: RT GI's quality against the
referee); F5c's known gap (`docs/changes/2026-10-31-frontier-f5c.md`, section 5).
**Status:** built. ReSTIR GI (F5d) is not needed for the walls; it stays the option for fewer rays or a moving camera.

## 1. Summary

**Flat walls are smooth and RT GI is no longer dark.** The helmet room's untextured walls showed low-frequency
blotches that a still camera kept, and settled 10% darker than the reference. Two faults in RT GI's denoiser, one
each:

- **The blotches: a spatial filter narrower than the noise.** Two rays a pixel at half resolution vary widely (a
  ray finds a lit patch or a dark corner). Two a-trous steps (1, 2) reach 6 half-resolution pixels: they turned
  the grain into blobs some 20 frame pixels wide, which 32 frames of history barely move. Four steps (1, 2, 4, 8)
  reach 30. The edge stops (depth, normal) are unchanged, so corners and contact shading stay.
- **The darkening: a clamp against a heavy tail.** The long (32-frame) history was clamped each frame to the
  range of the short (4-frame) one over the 3x3 neighbourhood. GI's samples are heavy-tailed: the mean includes
  rare bright bounces that 4 frames of 9 pixels rarely hold, so the range's top sat below the mean, and the clamp
  pulled every wall down. The history is now clamped up to the 3x3 minimum (a light turned on catches up in 4
  frames) and down to twice the maximum (one turned off or halved snaps down).

## 2. What was built

- `rt/gi_denoise.slang` `temporal`: the long history clamped to `[min, 2 max]` of the short one's 3x3.
- `RTGITechnique`: `kAtrousSteps` 4 descriptor sets per chain and slot; the diffuse chain runs steps 1, 2, 4, 8, the
  reflection's (F5b) 1 and 2 (its spread is the surface's roughness: wider steps would blur glossy reflections).
- `tests/renderer/check_gi_noise.py` (CTest `check_gi_noise`, gpu): the helmet room at 1280x720, a 240x240 patch
  of the left wall; blotchiness (the deviation of a 9x9 box less a 41x41 box over the mean) under 1.5% at frame 10
  and 1% at frame 60, frame 60's mean within 3% of reference quality (256 frames).

## 3. How it was found (measured, the wall patch, frame 60)

| change | blotchiness | wall mean (reference 0.514) |
|---|---|---|
| before | 4.13% | 0.461 |
| RT direct light at full resolution | unchanged | unchanged |
| GI at 8 rays | 1.60% | 0.497 |
| the clamp off | 3.30% | 0.508 |
| the clamp off, 200 frames, a 256-frame history | 1.69% | 0.498 |
| the clamp off, 4 a-trous steps | 0.31% | 0.505 |
| variance clipping, mean +- 2 sigma, 4 steps | 0.36% | 0.461 |
| variance clipping, mean +- 1 sigma, 4 steps | 0.40% | 0.424 |
| **built: [min, 2 max], 4 steps** | **0.41%** | **0.504** |

Blotchiness falling as 1/sqrt(frames) with a longer history said sampling noise, not a correlated pattern;
variance clipping darkened as the box clamp did (any bound near a heavy-tailed spread cuts the mean). The reference
itself measures 0.49%.

## 4. Bugs found and fixed

- **The clamp's darkening was the referee's metal gap.** F5b put the helmet's and the metal spheres' 8-9% shortfall
  against the path tracer on the reflection's split-sum structure; it was this clamp, on the diffuse bounce.
- `engine_tests` `sp2b_t10_realtime_converges_toward_the_reference` compared frame 1 and frame 16 against a
  16-frame reference whose own noise (32 rays, 16 frames) was as large as what four a-trous steps leave at frame 1:
  frame 1 measured 1.18 levels from it, frame 16 1.37. Against a converged reference (256 frames) frame 1 is 1.44 and
  frame 16 1.34, frame 16's mean +0.07 levels (frame 1's -0.87). The test's reference is 256 frames now.

## 5. Results

- **The referee** (`check_parity`, deferred/path tracer; before in brackets, F5b's run):

  | scene | ratio | stops |
  |---|---|---|
  | cornell_box | 0.986 (0.982) | 0.08 (0.10) |
  | damaged_helmet | 0.982 (0.925) | 0.14 (0.22) |
  | khronos_metal_rough_spheres | 0.969 (0.906) | 0.12 (0.23) |
  | area_light | 0.989 (0.980) | 0.06 (0.12) |
  | khronos_sponza | 1.014 (1.008) | 0.29 (0.29) |
  | blockout_room | 0.960 (0.949) | 0.07 (0.09) |

- `check_gi_noise`: frame 10 0.80%, frame 60 0.41%, mean x0.981.
- **Cost** (`render_bench`, medians; two steps against four in the same build): `rt.gi.denoise` on cornell_720p
  0.29 to 0.377 ms, on helmet_game_1080p 0.693 to 0.871 ms. Cornell's budget is 0.20 ms (over since F5b, 0.29).
- **Tests:** `tools/check.py` 58/58 (`check_gi_noise` the new one; 56/57 before the `sp2b_t10` fix, that test alone); render goldens 33/33 after the 17
  realtime deferred scenes were re-captured, each looked at against the old golden (walls smooth, bounce light up
  where the clamp cut it; max_abs up to 60 on Sponza at night, the lamps' bounce); reference and path-traced
  goldens unchanged.

## 6. Known gaps

- **No test of GI's lag behind a light change.** A light turned off, or dimmed past half, snaps down within the
  short history's 4 frames; a smaller dimming fades over the 32-frame history (about 0.5 s).
- **The wider filter costs 0.09 ms at 720p, 0.18 ms at 1080p.** `rt.gi.denoise`'s Cornell budget (0.20 ms) is for
  Frank to rule on, with F5b's second chain.
- A moving camera's history restarts at disocclusions, where the four steps carry the frame alone.

## 7. How to run

```console
$ python tests/renderer/check_gi_noise.py > /dev/null   # gpu
```
