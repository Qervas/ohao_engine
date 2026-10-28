# Sub-project 10, step 2: the enemy's full body

**Spec:** `docs/specs/2026-10-28-animation-design.md` ("Assets and content", "The step", "The frame",
"Performance budgets", "Determinism", "Test plan" tests 23 and 25, rulings 1–18, and "As built
(step 2)")
**Plan:** `docs/plans/2026-10-28-animation.md`, Task 2 (steps 1–7 ticked)
**Status:** built; Task 2 of 4. Roadmap row 10 stays open; row 9 stays "built, waiting for Frank".

## 1. Summary

The play scene's two enemies are skinned characters. Each holds a rifle in its right hand, with
its muzzle light at the barrel's tip. They idle, chase at a run, raise their rifles to aim (pitched
toward the player), kick on each round, flinch when hit, reload with handling sounds, and die by a
clip that ends on the floor. Their states come from the enemy AI and the step's combat events,
through saved integer state. A fight, its restart and a restore replay bit for bit, as before.

The one simulation change is OPEN ruling 7 (provisional): a dead enemy's Transform stays upright and
its death clip is the fall. Every other combat and physics expectation is unchanged. Exactly the two
play-scene goldens moved; the other 28 are exact.

## 2. What was built

- **Content** (`assets/characters/toon_enemy/`):
  - Quaternius' "Character Enemy" from the Toon Shooter Game Kit, CC0 1.0. Source: poly.pizza
    `mdGe4IN31v`, updated 2024-03-16, sha256 `0af9db94…30ab`. It was staged for this step by Grok
    Bot in `C:\Users\djmax\ohao_refs\staging\sp10_enemy\` with `SOURCES.txt`.
  - It is rebuilt by `tools/anim/make_enemy.py` (ours, MIT), in pure Python. Blender was not in this
    session's allowed tools, so the brief's fallback applied. The script checks the sha256, and two
    runs write the same bytes.
  - What the script does:
    - merges the body and head into one mesh on one 43-joint skin;
    - bakes the armature's ×100 scale and the export transform into metres;
    - fits the character to the 1.8 m capsule (upstream it is 2.28 m) and turns it to face −Z;
    - recomputes the inverse binds (upstream, the bind pose is a T-pose and the rest pose has the
      arms down);
    - drops the duplicate `CharacterArmature|*` clips and the unused ones.
  - `LICENSE.md` and `README.upstream.md` record the source, version, date, author and every change.
- **Clips:**
  - Kept from the kit: idle, walk, run (Run_Gun), walk_aim, run_aim and death.
  - Made by the script:
    - `aim_idle`: Idle_Shoot's first frame, held.
    - `rest`: additive zero.
    - `aim_up` and `aim_down`: the spine pitched ±60° (Abdomen 20°, Torso 25°, Neck 15°).
    - `fire` and `hit`: Idle_Shoot and HitReact, each relative to its own first frame.
    - `reload`: 2 s; the torso dips and the left arm drops to the magazine and back.
  - Additive clips are written as rest × delta, matching the engine's additive layer (rest⁻¹ ×
    layer).
- **The rifle** is the kit's own `AK`, from the same file, written as `rifle.glb` in the `Index1.R`
  joint's frame. The separate `Rifle.glb` that was offered is in another kit's texture-atlas style
  and has no grip transform for this rig, so it was not used.
- **`Fox.glb` and `BrainStem.glb` are deleted** (ruling 10, provisional). Nothing loads them; only
  history prose in two monograph pages names them.
- **The graph** (`assets/anim/enemy.animgraph.json`):
  - The `body` layer has four states:
    - `move` blends idle, walk and run by speed at 0, 1.25 and 2.5 m/s.
    - `combat` blends the aim set by speed, entered when the mode is Attack.
    - `death`, then `dead`, which holds the death clip at its end.
  - Four additive layers, masked to `Abdomen` and below:
    - `aim`: a blend by `aimPitch` at −60, 0 and +60°;
    - `fire`: a kick per `shot`;
    - `reload`: per `reload`;
    - `hit`: a flinch per `hit`.
  - Events: `mag_out` at 0.6 s, `mag_in` at 1.3 s and `bolt` at 1.75 s into the reload; `body_fall`
    at 0.55 s into the death clip.
- **The driver** (`ecs/enemy_anim.cpp`, registered as `enemy` by `installAnimation`):
  - The parameters come from saved state: `speed` from horizontal velocity, `aimPitch` (while
    attacking), `mode` and `alive`.
  - The events (`shot`, `hit`, `reload`, `death`) come from the step's combat events after the step's
    last `restart`.
- **Death** (`ecs/enemy_ai.cpp`, OPEN ruling 7): `lieDown` is now `die`. The enemy stops and its
  character is parked as before, but its Transform is untouched.
- **`JointAttach`** now poses the attached entity's whole subtree, so the muzzle light under the
  rifle follows the hand.
- **The play scene** (`assets/scenes/blockout_play.scene.json`):
  - Each enemy root carries the model, `SkinnedMesh` and `Animator`.
  - Its `Body` and `Head` entities keep their ids with no mesh (ruling 18: rounds hit the capsule).
  - Its `Gun` is the rifle, with `JointAttach` on `Index1.R`.
  - Its `Muzzle` sits under the rifle at the barrel tip (0.173, 0.945, −0.039).
  - Spawns, health and weapons are unchanged.
- **Sounds:**
  - `make_weapons.py` generates `mag_out`, `mag_in`, `bolt` and `body_fall`; the existing 10 sounds
    rebuild byte for byte.
  - `CombatSounds` reads the animation log by its own cursor. Each sound plays where the entity is
    drawn (its chest, or the floor for a fall), on the near curve, with an occlusion ray for anyone
    but the player.
- **Benches:**
  - `render_bench play_anim_1080p`: the play scene stepped before every frame.
  - `physics_bench` section 11: `anim_step_ms` and `pose_sync_ms` on the play scene, gated from
    `animStep` 2.

## 3. Bugs found and fixed

- **A restart step stepped the restored Animators with events from before the restore.** The round's
  restart restores inside the combat step, before the animation step. Without care, a round fired
  earlier in that step would kick a restored Animator and break the restart's `world.hash`. The driver
  now reads only the events after the step's last `restart`. `t25_fight_states` checks this: both
  Animators and `world.hash` equal the round start's.
- **The upstream rest pose is not the bind pose.** The bind pose is a T-pose; the nodes' rest pose has
  the arms down. The script's first version assumed they were the same and refused the file. It now
  derives the inverse binds from the mesh node's transform, as the file's own inverse binds do.
- **A test crashed on the unchanged scene.** `t25_fight_states` called `get<Animator>` on box enemies
  before the scene had changed. It now fails cleanly instead. The crash also showed that piped stdout
  is fully buffered, so a crash appears to happen at the last flushed test name.
- **Test bug of mine:** the palette is 41 bones (the bones that weight vertices; the two IK targets
  weight none), not 43.

## 4. Spec corrections ("As built (step 2)" in the spec)

- **The driver takes its state as pointers.** A dead enemy has lost its character components. It also
  reads events after the step's last restart. Damage dealt between steps (`combat.damage`) flinches
  nothing; death still follows `Health`.
- **The reload clip is authored at the enemies' `reloadTime` (2 s).** The graph's `rateFrom` is not
  built.
- **The play scene's skin and refit budgets are gated on `play_anim_1080p`, not `play_hud_1080p`.**
  The `play_hud` capture never changes a pose, so the skin passes never run there.
- **A dead enemy's reported `pos` is 0.3 m lower.** `combat.info` gives its feet; the tip-over used to
  raise the body by the capsule radius.
- **No impact marks on skinned enemies.** Marks need a `BoxPrim`, and the body boxes no longer draw.

## 5. Results

- **Tests (RTX 5070 Laptop):**
  - `combat_tests` 60/60, including `t23_enemy_states_from_ai`, `t23_enemy_reload_events`,
    `t25_play_scene_skinned`, `t25_fight_states` (`t23_restart_resets`) and the death edits to test
    16 and the mark test;
  - `anim_tests` 18/18, `runtime_tests` 42/42 (`sp10_t23_handling_sounds`), `api_tests` 66/66,
    `level_physics_tests` 59/59, `scene_tests` 84/84, `reflect_tests` 42/42;
  - `check_serve` 236/236, `check_cli` 33/33, `check_docs` 121/121;
  - `check_validation.py` 14/14 with zero messages (now with the play scene at 60 steps in deferred,
    `rt_offline`, `rt_realtime` and NRD);
  - `check_live.py` 68/68: in the fight, EnemyA's death clip is held and `body_fall` fires once, and
    the window and headless fights match on `world.hash`, `combat.info`, events, `animation.info` and
    the HUD picture bit for bit;
  - `e2e_mcp.py` 55/55, with `animation_info` read mid-fight and a state forced through `world_apply`.
- **Event timing:** an empty magazine's reload gives `mag_out`, `mag_in` and `bolt` exactly 36, 78 and
  105 steps into the clip.
- **Expectation moves (each in its own commit):**
  - `player.jsonl`: `world.hash`; the snapshot grew 9452 → 9500 bytes; a restore now changes 19
    entities instead of 17 (the two Animators);
  - `combat.jsonl`: `world.hash`, and the dead enemies' `pos.y` (ruling 7);
  - the serve guide's player and fight examples changed the same way;
  - CLI expectations unchanged.
- **Goldens:**
  - `cli_blockout_play_deferred` (max_abs 192) and `cli_blockout_play_hud` (180) moved only where the
    enemies stand, in their shadows and in GI. They were looked at, re-baselined, and are exact on a
    second run.
  - The other 28 are exact, including the Task 1 rig goldens.
- **`tools/list` (OPEN ruling 13):** unchanged at 24,137 of 24,576 bytes; step 2 adds no tool.
- **Benchmarks** (first runs; both benches were noisy, with other processes at 287% of a core, so
  this is not a verdict):

| Measure | Bench | Median | p95 | Budget (proposal, OPEN 16) |
|---|---|---|---|---|
| `skin` + `skin.bounds`, the play scene's 2 enemies | `render_bench play_anim_1080p` | 0.003 ms | 0.004 | 0.05 ms |
| `rt.blas.refit`, the play scene | `render_bench play_anim_1080p` | 0.077 ms | 0.079 | 0.10 ms |
| `play_hud_1080p` frame (pre-SP10 2.190 ms) | `render_bench play_hud_1080p` | 2.353 ms (+0.163) | 2.443 | 2.39 ms (+0.20) |
| `animation` step, the play scene | `physics_bench anim_step_ms` | 0.0007 ms | 0.0022 | 0.01 ms |
| `pose` sync, the play scene | `physics_bench pose_sync_ms` | 0.0111 ms | 0.0261 | 0.02 ms |
| `animation` step, 16 rigs | `physics_bench anim16_step_ms` | 0.0019 ms | 0.0039 | 0.05 ms |
| `pose` sync, 16 rigs | `physics_bench pose16_sync_ms` | 0.0077 ms | 0.0186 | 0.10 ms |

  Every measure is inside its proposal; none is tightened (ruling 16 stands). The play-scene frame is
  0.163 ms over its pre-SP10 median, against the 0.20 ms allowed, and the pre-SP10 median itself came
  from a noisy run.
- **Look shots** (`C:\Users\djmax\ohao_refs\look_shots\sp10_anim\`):
  - `step2_enemy_strip.png`: idle, run, aim, fire, reload, hit and death, left to right. Each state is
    forced through a driver-less copy of the graph and rendered headless at alpha 1, which is the pose
    a window draws for that state; it is not a capture from a running window.
  - `step2_rt_enemy.png`: the play scene after 45 steps, deferred (RT shadows and GI from the enemies)
    beside the path tracer.
- **`check.py --all`:** 45 of 51 passed. Every functional suite passed, including the 30 goldens.
  The six failures are benches:
  - five were noisy after `check.py`'s two reruns. Other processes used 282–328% of a core, and the
    CPU calibration ran up to ×2.94 slower than its reference. They are `scene_bench`,
    `serve_bench`, `level_bench`, `physics_bench` and `render_bench`;
  - in that noisy run, `physics_bench` read `pose_sync_ms` at 0.0238 ms, over its 0.02 budget. The
    earlier run, at ×1.16, read 0.0111. The budget is not loosened (ruling 16), but it is the
    animation budget closest to its limit, and the one to watch on a quiet run;
  - `feel_bench`'s `frame_p99_over_median` was 2.28 against 1.5. This is the miss carried over from
    sub-project 9 (1.99 at Feel step 2, open ruling 21), not new in this step.

## 6. Rulings applied (provisional, recorded for Frank)

- Rulings 1–6 as provisional in the spec.
- **OPEN 7 (death), applied:** the clip falls and the Transform stays upright. This is the only
  combat expectation change: the lying-down assertions in test 16 and the mark test, and the dead
  `pos.y` in `combat.jsonl` and the serve guide.
- **OPEN 10:** `Fox.glb` and `BrainStem.glb` deleted.
- **OPEN 11 (ADS):** not built; it belongs to Task 3.
- **OPEN 13:** no change needed (24,137 bytes).
- **OPEN 16:** gated at the proposals, none tightened.
- **OPEN 17 (footsteps stay physics):** applied; no footstep events from clips.
- **OPEN 18:** rounds hit the capsule; the body and head entities draw nothing.
- **Choices made here, for Frank to confirm:**
  - the kit's `AK` instead of the separate `Rifle.glb`;
  - the 1.8 m fit;
  - the generated clips' poses: the aim pitch split across the spine, and the reload motion;
  - `play_anim_1080p` as the capture for the play scene's skinning budgets.

## 7. Known gaps

- **The flinch over the aim pose swings the rifle arm high.** HitReact's delta is added to a raised
  rifle. This is a quality call for the hand test; masking `hit` to the spine alone would soften it.
- **The generated reload and aim poses are approximate.** They are world-axis rotations at the rest
  pose. A Blender-authored set would be better; the Blender script was not possible in this session.
- **No impact marks on the skinned enemies** (ruling 18; they need a box).
- **Damage dealt between steps does not flinch** (it does kill).
- **`rateFrom: weapon.reloadTime` is not built;** the enemy reload clip is authored at 2 s.
- **The path tracer overexposes the play scene.** `--exposure` does not reach `rt_offline`. This was
  seen while making the look shot; it is not related to animation and was not investigated.
- **The strip is not from a realtime window** (see section 5).
- **Commit `667d21a9`** ("handling sounds") also holds the driver, graph, death ruling and scene. The
  harness refused the intended commit's message, and the next commit took the staged files. The empty
  commit after it, `75f7e2e0`, records what `667d21a9` contains; history was not rewritten.

## 8. How to run the new tests

```
build/Release/combat_tests.exe                 # t23_enemy_*, t25_*, the death edits
build/Release/runtime_tests.exe                # sp10_t23_handling_sounds
python tests/serve/check_live.py               # GPU; the fight reads animation.info
python tests/serve/e2e_mcp.py                  # GPU; animation_info, a forced state
python tests/validation/check_validation.py    # GPU; the play scene's four rows
build/Release/render_bench.exe --only play_anim_1080p --check tests/bench/render_budgets.json
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
python tools/anim/make_enemy.py --source <dir with Character_Enemy.glb>   # rebuilds byte for byte
python tools/audio/make_weapons.py             # the handling sounds
```
