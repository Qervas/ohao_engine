# Sub-project 10, step 4: the gate and the close

**Spec:** `docs/specs/2026-10-28-animation-design.md` ("Hand-test acceptance", "As built (step 4)")
**Plan:** `docs/plans/2026-10-28-animation.md`, Task 4 (steps 1, 2 and 4 ticked; step 3, the hand
test, open)
**Status:** built, waiting for Frank. Roadmap row 10 is "built, waiting for Frank" (the hand test and
the provisional rulings). Row 9 is unchanged.

## 1. Summary

Sub-project 10 is built. It has four parts:

- **Reading and stepping.** Skinned glTF characters are read by our own reader. They are stepped
  as saved integers, so `world.hash`, files and snapshots hold an animation exactly.
- **Skinning.** The GPU deforms each instance's own mesh. Every renderer reads that mesh: the
  G-buffer, meshlets, RT shadows and GI through a refitted BLAS, and the path tracer. Motion
  vectors come from the previous pose.
- **The enemies.** The play scene's two enemies are skinned CC0 characters. They move, aim, fire,
  reload, flinch and die, and their events play sounds.
- **The player.** The player holds skinned arms. Their reload is timed to the weapon's, and they
  can aim down the sights.

The gate found one engine defect, which is now fixed: the skinned BLAS refits ran one after
another and missed the play scene's `rt.blas.refit` budget. Every SP10 budget now passes on
quiet runs.

Validation is clean in raster and RT. All 30 goldens are exact; Task 4 moved none.

Frank's direction (2026-10-28):

> Cartoon CC0 minions are fine for now; the technology must be high quality and aim at
> photorealism, so that photoreal art dropped in later holds up.

No art changed for it. What the technology does to meet it is recorded in the spec's "As built
(step 4)".

## 2. What was built

- **The gate.** `python tools/check.py --all`; every SP10 budget on quiet runs; validation;
  goldens; `feel_bench`. The results are in section 5.
- **Docs.**
  - `AGENTS.md`:
    - the `anim_tests` and `skinning_gpu_test` suites;
    - the animation notes and gotchas;
    - the arms, ADS and `rateFrom`.
  - `docs/ARCHITECTURE.md`: the animation flow and a recipe, "add an animated character".
  - `docs/agents/ohao-cli.md` and `docs/agents/ohao-serve.md`:
    - `animation.info`;
    - `render.stats`' `skinning`;
    - forcing a state through `component.set` on the `Animator`, with a new transcript,
      `docs/agents/examples/animation.jsonl`.
- **Monograph.**
  - A new `scene/animation` unit, covering:
    - saved integers;
    - the pose at render rate;
    - `rateFrom`;
    - a skinned instance as its own mesh;
    - the batched refit;
    - previous-pose motion vectors;
    - `JointAttach`.
  - The combat unit now describes:
    - ADS as built (ruling 11): the aim stepped over `adsTime`, dropping to 0 while reloading
      or dead, and the cone times `1 − 0.5·aim`;
    - the procedural kick at half amplitude over the arms' fire clip (ruling 8).
  - The feel unit describes the arms under the `ViewModel` and the `DrawnFov` of live frames.
  - All three are cite-checked from the repo root, and the site was rebuilt (105 pages).
- **The hand test.** The spec's "Hand-test acceptance" table now has rows A1–A11 for SP10:
  - A1 enemy bodies, A2 hit reactions, A3 the death clip (ruling 7);
  - A4–A6 the arms at idle, firing, and the kick's feel (ruling 8);
  - A7 reload, A8 reload sounds at the ear;
  - A9 ADS (ruling 11), A10 sprint, A11 latency and pacing.

  Frank's notes column is empty and every row is open.
- **The spec's "As built (step 4)"**: the gate, the defect, Frank's direction, and the rulings
  still provisional.

## 3. Bugs found and fixed

- **The skinned BLAS refits ran one after another** (`perf(render)`, e10c52f1).
  - **Cause:** `rt.blas.refit` recorded one `vkCmdBuildAccelerationStructuresKHR` per skinned
    instance. Each call sat behind a full barrier and shared one scratch buffer. With the arms,
    the play scene refits three BLAS a frame and measured 0.101 ms (p95 0.102) against its 0.1
    budget on a quiet run.
  - **Fix:** `RTAccelerationStructure::recordBlasUpdates(cmd, span<BlasUpdate>, ...)` records a
    frame's updates in one call after one barrier. Each update gets its own scratch slice at the
    device's `minAccelerationStructureScratchOffsetAlignment`.
  - **Result:**

    | Scene | Before | After |
    |---|---|---|
    | `play_anim_1080p` | 0.101 ms | 0.049–0.051 ms |
    | `anim16_1080p` | 0.136 ms | 0.013 ms |

  - **Test:** `skinning_gpu_test` `t24_refits_batched` steps two moving rigs for 4 frames and
    expects 8 updates in 4 calls. `render.stats` `skinning.blasUpdateCalls` is new. The test
    failed with 0 calls before the fix and passes after it (23/23).
- **The monograph's animation unit cited the old one-call refit** (ab7a0918). After e10c52f1 the
  cited line no longer existed. As a result, `check_docs` failed at the monograph command in
  `AGENTS.md`, and so did `monograph_structure`. The cite now points at the batched line.

## 4. Spec corrections

None to the design. The spec gains:

- the hand-test rows A1–A11;
- "As built (step 4)".

## 5. Results

All results are from runs on 2026-10-28.

**`python tools/check.py --all`** (after e10c52f1): 46 of 51 passed. The five failures:

| Test | Result | Why |
|---|---|---|
| `check_docs` | 89 passed, 1 failed | The stale monograph cite. After ab7a0918: 124/124 (24 skipped). |
| `monograph_structure` | failed | The same cite. After ab7a0918: 16/16. |
| `scene_bench` | noisy | Noisy in the gate and in one rerun. Not SP10's (see below). |
| `render_bench` | quiet, 28 passed, 1 failed | `cornell_720p` lighting, 0.367 ms (p95 0.369) against 0.32. This miss has stood since sub-project 5 step 4 and is parked by Frank. |
| `feel_bench` | quiet, 1 failed | Pacing (below), SP9 ruling 21. |

Every functional suite passed. Those named for this sub-project:

| Suite | Passed |
|---|---|
| `skinning_gpu_test` | 23/23 |
| `deferred_determinism_gpu_test` | 80/80 |
| `cull_parity_gpu_test` | 49/49 |
| `check_validation.py` | 14/14 |

`anim_tests`, `combat_tests`, `runtime_tests`, `check_live.py` and `e2e_mcp.py` also passed.

**SP10 budgets.** All runs were quiet:

- `render_bench`: calibration CPU 23.20 → 23.47 ms against a reference of 28.25; GPU 1.554 →
  1.888 ms against 1.551.
- `physics_bench`: other processes used 30% of a core.

| Budget | Median | p95 | Budget | Verdict |
|---|---|---|---|---|
| `anim16_1080p` `skin` | 0.0036 ms | 0.0052 | | |
| `anim16_1080p` `skin.bounds` | 0.0065 | 0.0080 | | |
| `anim16_1080p` `skin` + `skin.bounds` | 0.010 | | 0.25 | pass |
| `anim16_1080p` `rt.blas.refit` | 0.0128 | 0.0128 | 0.6 | pass |
| `play_anim_1080p` `skin` | 0.0023 | 0.0035 | | |
| `play_anim_1080p` `skin.bounds` | 0.0013 | 0.0014 | | |
| `play_anim_1080p` `skin` + `skin.bounds` | 0.0036 | | 0.05 | pass |
| `play_anim_1080p` `rt.blas.refit` | 0.051 | 0.060 | 0.1 | pass |
| `play_hud_1080p` frame | 2.269 | 2.278 | 2.39 | pass |
| `play_hud_1080p` `hud` | 0.0043 | | 0.05 | pass |
| `anim16_step_ms` | 0.0016 | 0.0018 | 0.05 | pass |
| `pose16_sync_ms` | 0.0071 | 0.0073 | 0.1 | pass |
| `anim_step_ms` | 0.0008 | 0.0011 | 0.01 | pass |
| `pose_sync_ms` | 0.0099 | 0.0113 | 0.02 | pass |

`pose_sync_ms` was 0.0271 in step 3, on a noisy run. On a near-quiet run (calibration ×1.02) it
measured 0.0098 ms median and 0.0115 p95, and 0.0099 / 0.0113 in the gate. The step 3 number was
the machine's load, not the arms.

No budget moved (OPEN ruling 16).

**`scene_bench`: noisy**, both in the gate and in the rerun.

- In the rerun, its calibration took 61.9 → 68.6 ms against a reference of 23.2. Other processes
  used 173% of a core (chrome, steamwebhelper), and Defender's `MsMpEng.exe` was at 130% just
  before.
- Clean-round medians:

  | Operation | Median |
  |---|---|
  | bench10k `save_pretty` | 23.6 ms |
  | bench10k `load` | 37.0 ms |
  | bench10k `validate` | 24.6 ms |
  | bench10k `snapshot` | 1.11 ms |
  | bench10k `restore_unchanged` | 1.52 ms |
  | bench10k `apply_100_field_set` | 0.446 ms |
  | bench10k `query_1000_ids` | 3.22 ms |

- These are not a verdict, and nothing SP10 built runs in its scenes.

**Validation** (`OHAO_VALIDATION=1`, `check_validation.py`): zero messages through teardown in
every run:

- the skinning rig: deferred, `rt_offline`, `rt_realtime` and `rt_realtime` through NRD;
- the play scene a second into the fight: the same four.

**Latency and pacing** (`feel_bench --check`, a 1920x1080 window, 240 Hz, present wait, 60 s per
mode, quiet):

| Mode | Look latency median | p95 | Frame time median | p99/median | Stalls |
|---|---|---|---|---|---|
| immediate | 6.62 ms | 45.1 ms | 2.538 ms | 2.62 | 21 |
| mailbox | 4.94 ms | 10.5 ms | 2.529 ms | 1.83 | 1 |
| fifo | 33.7 ms | 265 ms | 4.304 ms | 61.4 | 143 |

- `look_latency_ms` 6.62 is within 15: pass.
- `frame_p99_over_median` 2.62 misses 1.5: this is SP9 ruling 21's open gate, awaiting Frank.
  Nothing here tried to fix pacing.
- The fifo mode ran while other processes used 56.5% of a core (two `Grok Bot.exe`).

**Goldens:** 30/30 exact; none moved in Task 4. `cli_anim_rig_pt` max_abs 0.

**`tools/list`:** 24,137 bytes, unchanged (no tool or description changed; cap 24,576, ruling 13).

**Content:** `Fox.glb` and `BrainStem.glb` are absent (deleted in step 2). Nothing from Somnabulate
or Flat42 is committed.

During the gate, a session of `ohao_game` on the play scene ran for about ten minutes, started
outside this work. Its window held the `ohao_game.exe` link until it closed. No hand-test notes
came back, so the hand test stays open.

## 6. Known gaps and open items for Frank

- **The hand test**, A1–A11 in the spec's "Hand-test acceptance": not played. Plan Task 4 step 3
  stays unticked.
- **Provisional rulings:**
  - 1: CC0 or our own content.
  - 2: in place, no root motion.
  - 3: compute skinning.
  - 4: deterministic poses from the step count.
  - 5: the order.
  - 6: what is out of scope.
  - 7: the death clip, with the `Transform` left upright.
  - 8: the procedural kick at half amplitude over the fire clip.
  - 11: ADS:
    - 0.2 s to aim;
    - the cone halving linearly at full aim;
    - 75° → 55° in live frames only;
    - aim dropping during a reload.
  - 13: `tools/list` at 24,137 of 24,576 bytes.
  - 16: the SP10 budgets at their proposals.

  Ruling 10 is done.
- **Pacing (SP9 ruling 21):** `frame_p99_over_median` 2.62 against 1.5, from the presentation
  engine's stalls.
- **Standing misses, not SP10's:**
  - `cornell_720p` lighting (0.367 against 0.32, parked);
  - `scene_bench` noisy on this machine today.

## 7. How to run

```
python tools/check.py --all
build/Release/skinning_gpu_test.exe            # t24_refits_batched among 23
build/Release/render_bench.exe --check tests/bench/render_budgets.json --only play_anim_1080p
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
python tests/validation/check_validation.py -v
build/Release/feel_bench.exe --check tests/bench/feel_budgets.json --seconds 10
python tests/docs/check_docs.py
python site/tools/check_unit.py scene/animation
```
