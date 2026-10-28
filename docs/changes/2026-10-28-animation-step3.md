# Sub-project 10, step 3: the player's arms and rifle

**Spec:** `docs/specs/2026-10-28-animation-design.md` ("The frame", "The step", "Test plan" tests 23
and 25, OPEN rulings 8 and 11, and "As built (step 3)")
**Plan:** `docs/plans/2026-10-28-animation.md`, Task 3 (steps 1–6 ticked)
**Status:** built; Task 3 of 4. Roadmap row 10 stays open; row 9 stays "built, waiting for Frank".

## 1. Summary

The play scene's box gun is gone. The player now sees skinned arms holding a rifle. They breathe
at idle, kick on each round, roll the rifle to swap the magazine on a reload, carry it across the
body while sprinting, and raise the sights to the eye while aiming. The reload clip stretches to
the weapon's `reloadTime`, so the magazine goes home within one step of the weapon's `reloaded`.

Aiming down the sights is new gameplay (OPEN ruling 11, provisional): holding the right mouse
button narrows the spread cone to half and the drawn view from 75° to 55°. The procedural view kick
plays at half amplitude on top of the arms' fire clip (OPEN ruling 8, provisional).

Only the two play-scene goldens moved; the other 28 are exact. The `world.hash` expectations moved
for the three new saved fields and the play scene's one new entity; nothing else.

## 2. What was built

- **Content** (`assets/characters/fps_arms/`). No CC0 first-person arms rig was available (no
  network, nothing staged), so the plan's fallback applied.
  - `tools/anim/make_fps_arms.py` (ours, MIT) builds the file in Blender 5.2.2 LTS (d13f752e3b9c),
    in background mode, from an empty factory scene. Blender worked; no pure-Python substitute was
    used.
  - The rifle is the script's own blockout, not the enemies' Quaternius AK: the AK is in the enemy's
    hand-joint frame and has no separate magazine to pull.
  - One mesh `Arms` (456 vertices, 4 flat materials) sits on a 5-joint skin `ArmsRig`: `root`,
    `gun` (the rifle and right arm, rigid), `mag`, `upper_L`, `fore_L`. Every vertex is weighted to
    one joint.
  - Clips (60 fps, linear): `rest`, `idle` (2 s), `fire` (0.15 s), `reload` (2 s; magazine out at
    0.6 s, home at 1.5 s), `ads_in` (0.2 s), `ads_hold`, `sprint` (0.6 s loop).
  - Deterministic: two runs wrote the same 34,680 bytes for the first layout, and after the hold was
    re-placed (below) the checked-in file again matched its rebuild. `check_level_assets.py` gains
    `a3_fps_arms_rebuild_byte_identical` (skips without Blender 5.2.2).
  - `LICENSE.md` (MIT, generated) and `README.upstream.md` (generator, Blender version, rebuild
    command) are beside it; `.gitattributes` keeps `assets/characters/**` byte for byte.
- **`rateFrom`** (`anim/anim_graph`): `{param, at}` plays a clip so its event `at` (else its end) lands
  `param` seconds into the state. A parameter at 0 plays the clip at its own rate. Unknown or
  non-float parameters, a missing event, and `rate` with `rateFrom` are errors with their paths.
  `animation.info` times such clips by the entity's last parameters.
- **The arms graph** (`assets/anim/arms.animgraph.json`, driver `arms`):
  - body `ready` (a 1-D blend of `idle` and `ads_hold` by `aim`), `sprint`, `reload` (`rateFrom
    {reloadTime, mag_in}`);
  - additive `fire` (`kick` per `shot`).
  - `ads_in` ships unused: the blend by the stepped `aim` is the transition, so the arms reach the
    sights exactly when the cone tightens.
- **The driver** (`ecs/arms_anim.cpp`, `driveArmsAnim`):
  - It writes `moving`, `sprinting`, `aim`, `alive`, `reloadTime` and the events `shot` and
    `reload`, from the saved state of the arms' owner (the nearest ancestor with a
    `PlayerController` and a `Weapon`).
  - It reads the step's combat events after the step's last `restart`, as the enemy driver does.
  - `sprinting` comes from the saved velocity, not from input, so a restore drives the same states.
- **ADS** (provisional ruling 11; `components.hpp`, `combat.cpp`, `view_model.cpp`, `scene_view.cpp`):
  - New saved fields: `Weapon.adsTime` (0.2 s), `Weapon.aim` (the action, `"aim"`) and
    `WeaponState.aim`.
  - A living input player holding `aim` while not reloading steps `aim` toward 1 at
    `dt / adsTime`; otherwise it steps toward 0. It snaps to the end within 1e-5 and is updated
    before the step's round.
  - The cone is `(spreadDeg + bloomDeg) × (1 − 0.5 · aim)`.
    - **The interpolation is linear.** Partial aim gives partial tightening, so the 0.2 s ramp is
      not a dead zone and there is no cliff on its last step.
    - At 1 it is exactly half, as the ruling says. At 0 it is the old cone bit for bit, and enemies
      never aim.
  - The drawn field of view:
    - `view_model` follows `WeaponState.aim` at the weapon's own rate. The drawn aim never runs
      ahead of the step and never overshoots.
    - In live frames only, it writes a runtime `DrawnFov` on the view's camera, from
      `Camera.fovDeg` toward 55° by that drawn aim. `resolveSceneView` prefers it, and the arms are
      remapped through it.
    - Lockstep frames and every capture keep 75°. A camera no gun narrowed this frame loses its
      `DrawnFov`.
- **The kick** (provisional ruling 8): data, not code. The play scene's `ViewModel` has `kickBack`
  0.015 m and `kickPitchDeg` 1.25°, half the defaults, on top of the arms' fire clip.
- **The play scene** (`blockout_play.scene.json`):
  - `Gun` is now the `ViewModel` root at the eye, and its child `Arms` is skinned and animated.
  - The `Muzzle` light is a `JointAttach` on `gun` at the barrel's tip, (0, 0.40, 0.018) in the
    joint's frame.
  - New input binding: `aim` on `MouseRight`.
  - `view_model` no longer draws `JointAttach` children; the pose system draws them after it.
- **Sounds**: the arms' `mag_out` and `mag_in` play as the player's own (at the ear, no occlusion
  ray). `animSoundOwner` maps an animated entity below the player to the player.

## 3. Bugs found and fixed

- **The first hold was off screen.** The first layout put the grip 32° below the eye line. A view
  model is drawn as if through its 60° lens and pulled to 0.41 of its depth, so the hands fell
  below the screen and the forearms inside the near-plane clip. The play-scene capture showed a
  rifle and a sliver of sleeve.
  - Fix: the hold was re-placed. The grip is now about 20° below and right of the eye line. The
    stock is shortened (so ADS never shows it cut by the clip). The barrel is shortened (the muzzle
    sits 0.31 m from the eye once remapped, against the 0.3 m capsule radius). The gloves are
    lighter.
  - `light_emitter_gpu_test`'s new arms case measures it: 7,484 pixels drawn, none outside the
    footprint.
- **The arms' handling sounds would have played as an enemy's**, with an occlusion ray, 1.1 m above
  the arms, because the arms are not the player entity. `sp10_t23_arms_sounds_are_the_players`
  covers the fix.

## 4. Spec corrections (in "As built (step 3)")

- `rateFrom` is `{param, at}`, not the string `"weapon.reloadTime"`. The driver supplies the
  parameter.
- `driveArmsAnim` takes the owner's state as pointers and no `InputMap`; sprint comes from the
  velocity.
- ADS also drops while reloading, so the arms leave the sights for the reload clip.

## 5. Results

- **`python tools/check.py --all`**: 51 suites; 44 passed and 7 failed.
  - `check_docs` failed on four transcripts in `docs/agents/ohao-serve.md`. The play scene loads 20
    entities, and the seeded walk's snapshot and `world.hash` moved. They were updated from the
    real runs; `check_docs.py` (GPU commands included) then passed 121/121.
  - The six benches were noisy: other processes used 3.3–4.4 cores, mostly other bots' Python.
    `check.py` retried them, and one more run of the bench label was also noisy throughout. Per
    the rules, noisy runs are reported, not gated, and no budget was touched (ruling 16).
  - Every functional suite passed.
- **The play scene's animation budgets** (`physics_bench`, noisy run; the scene now has three
  animators, the arms included):

  | Budget | Median | p95 | Budget value | Verdict |
  |---|---|---|---|---|
  | `anim_step_ms` | 0.0022 ms | 0.0057 ms | 0.01 ms | pass |
  | `pose_sync_ms` | 0.0271 ms (min 0.0118) | 0.0389 ms | 0.02 ms | over |

  `pose_sync_ms` was already 0.0238 noisy in step 2; whether the arms push it over on a quiet
  machine is for Task 4's quiet rerun.

- **Reload timing** (`t23_reload_mag_in_on_reloaded`): `mag_in` lands within one step of the
  weapon's `reloaded`, one step early in both cases (the reload state is entered in the reload's
  own step).

  | `reloadTime` | `mag_out` | `mag_in` | `reloaded` |
  |---|---|---|---|
  | 1.5 s | step 35 | step 89 | step 90 |
  | 2.0 s | step 47 | step 119 | step 120 |

  Steps are counted from the reload's start.
- **ADS** (`t23_ads_*`):
  - `aim` is 0.083333 after one step held, exactly 1 after 12 steps (0.2 s), and back to 0 twelve
    steps after release. It stays 0 for the dead.
  - The cone measured 2.0000°, 1.5000° and 1.0000° at aim 0, 0.5 and 1 (spread 2°, no bloom).
  - The drawn view measured 55.000° after 0.5 s of live frames aimed. Lockstep frames read 75°.
- **New tests**:
  - `anim_tests` `t23_rate_from_param`;
  - `combat_tests` `t23_reload_mag_in_on_reloaded`, `t23_ads_aim_steps_over_ads_time`,
    `t23_ads_halves_spread`, `t23_ads_fov_live_only`, and the arms case of `t25_play_scene_skinned`;
  - `runtime_tests` `sp10_t23_arms_sounds_are_the_players`;
  - `light_emitter_gpu_test` `view_model_arms_change_only_their_pixels`;
  - `check_level_assets` `a3_fps_arms_rebuild_byte_identical`.
- **Suites run by hand**:

  | Suite | Result |
  |---|---|
  | `combat_tests` | 64/64 |
  | `anim_tests` | 19/19 |
  | `runtime_tests` | 43/43 |
  | `light_emitter_gpu_test` | 33/33 |
  | `check_level_assets` | 5/5 |
  | `check_live.py` | 68/68 |
  | `e2e_mcp.py` | 55/55 |
  | `check_validation.py` | 14/14 (the play scene clean in deferred, `rt_offline`, `rt_realtime` and NRD) |
  | `check_serve.py` | 236/236 |

- **Latency and pacing** (`feel_bench --check --seconds 10`, 240 Hz, present wait): both runs were
  **noisy**. Other processes used 3.3 and 4.35 cores, mostly other bots' Python, Grok Bot and
  WebView2. Per SP9's rules a noisy run is neither a pass nor a fail; it is reported, not loosened.

  | Present mode | Look latency median (run 1 / run 2) | p99/median (run 1 / run 2) |
  |---|---|---|
  | immediate | 6.70 / 6.60 ms (budget 15) | 2.47 / 2.50 |
  | mailbox | 8.72 / 8.82 ms | 2.27 / 3.53 |
  | fifo | 9.14 / 9.47 ms | 1.89 / 2.05 |

  The pacing budget is 1.5: SP9 ruling 21's known miss, which still awaits Frank. No stalls over
  100 ms.
- **`tools/list`** (OPEN ruling 13): 24,137 bytes before and after (36 tools; no tool changed).
  No trim, no new cap.
- **Goldens**:
  - `cli_blockout_play_deferred` moved by max_abs 141 on 18,307 px, and `cli_blockout_play_hud` by
    max_abs 139 on 18,078 px. Both changes are inside the gun's region (the box replaced by the arms
    and rifle), plus 1 LSB on pixels beside it (the screen-space filters). Re-baselined after looking,
    in their own commit.
  - The other 28 goldens are exact, or at the path tracer's floor.
- **Expectations moved** (diffed before `--update`; nothing else changed):
  - `schemaHash` `ced24ee473d57f01` → `613adb2c6a96d4fb` in every serve session and CLI case. The
    cause is the three new saved fields; the catalogue and schema goldens add exactly those three.
  - The play scene's sessions load one more entity, `Arms`:
    - `combat`: 19 → 20 loaded, 36 → 37 with the level's nodes; `world.hash` `a885b5a2995b52ed` →
      `a776967d6450c6c7`.
    - `player`: the same counts; `world.diff` changed 19 → 20; the spawn snapshot 9,500 → 9,924
      bytes; `world.hash` `e2ed55b372c9f547` → `834bb4eb9bd5b330`.
- **Look shots** (`C:\Users\djmax\ohao_refs\look_shots\sp10_anim\`). Both are headless, not window
  captures.
  - `step3_arms_strip.png`: the play scene rendered by `ohao run` (deferred, 10 frames, 480×270)
    with the arms forced into each clip through a look-only graph: idle, fire (3 steps in), reload
    (magazine out at step 50, home at step 85), ADS (`ads_hold`) and sprint. The ADS panel shows the
    arms' pose only: headless frames keep 75°, so the narrowed view is not in it.
  - `step3_reload_timing.png`: the measured steps above as a timeline.

## 6. Known gaps and open items for Frank

- **Ruling 11 (ADS), provisional:** the 55° view, the 0.2 s `adsTime`, the linear cone, and ADS
  dropping during a reload are proposals for the hand test.
- **Ruling 8 (kick), provisional:** half amplitude, to tune by hand.
- **The arms are a Blender blockout**, placed by eye for the view model's remap. The parts that come
  from behind the eye are cut by the near plane. A real rig wants its own near plane or depth range
  for view models.
- **The muzzle can reach 1 cm past the 0.3 m capsule radius** when the player stands square against
  a wall; the box gun reached 0.2 m.
- `feel_bench` was noisy twice, and every bench was noisy in the gate. A quiet rerun belongs to
  Task 4's gate, `pose_sync_ms` first (above). Pacing stays SP9's open miss.
- The monograph's combat unit does not yet describe ADS; the animation unit is Task 4's.

## 7. How to run the new tests

```
build/Release/anim_tests.exe
build/Release/combat_tests.exe
build/Release/runtime_tests.exe
build/Release/light_emitter_gpu_test.exe --keep
python tests/level/check_level_assets.py
C:/Users/djmax/scoop/shims/blender.exe -b --factory-startup --python tools/anim/make_fps_arms.py -- assets/characters/fps_arms/fps_arms.glb
```
