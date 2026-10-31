# render_bench: the Sponza scenes

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3: the frame's cost on a real scene).
**Status:** built; budgets for Frank.

## 1. Summary

`render_bench` measures two scenes that carry the frontier renderer's real load: `sponza_night_1080p` (RT direct
light's showcase, `assets/scenes/sponza_night.scene.json`: 37 lamps, no sun, ReSTIR DI at full load) and `sponza_1080p`
(`assets/scenes/khronos_sponza.scene.json` by day: the heaviest checked-in geometry, the sun and the sky), both at
1920x1080 with TAA and bloom at EV100 0. Neither has a budget: they are measured and reported (`--only sponza`).

## 2. Results

The first runs were noisy (a game running on the same GPU and CPU: `wait_quiet.py` found SkyrimSE.exe at 135% of a core
for 5 minutes), so these are not a baseline: frame medians 17.0 ms (night) and 19.7 ms (day). In both the largest pass
was `rt.gi`, about 5.1-5.3 ms, against 0.7 ms in helmet_game_1080p in the same conditions; then `di.denoise` (1.6, 2.4
ms) and `rt.gi.denoise` (1.0 ms). A quiet run is needed before any budget.

## 3. Known gaps

- No budgets; `rt.gi`'s cost on Sponza is the next thing to measure on a quiet machine.
