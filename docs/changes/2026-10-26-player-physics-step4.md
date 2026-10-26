# Player and physics, step 4: footsteps

**Spec:** `docs/specs/2026-10-26-player-physics-design.md` ("Footsteps (step 4)" and "As built
(step 4)"); plan `docs/plans/2026-10-26-player-physics.md`, Task 4. The level pipeline spec's
hint table gains `trig` and `surf`.

**Status:** built and tested.
- `tools/check.py --all`: every functional suite passes, apart from expectation updates made
  after the run (below), and the 26 render goldens are unchanged.
- The five benches were flagged noisy: other processes used 0.3–9 cores and calibration ran up
  to x1.43. Every physics budget was met.
- The run's functional failures were the schema hash in the CLI and serve expectations, one
  `world.hash` quoted in the serve guide and one monograph citation. All are updated and rerun:
  `check_cli` 33/33, `check_serve` 198/198, `check_docs` 115/115, monograph 16 OK.

## 1. Summary

The player's steps are heard and logged.
- **Footsteps.** Every 0.75 m walked (0.95 m sprinting) on the ground is a `footstep` event.
- **Landings.** A fall of more than 0.3 m ends in a `land`.
- **Surface.** Each event carries the ground's surface: `Collider.surface`, set in levels by the
  hint `surf`. The blockout now says concrete, metal and wood.
- **Agents** read the events in `physics.events`, beside contacts and triggers.
- **People** hear them in `ohao_game`: our own generated sounds, or Somnabulate's staged into
  git-ignored `assets/local/` for local listening (Frank's ruling).

## 2. What was built

**Surface:**
- `Collider.surface` is a saved string of at most 32 characters, `omitDefault` (empty means
  `default`), so files and hashes without it keep their bytes.
- The level hint `surf` (1–32 characters) sets it on the node's collider, and
  `ohao schema level-extras` lists it.
- `tools/level/blockout_room.py` gives the Floor `concrete`; the Step, Platform, Ramp and Pillar
  `metal`; the crates `wood`. `assets/levels/blockout_room.glb` is rebuilt with Blender 5.2.2.
  Only the extras changed: 16,788 to 16,896 bytes, and `check_level_assets`' byte-for-byte
  rebuild passes.

**Events:**
- `PhysicsEvent::Kind::Footstep` and `Land` join the physics events and are sorted after them
  within a step.
  - `a` is the player and `b` the ground's entity.
  - `point` is the feet, `normal` the ground's.
  - `speed` is the horizontal speed, or for a landing the downward speed.
  - `surface` is the ground body's `Collider.surface`, or `default`.
- They are emitted by the player's step in `PhysicsScene`:
  - The stride accumulates the ground distance actually covered (into a wall: none).
  - Touching down starts a new stride.
  - A landing compares the highest point since leaving the ground against 0.3 m.
  - A teleport restarts both.
- `physics.events` writes `surface` for them, and kinds `footstep` and `land`.

**Sound:**
- `runtime::FootstepSounds` picks a file: `<surface>_<n>.wav`, the first folder that holds a
  surface supplies all of its sounds, round robin in number order, else `default`'s.
- `runtime::Footsteps` reads the log since its last call once per frame in `LiveHost` and plays
  through `AudioSystem` at the feet, heard from the camera.
  - Volume: 0.25 + 0.1 × speed for footsteps, speed / 6 within [0.4, 1] for landings.
  - It runs only with `LiveConfig::footsteps`: on in `ohao_game` (`--mute` turns it off), off in
    `ohao serve --window`.
- Folders: `assets/local/audio/footsteps` (git-ignored), then `assets/audio/footsteps`.
  `ohao_runtime` now links `ohao_audio`.

**Sounds:**
- `assets/audio/footsteps/{default,concrete,metal,wood}_{1..4}.wav`: 16 files, mono 16-bit
  44.1 kHz, 0.3 s.
- Synthesized by `tools/audio/make_footsteps.py` with a fixed seed: a heel and a toe strike of
  filtered noise, with a low thump (concrete), ringing partials (metal), a hollow knock (wood) or
  a dull pat (default). They are ours (MIT).
- **Frank's ruling:** `tools/audio/stage_somnabulate_footsteps.py` copies Somnabulate's wood and
  tile walks, read only from its hub audio folder, into `assets/local/audio/footsteps` as `wood`
  and `concrete`, 12 files. That folder is git-ignored and documented in
  `assets/local/README.md`; nothing is committed. With them staged, `ohao_game` finds 20
  sounds: 12 local, plus our `default` and `metal`.

**Tests:**
- `level_physics_tests` test 13, five new:
  - walk strides 0.75 m;
  - sprint strides 0.95 m;
  - the surface from the ground body: the blockout's hints, an edited `Collider.surface`, and
    `default`;
  - a land after a 0.5 m drop, none after 0.2 m, one after a jump;
  - none standing still or in the air.
- `runtime_tests`, two new:
  - which sound plays: round robin, the local folder first, the fallback, other files ignored;
  - `Footsteps` plays every logged footstep exactly once across frames, at volume 0, skipping
    without an audio device.
- `api_tests`: `sp6_t13_footsteps_in_physics_events`.
- `scene_tests`:
  - the `surf` hint read and refused;
  - `Collider.surface`'s 32-character limit;
  - the blockout's CrateA extras;
  - a random `Collider.surface` in the round-trip world.

## 3. Bugs found and fixed

- **My own test:** the surface test first started 0.7 m from the Step, so its first stride
  completed on metal and the test found no floor footstep. It now starts with 2 m of Floor
  before the Step.
- **The look shot's timeline** was offset by the first event's step. Event steps already count
  from the play scene's first `world.step`; fixed in the plotting script.

## 4. Spec corrections and rulings

These are in the spec's "As built (step 4)":
- **Surface default.** `default` is the empty string, reported as `"default"`, not a stored
  `"default"`. With `omitDefault`, every existing file and hash stays as it was.
- **Where the events live.** Footsteps and landings are `PhysicsEvent` kinds in the physics log,
  not a separate player-event stream; the spec's "same log" holds.
- **Sounds.** The spec's "never Somnabulate's" is narrowed by Frank's ruling: allowed in
  `assets/local/` for local listening, never committed. The committed set is generated.
- **Silent windows.** A server's window (`ohao serve --window`) is silent by default, so agents'
  hidden windows make no sound. This was the conservative choice.

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | functional suites green after the expectation updates; `render_golden` 26/26 unchanged; benches noisy |
| `level_physics_tests` | 44/44 (5 new) |
| `runtime_tests` | 32/32 (2 new) |
| `api_tests` | 56/56 (1 new) |
| `scene_tests` | 84/84 |
| `ecs_tests` / `engine_tests` / `audio_system_tests` | 37/37 / 85/85 / 13/13 |
| `check_cli` / `check_serve` / `e2e_mcp` / `check_live` | 33/33 / 198/198 / 33/33 / 42/42 |
| `check_level_assets` / `check_docs` | 4/4 (Blender rebuild byte for byte) / 115/115 |
| `ohao_game blockout_play --frames 300`, validation on | 0 messages through teardown, 0 unrequested readbacks, 20 footstep sounds found |

**Footsteps** (`level_physics_tests`, blockout, 1/60 s steps):

| Case | Result |
|---|---|
| Walking 4.833 m | 6 footsteps, 0.733–0.800 m apart, on concrete, at 4.0 m/s |
| Sprinting 4.400 m | 4 footsteps, 0.867–0.975 m apart, at 6.5 m/s |
| West across the room | `Floor:concrete` ×2, then `Step:metal` ×2 and `Platform:metal` |
| A 0.5 m drop | one land at 3.188 m/s (free fall 3.132); a 0.2 m drop: none |

**The look-shot walk** (`ohao run` of `blockout_play.scene.json`, 150 steps):
- 2 footsteps on concrete, then 5 on the Ramp's metal.
- A land at step 108, at 3.19 m/s on concrete, after sprinting off the ramp's side (0.51 m).
- Then 3 sprint footsteps on the Step and Platform.

**Benchmarks** (`physics_bench`, gate run: noisy, other processes 5.3 cores, calibration x1.30).
All met:

| Budget | Median | p95 | Limit |
|---|---|---|---|
| `player_step_ms` | 0.0043 ms | 0.0099 ms | 0.1 |
| `blockout_step_ms` | 0.0193 ms | 0.0475 ms | 0.25 |
| `blockout_rest_step_ms` | 0.0019 ms | 0.002 ms | 0.02 |
| `events_1k_ms` | 0.433 ms | 0.923 ms | 0.5 |

Two standalone reruns of `events_1k_ms` ran with the machine at calibration x1.94 and x2.00:
0.5276 ms (over) and 0.3242 ms. Both are noisy, so not a verdict. The events now carry a
`surface` string, empty and inline for contacts. A quiet rerun is needed to compare with
step 2's 0.21 ms.

The other benches, noisy:
- `scene_bench`: `save_pretty` 8.935 ms (budget 10), `apply_100_field_set` 0.174 ms (0.2),
  both within budget.
- `serve_bench`: `world_apply_100_rpc` 0.1027 ms against 0.4.
- `level_bench`: `blockout_load_ms` 13.96 ms against 20.
- `render_bench`: Cornell lighting 0.409 ms against 0.32 (the standing miss).

**Goldens, each diff looked at:**
- **The catalogue and scene schema:** `Collider.surface` with its doc, 32-character limit and
  `omitDefault`.
- **`tests/level/extras_schema.json`:** `surf`.
- **The CLI and serve expectations:** schema hash `36655761f296b8a0` → `94d642166fb6b4e2`.
- **`world.hash` of the level sessions and the serve guide's level example:** the blockout's
  nodes now carry surfaces, which are hashed.

No render golden changed: the level's meshes are the same.

**Look shot:** `C:\Users\djmax\ohao_refs\look_shots\sp6_player\step4_footsteps.png`.
- **The map:** the blockout from above, with the walk's path, each footstep coloured by surface
  and the landing marked.
- **The timeline:** the feet's height against the step, a line per footstep (concrete grey,
  metal blue) and the landing in red at the moment the feet reach the floor.
- **The sounds:** waveforms of the four committed sounds.

## 6. Known gaps

- **Unverified by ear.** Footstep sound in a real window was checked only through the
  `runtime_tests` player at volume 0 and the window's sound count. Nobody has listened, so the
  sounds' character is unjudged.
- **One sound per landing.** Landings use the surface's footstep sounds, louder; there are no
  separate landing sounds.
- **No surface on walls.** The blockout's walls carry none; they are never walked on.
- **Ground body at an edge.** A footstep's ground body is the character's; near an edge the
  capsule can still stand on a body its centre has left (the step-86 footstep at x 1.83 on the
  Ramp).
- **`surf` without a collider** is silently unused.

## 7. How to run

```
build/Release/level_physics_tests.exe
build/Release/runtime_tests.exe
python tools/audio/make_footsteps.py
python tools/audio/stage_somnabulate_footsteps.py
build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
```
