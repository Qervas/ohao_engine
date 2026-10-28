# Audio: Frank's mix changes from the hand test

**Spec:** `docs/specs/2026-10-28-audio-design.md`, rulings 22 and 23; "Hand-test acceptance"
(the note on placing a sound).
**Status:** built. The hand test U1–U10 stays open for Frank; ROADMAP row 11 stays "built,
waiting for Frank".

## 1. Summary

Frank played the outdoor play scene on 2026-10-28 and asked for three changes. The player's
got-hit sound covered the enemy's shot and is gone. Enemy fire sounded too short; the root cause
was that masking, and the committed clip is also longer now. A test grenade on G gives a sound
he can place for the direction, distance, cover and room tests, because an enemy stops firing
once he is behind a wall.

## 2. What was built

- **Ruling 22, no player hurt sound** (`2eb2b1ec`).
  - `combatSoundNames` names no sound for `Damage`.
  - The bank's `hurt` sound and its `world` duck (−4 dB) are gone.
  - Removed with it: `assets/audio/weapons/hurt.wav`, its generator in `make_weapons.py`, and
    its slot in `audio_bench --listen`.
  - What stays: the impacts of the player's rounds, the enemies' `death` and `body_fall`, and
    the HUD's red vignette.
- **A longer enemy shot** (`0fbe549c`).
  - `enemy_fire` is now `distant_shot(13)`: a 1.2 s clip (a dark crack, body and thump, a slap
    at 45 ms, a low roll with a 0.32 s decay).
  - `fire.wav` is unchanged, and `holdMs` stays 600.
- **Ruling 23, the test grenade** (`332bb7cc`).
  - **Throw:** `grenade` is bound to G in `blockout_play.scene.json`. It throws from 0.4 m
    ahead of the eye along the look, at 9 m/s plus 3 m/s straight up under −9.81 m/s², with a
    1 s cooldown (`combat::kGrenade*`).
  - **Flight:** it flies in the combat step, after the weapons and before the round. It sticks
    5 cm off the first thing a ray along its move meets; it does not bounce.
  - **Fuse:** 2.5 s after the throw (150 steps) it goes off, writing an `explode` combat event at
    its place. `combat.events` reports it with its `point`. It does no damage and has no HUD or
    visual effect.
  - **Sound:** the schedule plays `explode`:
    - bus `weapons`, priority 230;
    - shape `blast`, 2–80 m, rolloff 1;
    - send 0.4, occluded.

    It plays with no owner, so walls cut it even though the player threw it.
  - **Clip:** ours, `explode()` in `make_weapons.py`: 2.2 s, rebuilt byte for byte.
  - **State:** the grenade in flight is runtime state of the combat system. It is never saved,
    hashed or snapshotted, and a restart clears it.
- **Docs:** G is documented in the `ohao_game` header, `docs/agents/ohao-serve.md`, AGENTS.md
  (Combat, and the `audio_schedule_tests` row), and the spec's hand-test section.

## 3. Bugs found and fixed

- **The masking**, found in step 4's listen schedule. All 22 `enemy_fire` plays landed in the
  same step as a `hurt` at the ear (0.85), and 20 of them also under the −4 dB `world` duck.
  Enemy rounds are hitscan, so the hurt always arrived with the shot it answered.
- **The grenade as the player's own sound.** The schedule passed the thrower's id as the voice's
  owner. The player's own sounds are never occluded, so a grenade behind a wall would have sounded
  clear. `Explode` now plays with owner 0. Test 30 checks `source == 0`.

## 4. Spec corrections

- Ruling 22 voids ruling 8's hurt duck.
- Ruling 23 records the grenade as built. A grenade at rest waits out its fuse, so the player can
  turn or walk before it sounds; it does not go off at rest.

## 5. Results

- **Clip lengths** (length; last sample above −60 dB):

  | Clip | Length | Last above −60 dB |
  |---|---|---|
  | `enemy_fire.wav`, committed before | 450 ms | 447 ms |
  | `enemy_fire.wav`, committed now | 1,200 ms | 1,195 ms |
  | `explode.wav` | 2,200 ms | 2,195 ms |

  - Frank's local `assets/local/audio/weapons/enemy_fire.wav` is 1,500 ms (48 kHz), with its last
    sample above −60 dB at 1,267 ms and above −40 dB at 1,070 ms. The bank looks in
    `assets/local/audio` first, so his play uses that clip, which was never truncated. It was not
    modified.
- **Test 30** (`t30_grenade_explode_pans`):
  - thrown at step 6 from (0, −0.05, −10) facing north, it goes off at step 155 at
    (0, 0, −19.011);
  - the arc worked out independently gives (0, 0, −19.011) after 0.957 s of flight;
  - unoccluded (0 walls);
  - with the listener facing west the right channel is +13.91 dB over the left, and facing east
    −13.91 dB.
- **Suites:**

  | Suite | Result |
  |---|---|
  | `audio_schedule_tests` | 19/19 |
  | `audio_system_tests` | 28/28 |
  | `combat_tests` | 64/64 |
  | `api_tests` | 68/68 |
  | `runtime_tests` | 37/37 |
  | `check_cli --cpu-only` | 29/29 |
  | `check_docs` (GPU), after the re-baseline below | 131/131 |

- **`tools/check.py --all`:** 51 of 55 tests passed before the doc hash fix.
  - `check_docs` failed on the moved hash below and passed once it was updated.
  - Three benches failed as in steps 1–4:
    - `scene_bench` bench10k: `save_pretty` 11.14 ms (budget 10), `validate` 10.11 (10),
      `apply_100_field_set` 0.235 (0.2);
    - `render_bench` `cornell_720p lighting` 0.368 ms (0.32);
    - `feel_bench` `frame_p99_over_median` 2.51 (1.5; SP9 ruling 21, open). Look latency passed:
      9.47 ms against 15.
  - Everything else passed, including `check_live`, `e2e_mcp`, `check_validation`, the render
    goldens and `audio_bench`.
- **Re-baselines, each reviewed line by line:**
  - `tests/serve/expected/audio.jsonl`, for the hurt removal: the `world` −4 dB duck commands and
    the `hurt` plays are gone, and `stolen` went from 1 to 0. Shots, impacts and positions are
    unchanged.
  - For the grenade: the play scene's saved `InputActions` gained the G binding, which moves
    `world.hash`, and only hash lines changed:
    - `audio.jsonl` lines 9 and 13;
    - `combat.jsonl` line 17;
    - `player.jsonl` lines 8 and 17;
    - the two hash lines of the player example in `docs/agents/ohao-serve.md`.
  - The `enemy_fire` change moved no expectation: the schedule reads no file lengths.

## 6. Known gaps

- A grenade in flight is not in a snapshot: `world.restore` keeps the grenades in flight, and a
  restart clears them.
- No bounce, so a throw into a wall sticks there.
- `t27_behind_cover_render` reads Frank's local `enemy_fire` when it is staged, so its printed
  numbers depend on the machine: on this machine, −9.02 dB broadband and −19.56 dB above 2 kHz.
  Both are within the test's limits.

## 7. How to run

```
build/Release/audio_schedule_tests.exe
build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
```

In the game, press G, then turn or walk before it goes off (2.5 s).

## 8. Follow-up: the grenade sampled in space

Frank asked for the space to be sampled, not only listened to. `python tools/audio/grenade_spatial.py`
runs `audio_bench --grenade-spatial DIR`, then measures and judges what it rendered:

- **Setup:** the play scene's player stands outdoors at (0, −0.05, −20) facing north, with the
  ambience bus muted.
- **Booms:** the grenade goes off 4 m and 20 m to its left and right, and 4 m in front. Each is
  thrown with G from where a level throw lands there, and the player is put back before it goes
  off.
- **Signal path:** everything goes through the schedule and the window's `AudioPlayer` on a mixer
  without a device. The panning measured is therefore the window's own (the listener from the
  frame's camera).
- **Output:** each 3 s window's measurements go to `grenade_spatial.csv`/`.json` beside the WAV.
  - By hand it writes `C:/Users/djmax/ohao_refs/listen/sp11_audio` (not committed).
  - CTest `grenade_spatial` (label `quick`, 0.66 s in `check.py`) writes `build/grenade_spatial`.

Measured (distance from the ears, 1.57 m above the ground; azimuth right of the facing):

| Window | Azimuth | Distance | L RMS | R RMS | ILD dB | Peak | dBFS | Azimuth guess | |
|---|---|---|---|---|---|---|---|---|---|
| LEFT-near | −90.0 | 4.30 | 0.03456 | 0.00718 | +13.65 | L | −32.06 | −47.7 | PASS |
| LEFT-far | −90.0 | 20.06 | 0.00766 | 0.00154 | +13.94 | L | −45.16 | −48.4 | PASS |
| RIGHT-near | 90.0 | 4.30 | 0.00718 | 0.03455 | −13.65 | R | −32.06 | +47.7 | PASS |
| RIGHT-far | 90.0 | 20.06 | 0.00154 | 0.00765 | −13.94 | R | −45.16 | +48.4 | PASS |
| FRONT-near | 0.0 | 4.30 | 0.01791 | 0.01790 | +0.00 | L | −34.94 | −0.0 | PASS |

- **Checks:** LEFT ILD ≥ +6 dB; RIGHT ≤ −6 dB; FRONT |ILD| ≤ 2 dB; no other sound starting in a
  window. 20 m is 13.10 dB below 4 m on both sides, against the 6 dB required. All 12 checks pass.
- **The gate can fail:** a copy of the render with its channels swapped fails all four side checks.
- **The azimuth guess** inverts a full constant-power pan. miniaudio's spatializer gives about
  13.7 dB at 90°, so the guess reads about ±48°. Its sign is the reliable part, not its size.
- **The near-far drop** of 13.1 dB is close to the `blast` shape's inverse distance between the two
  ear distances (20 log10(20.06/4.30) = 13.4 dB), with the outdoor reverb's tail in both windows.
