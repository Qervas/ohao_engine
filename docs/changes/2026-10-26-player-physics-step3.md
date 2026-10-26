# Player and physics, step 3: the camera

**Spec:** `docs/specs/2026-10-26-player-physics-design.md` ("The camera (step 3)",
"Interpolation" under "Physics ↔ transform sync", and "As built (step 3)"); plan
`docs/plans/2026-10-26-player-physics.md`, Task 3.

**Status:** built and tested.
- `tools/check.py --all`: every functional suite passes, apart from two failures that are now
  fixed (below), and the 26 render goldens pass.
- `serve_bench` passed.
- Four benches (`scene_bench`, `level_bench`, `physics_bench`, `render_bench`) were flagged noisy:
  other processes used 0.25–2.2 cores and calibration ran x1.3–1.8. Every physics budget was
  met.
- The two functional failures were monograph citations of the render extract's old install line
  (`monograph_structure`, and `check_docs` through it). Both are fixed and rerun: 16 monograph
  tests pass, and AGENTS.md's commands pass 42/42.

## 1. Summary

The player sees. Its `Head` child holds the camera at eye height, pitched by the look within
±89°, while yaw turns the body. `assets/scenes/blockout_play.scene.json` loads with the view at
the player spawn from the first frame. `ohao_game` plays it with the mouse captured.

Realtime frames draw what the steps move (the player, its head, falling bodies) between their
last two step poses, so the view is smooth at any frame rate. What is saved and hashed stays
exactly the step's.

## 2. What was built

**The head** (`ecs/physics_scene.cpp`):
- It is the player's first child, in creation order, holding a `Camera`; failing that, its first
  child named `Head`.
- The step writes its local `Transform`: (0, `eyeHeight`, 0), pitched about +X. It writes only
  when that changes.
- A head rotation written by anything else sets the pitch, clamped, as an edited player rotation
  sets the yaw.
- The scene authors the head; the engine never creates it.

**Spawn in sync:**
- A `player_spawn` sync system, right before `transform`, creates the characters of new or
  changed `PlayerController`s. So the first frame of a loaded play scene is drawn from the spawn.
- The spawn's pose comes from the `Transform` chain, since a freshly expanded level has no
  `WorldTransform` yet.

**Render interpolation:**
- `World::setRenderAlpha` sets it; the default is 1.
- The physics step records each move as world poses before and after: players, heads and
  written-back dynamic bodies. A spawn, a teleport or an edit records nothing, so nothing is
  drawn across it.
- The `interpolate` sync system, after `physics` and before `render_extract`, gives each mover
  and its subtree a runtime `RenderPose{world, prevWorld}` at alpha. At alpha 1 every
  `RenderPose` is removed.
- The render extract and the scene view's camera use `RenderPose` when there is one.
- `installRenderExtract` places itself after `interpolate` when that exists, so the order does
  not depend on which is installed first.
- `LiveHost` sets the timeline's alpha in realtime. It sets 1 in lockstep, while paused and for
  lockstep captures, so a lockstep window still equals a headless capture.

**The view:** `resolveSceneView` takes the first *active* camera. A level's own camera is
inactive beside a player's head, by the level system's rule, and used to hide it.

**Capture:** `captureFrame` syncs before it resolves the view (section 3).

**Window and `ohao_game`:**
- `Window` reports look input only while the cursor is captured, as `MouseDelta`. Capturing or
  freeing the cursor is not a look. `Window::focused()` is new.
- `ohao_game` on a scene with a `PlayerController` is play mode:
  - the mouse is captured while the window has focus;
  - the scene's `release_mouse` (Esc) frees it and `capture_mouse` (left click) takes it back;
  - closing the window quits.
- A fly scene behaves as before.

**The play scene:** `blockout_play.scene.json` holds:
- the blockout's level, sun, lamp and sky;
- `Player` (`PlayerController` with its defaults);
- `Head` (`Camera` fov 75°, near 0.05 m, at 1.62 m);
- `Controls` (WASD, Space jump, LeftShift sprint, Esc, left click).

**Tests:** `level_physics_tests` test 12, five new:
- `t12_head_at_eye_height`;
- `t12_pitch_clamps_at_89`, which also checks that an edited head sets the pitch;
- `t12_yaw_turns_the_body`;
- `t12_interpolation_leaves_hash_and_save`: the head and a falling crate are drawn halfway at
  alpha 0.5, while `WorldTransform`, `world.hash` and the saved text are unchanged and nothing is
  left at alpha 1;
- `t12_play_scene_loads_with_head_camera`.

**Golden:** `cli_blockout_play_deferred`, the play scene through the head before any step
(26 render goldens).

**Docs:**
- the spec's "As built (step 3)" and the plan's Task 3, ticked;
- AGENTS.md: the player's camera, interpolation, the window's mouse, test 12, the golden count;
- ARCHITECTURE's sync order (`player_spawn`, `interpolate`) and the view;
- the CLI guide's `Camera` and `PlayerController` rows;
- the serve guide's "Walk the level", with `docs/agents/examples/walk.jsonl`;
- the monograph's two citations of the render extract's install.

## 3. Bugs found and fixed

- **The first captured frame of the play scene looked from the origin.** `captureFrame`
  resolved the view before its first `world.sync()`. Until this step the view never depended on
  sync; now the player spawns there. Found by looking at the first look shot, whose view filled
  with the pillar's blue-grey. `captureFrame` now syncs first; every other golden is unchanged.
- **The level's own camera hid the head's.** It was the first camera in creation order and
  inactive, so the view had no camera at all. Found by `t12_head_at_eye_height`. Fixed by taking
  the first active camera.
- **The play-scene test expected one `Camera`.** The level brings its own camera (inactive), so
  the test now requires exactly one active camera.

## 4. Spec corrections and rulings

All of these are in the spec's "As built (step 3)":
- **The head is authored, not created.** A runtime-made entity would be saved and hashed. This
  was the conservative choice.
- **Spawning moved into a sync system**, so the first frame shows the spawn.
- **Interpolation is a runtime component written by a sync system** rather than render-only
  matrices inside the renderer. The renderer is untouched; only the extract reads `RenderPose`.
  Lights are not interpolated.
- **Look latency.** Interpolation draws up to one step (16.7 ms) behind the simulation, the cost
  of smooth motion at any frame rate. Head pitch is interpolated with the head.
- **Esc in play mode frees the mouse** and no longer quits; the window's close button quits. The
  spec says "Esc releases it" and says nothing about quitting.
- **The window's look input changed for every host.** Only a captured cursor turns anything. The
  fly camera captures while `look` is held, so it behaves as before, except that the first frame
  of a right-button hold is not yet captured.

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | functional suites green after the citation fix; `render_golden` 26/26; `serve_bench` 16/16; `scene_bench`, `level_bench`, `physics_bench`, `render_bench` noisy |
| `level_physics_tests` | 39/39 (5 new) |
| `ecs_tests` / `scene_tests` / `reflect_tests` / `api_tests` | 37/37 / 84/84 / 42/42 / 55/55 |
| `engine_tests` / `runtime_tests` / `systems_tests` | 85/85 / 30/30 / 9/9 |
| `check_cli` / `check_serve` / `e2e_mcp` / `check_live` | 33/33 / 198/198 / 33/33 / 42/42 |
| `check_validation` / `ohao_game` presentation gate | 6/6 / ok |
| `check_docs` | 82 commands, all passing after the fix |
| `ohao_game blockout_play --frames 300`, validation on | 0 messages through teardown, 0 unrequested readbacks, 408 fps |

**The camera:**
- The head sits at (0, 1.62, 0) on the player. The view is at the spawn (1.5000, 1.6200, 2.2000),
  facing −Z (pitch 0, yaw −90), fov 75, near 0.05.
- Pitch stops at −89.000 and +89.000.
- 90° of look right gives a body facing (1.0000, 0, 0), and walking forward then goes +X.
- Interpolation at alpha 0.5 draws the eye at z 1.9000, halfway between the steps' 1.9333 and
  1.8667, within 1e-4.

**Benchmarks** (`physics_bench`, in the gate, noisy: other processes 2.2 cores, calibration
x1.78). Every budget was met:

| Budget | Median | p95 | Limit |
|---|---|---|---|
| `player_step_ms` | 0.0058 ms | 0.0206 ms | 0.1 |
| `blockout_step_ms` | 0.0179 ms | 0.0659 ms | 0.25 |
| `blockout_rest_step_ms` | 0.0034 ms | 0.0034 ms | 0.02 |
| `events_1k_ms` | 0.468 ms | 0.600 ms | 0.5 |

The other noisy benches are pending a quiet rerun:
- `scene_bench`'s clean rounds: `save_pretty` 11.248 ms against 10, `apply_100_field_set`
  0.299 ms against 0.2.
- `level_bench`: `blockout_load_ms` median 20.966 ms against 20.
- `render_bench`: `cornell_720p lighting` 0.372 ms against 0.32 (the standing miss).

This step changes no scene I/O or renderer code those budgets time, apart from extract reading
`RenderPose`, which is a `try_get` per uploaded instance. Step 2's quiet-ish run met
`save_pretty` at 9.208 ms.

**Golden added, looked at:** `cli_blockout_play_deferred` shows the spawn's view at eye height,
facing north into the room: the Step and Platform on the left, the pillar, CrateA and CrateB, and
the ramp on the right. No other golden changed.

**Look shots** are in `C:\Users\djmax\ohao_refs\look_shots\sp6_player\`. They are first-person
renders through the player's head, from real `ohao run`s of the play scene with injected input,
deferred at 1280x720:

| File | Step | Shows |
|---|---|---|
| `step3_eye_000.png` | 0 | the spawn's view |
| `step3_eye_018.png` | 18 | turned to the doorway |
| `step3_eye_026.png` | 26 | at its threshold, the sky and plain beyond |
| `step3_eye_045.png` | 45 | turned back |
| `step3_eye_062.png` | 62 | toward the ramp |
| `step3_eye_080.png` | 80 | at its foot |
| `step3_eye_100.png` | 100 | up it, y 0.16 |
| `step3_eye_125.png` | 125 | near the top, y 0.48, facing the north wall |
| `step3_eye_140.png` | 140 | turned left over the room from the top, y 0.55 |
| `step3_eye_155.png` | 155 | looking down 24° |
| `step3_eye_strip.png` | — | the ten shots |

## 6. Known gaps

- **Fireflies on the doorway's sunlit jamb and lintel** in the deferred frame. They are bright
  dots on the faces whose rays leave through the doorway, visible in `step3_eye_018` and
  `step3_eye_026`, and still there with every post effect off.
  - Most likely RT GI's rays meeting the outdoor HDRI's sun unclamped. Not investigated: it is
    renderer work, not this step's.
  - The step-3 golden does not see the doorway.
- **No floor outside the doorway** in the blockout: a player walking out falls. The level is
  what it is.
- **The player's own body is not drawn** (first person: no mesh). Footsteps are step 4.
- **Interpolation covers what physics moves.** A spinning `Spin` or a fly camera still moves once
  per step.
- **Snapshots do not hold velocities.** A restore puts the player at rest (step 5).

## 7. How to run

```
build/Release/level_physics_tests.exe
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_blockout_play_deferred
build/Release/ohao.exe run assets/scenes/blockout_play.scene.json --script docs/agents/examples/walk.jsonl
build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
```
