# render_bench: the Sponza scenes

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3: the frame's cost on a real scene).
**Status:** built; budgets for Frank.

## 1. Summary

`render_bench` measures two scenes that carry the frontier renderer's real load: `sponza_night_1080p` (RT direct
light's showcase, `assets/scenes/sponza_night.scene.json`: 37 lamps, no sun, ReSTIR DI at full load) and `sponza_1080p`
(`assets/scenes/khronos_sponza.scene.json` by day: the heaviest checked-in geometry, the sun and the sky), both at
1920x1080 with TAA and bloom at EV100 0. Neither has a budget: they are measured and reported (`--only sponza`).

## 2. Results

`render_bench --only <scene>`, medians of 100 frames, the GPU otherwise idle (the CPU busy: the run is marked noisy):

| scene | frame | rt.gi | di.denoise | rt.gi.denoise | lighting | di.shade |
|---|---|---|---|---|---|---|
| sponza_night_1080p | 7.64 | 1.83 | 1.37 | 1.03 | 0.88 | 0.57 |
| sponza_1080p | 8.18 | 2.24 | 1.74 | 1.01 | 0.50 | 0.60 |
| helmet_game_1080p (beside them) | 5.36 | 0.78 | 1.12 | 0.92 | | 0.53 |

A first run beside a game on the same GPU measured `rt.gi` at 5.1-5.3 ms: never judge a GPU budget beside another GPU
load.

## 3. Known gaps

- No budgets. `rt.gi` is the largest pass on Sponza (2.2 ms by day), then the two denoisers.
