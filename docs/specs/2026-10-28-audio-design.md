# Sub-project 11 — Audio: a voice schedule from the step, a mixer with buses, snapshots and reverb zones

Status: written 2026-10-28, after Frank's "ok, continue" on the fireflies fix, as Day 1 of
sub-project 11 (documents only; no code).
- **PROVISIONAL rulings.** Frank deferred his rulings on the audio stack, determinism, content and
  the order to this row. They are written under "Rulings for Frank" as **PROVISIONAL**
  (recommended, not yet ruled), and the design follows them.
- **OPEN rulings.** Every other judgment call is listed there as **OPEN**, with a recommendation.
- The spec is binding once Frank accepts it.

Roadmap: row 11 of `docs/ROADMAP.md`. Done when:
- the same fight gives the same voice schedule;
- occlusion audibly changes behind cover;
- a mix-cost budget exists;
- Frank signs off the mix by ear.

Neighbours:
- **Before it:** 9 (Feel) and 10 (Animation). Both are built and wait for Frank's hand tests and
  rulings. Nothing here depends on those rulings.
- **After it:** 12, VFX and transparency.

Builds on:
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): the API registry
  (`ohao/api/session.cpp`'s method table and tool docs), `ohao serve` and its MCP tools under the
  24 KB `tools/list` cap, `world.snapshot`/`world.restore`, `input.record`/`input.replay`, and the
  window host (`runtime/live_host.cpp`).
- **Sub-project 5** (`2026-10-26-level-pipeline-design.md`): level hints in glTF `extras`
  (`ohao/level/level_hints.hpp`, `tests/level/extras_schema.json`): lower-case keys such as `surf`
  and `trig`, and capitalised component passthrough.
- **Sub-project 6** (`2026-10-26-player-physics-design.md`):
  - the fixed 60 Hz step;
  - the physics event log (`Footstep`, `Land`, with the ground's `Collider.surface`);
  - `PhysicsScene::raycastFrom`;
  - the player's head as the camera;
  - snapshots that replay bit for bit through the World's `SnapshotHook`.
- **Sub-project 7** (`2026-10-27-combat-design.md`): the combat event log (`shot`, `hit`,
  `damage`, `death`, `reload`, `reloaded`, `round_start`, `restart`, `win`, each with `step` and
  `seq`), the round's phases, and the generated weapon sounds (`assets/audio/weapons/`,
  `tools/audio/make_weapons.py`).
- **Sub-project 9** (`2026-10-27-feel-design.md`, "Audio quick wins"):
  - one `AudioSystem` for footsteps and combat (`runtime/game_audio.hpp`);
  - the 24-voice SFX cap that steals the quietest at the listener;
  - distance curves per kind (`SoundShape`, `shapeFor`);
  - one occlusion ray per enemy sound (−8 dB, 1.2 kHz low-pass);
  - `AudioSystem::initialize(true)`, a mixer without a device;
  - `physics_bench occlusion_ray_us`.
- **Sub-project 10** (`2026-10-28-animation-design.md`): the animation event log (`AnimEvent`:
  `mag_out`, `mag_in`, `bolt`, `body_fall`), whose handling sounds play at the player's ear or with
  an occlusion ray (`runtime/combat_sounds.cpp` `soundsForAnim`, `planAnimSound`). Its OPEN
  ruling 17 left foot-synced footsteps to this row.

## Thesis: the voice schedule is simulation; sound is presentation

What plays, when, on which bus and how loud is decided in the step, from saved state and the
step's events. The result is written to a **voice schedule**: a log of commands, each stamped with
its step.
- **Voice commands** say which sound starts, as which variant, on which bus, with which priority,
  where, with which gain, occlusion and reverb send, and whether it took a voice, stole one or was
  dropped.
- **Mix commands** change a bus gain, a snapshot, a reverb zone, or stop the world's voices.

Every host builds the schedule, headless ones included. It does not depend on a device, a frame
rate or which file is on disk. It is the artifact the tests, the agents and Frank's "the same
fight gives the same voice schedule" compare.

The mixer then renders the schedule. A window's miniaudio device plays each command at its step's
time on the audio clock, sample-accurately: two steps run in one frame start their sounds 1/60 s
(800 frames at 48 kHz) apart, not together.
- **The schedule is authoritative.** It is pure, deterministic and testable without a device.
- **The mixed signal is presentation.** It follows the frame's listener at render rate for
  panning, and sounds whatever files the machine has (the local recordings first).

A deviceless render (`ma_engine_read_pcm_frames` on the existing `noDevice` engine) turns a
schedule into PCM in a test, so the mix's arithmetic is checked by numbers too: occlusion behind
cover, a snapshot's low-pass, a reverb tail.

## Goal

On `ohao_game assets/scenes/blockout_play.scene.json`, the fight is mixed:
- **Buses with caps:** sounds play through buses (weapons, impacts, foley, voice, ambience, UI,
  music) under one master. Each bus has its own voice cap, and a priority decides who is stolen
  when a bus or the whole mixer is full.
- **Ducking:** the player's own shots duck the ambience and the foley; being hurt ducks the world.
- **Snapshots:**
  - dying muffles the world (`dead`) until the restart;
  - winning settles the mix (`won`);
  - each blends over its own time.
- **Reverb zones** from level hints and scene data give the room its tail, and the open air its
  short slap. The zone follows the listener.
- **Occlusion:** an enemy behind cover is muffled by the walls between, graded by how many.
  Long sounds re-check as the listener moves.
- **Sample-accurate timing:** every sound starts at its step's time on the audio clock.
- **Footsteps:** enemies' footsteps fall on their feet (clip events); the player's stay distance
  based.
- **Agents:** they read the mix (`audio.info`: buses, voices, the snapshot, the zone, the schedule
  since a cursor), and set bus gains, mute, solo or a snapshot for a test (`audio.mix`).
- **Budgets:** the mix's CPU cost and the schedule's step cost are gated.
- Frank signs the mix off by ear.

## Scope: in 11 vs later

In 11:
- **The schedule:** the `audio_schedule` simulate system and its log, built in every host. The
  sound rules move out of the window host into it (which sound an event plays, the round robin,
  the distance curve, the occlusion ray).
- **Data:** a sound bank and mix as data (`.audio.json`), and the saved singleton `AudioMix`
  naming it.
- **Buses:** real buses (`ma_sound_group` per bus, nested under master); bus gains, mute and solo;
  per-bus voice caps; priority and stealing in the schedule.
- **Mix rules:** ducking rules and named snapshots with blend times, driven from the round's state
  and the step's events.
- **Reverb:** the `ReverbZone` component, the level hint `verb`, presets as data, and one reverb
  bus (a send) whose preset follows the listener's zone.
- **Occlusion polish:** graded by the walls between (0, 1, 2 or more), re-cast for long voices,
  smoothed; still one ray per cast, built on Feel's.
- **Scheduling:** sample-accurate starts from step stamps (the audio clock anchored to the step
  clock), the horizon, and the restart's stop.
- **Footsteps:** enemies' from clip events (`foot_l`, `foot_r`) on the near curve; the player's
  stay physics' distance footsteps.
- **Content:**
  - sounds preloaded and decoded at bank load (no file opened on a play);
  - a generated ambience bed per preset;
  - licence notes for every committed folder;
  - the local-folder-first rule kept for every bank sound.
- **Agents and benches:** `audio.info` and `audio.mix` for agents; a new `audio_bench` (deviceless
  mix cost, schedule cost).
- **Tests and acceptance:** tests 26–29, look-and-listen recordings, and Frank's hand test.

Later:
- **Not in 11:**
  - a music system (the music bus exists, nothing schedules it);
  - a dialogue or voice-over pipeline;
  - HRTF and binaural rendering (PROVISIONAL ruling 1);
  - per-source reverb zones (sources send to the listener's zone's reverb);
  - diffraction and portals (a sound around a doorway is occluded, not bent);
  - Doppler.
- **Untouched here:** the HDRI orientation (row 16), pacing (SP9 ruling 21), VFX (12).
- **13:** ragdoll impact sounds from contact events.
- **15:** cooked, compressed audio in the asset database. The bank decodes WAV at load here.

## Why (what the code does today)

- **The rules live in the window host.** Which sound an event plays, and how, is in
  `runtime/footsteps.cpp` and `runtime/combat_sounds.cpp` (`soundsFor`, `planCombatSound`,
  `soundsForAnim`, `planAnimSound`, `FootstepSounds`' round robin). Each reads its event log by
  cursor once per frame, after the frame's steps (`live_host.cpp` 203–205).
  - A headless host plays nothing and schedules nothing, so no test or agent can see what the
    fight would have sounded like.
  - The voice cap's choices depend on what is still playing on the device, which depends on the
    frame rate.
- **Sounds start when the frame runs, not when their step did.** All the sounds of a frame's steps
  start together at the next `update`, so two shots 1/60 s apart can play on the same sample.
- **The buses are multipliers.** `SoundCategory` (SFX, Music, Ambient) is a volume multiplied into
  each sound (`m_categoryVolumes`); there is no `ma_sound_group` and no bus node to filter, duck or
  send.
- **One cap for everything.** 24 SFX voices, stealing the quietest at the listener, with no
  priority. An enemy's footstep and a distant impact are equal; the player's own shot can be
  dropped when it would be the quietest.
- **Occlusion is binary and fixed at start.** One ray when a sound starts: −8 dB and 1.2 kHz, or
  nothing. A long sound keeps its first answer however the listener moves.
- **No reverb and no zones:** miniaudio's core has no reverb node, and no hint or component
  describes a space.
- **A file is opened on every play.** `playSoundAt` calls `ma_sound_init_from_file` (flags 0) on
  the main thread each time. Nothing is preloaded or decoded ahead, and the cost and any first-play
  hitch land in a frame.
- **Content:**
  - `assets/audio/weapons/` has a README;
  - `assets/audio/footsteps/` (16 generated WAVs, `tools/audio/make_footsteps.py`) has none;
  - there is no ambience.
  - Local recordings (`assets/local/audio/footsteps/`) are found first for footsteps only.
- **Stack:** miniaudio 0.11.21 (`external/cmake/miniaudio.cmake`, public domain or MIT-0), one
  header, already in the tree. It has everything 11 needs:
  - sound groups (nested buses);
  - `ma_sound_set_start_time_in_pcm_frames` (sample-accurate starts on the engine clock);
  - custom nodes (a reverb, a bus filter);
  - `ma_lpf_node`;
  - a deviceless engine read by `ma_engine_read_pcm_frames`.

## Data: the bank and the mix (`.audio.json`)

One file per game, referenced by a saved singleton (OPEN ruling 14):
`assets/audio/slice.audio.json`, named by `AudioMix { asset }` in `blockout_play.scene.json`. A
world without `AudioMix` uses a built-in default equal to today's behaviour, so other scenes play as
before. Parsed and validated like `.animgraph.json`: every error has its JSON pointer, and
`ohao schema` exports its schema.

```json
{
  "folders": ["assets/local/audio", "assets/audio"],
  "buses": {
    "master":   {},
    "world":    {"parent": "master"},
    "weapons":  {"parent": "world", "cap": 12},
    "impacts":  {"parent": "world", "cap": 8},
    "foley":    {"parent": "world", "cap": 10},
    "ambience": {"parent": "world", "cap": 4},
    "voice":    {"parent": "master", "cap": 2},
    "ui":       {"parent": "master", "cap": 4},
    "music":    {"parent": "master", "cap": 2},
    "reverb":   {"parent": "master", "return": true}
  },
  "voices": 32,
  "sounds": {
    "fire":       {"files": "weapons/fire", "bus": "weapons", "priority": 250, "shape": "ear", "holdMs": 450, "send": 0.15},
    "enemy_fire": {"files": "weapons/enemy_fire", "bus": "weapons", "priority": 220, "shape": "gunfire", "holdMs": 600, "send": 0.35, "occlude": true},
    "footstep_*": {"files": "footsteps/{surface}_{n}", "bus": "foley", "priority": 100, "shape": "near", "holdMs": 250, "send": 0.1}
  },
  "shapes": {"gunfire": {"min": 1, "max": 60, "rolloff": 1}, "near": {"min": 0.5, "max": 20, "rolloff": 1.5}},
  "ducks": [
    {"on": "fire", "bus": "ambience", "db": -6, "attackSteps": 1, "releaseSteps": 15},
    {"on": "fire", "bus": "foley", "db": -3, "attackSteps": 1, "releaseSteps": 15},
    {"on": "hurt", "bus": "world", "db": -4, "attackSteps": 1, "releaseSteps": 18}
  ],
  "snapshots": {
    "default": {},
    "dead": {"buses": {"world": {"db": -9, "lowPassHz": 900}, "ambience": {"db": -12}}, "inSteps": 18, "outSteps": 30},
    "won":  {"buses": {"world": {"db": -4}}, "inSteps": 60, "outSteps": 30}
  },
  "reverb": {
    "outdoor":  {"room": 0.30, "damp": 0.60, "wet": 0.05, "bed": "ambience/outdoor"},
    "room":     {"room": 0.70, "damp": 0.40, "wet": 0.25, "bed": "ambience/room"},
    "corridor": {"room": 0.80, "damp": 0.30, "wet": 0.30, "bed": "ambience/room"}
  },
  "defaultZone": "outdoor"
}
```

The values above are the proposals (OPEN rulings 8–12); the shape of the file is the design.

**Sounds are names, files are presentation.** A schedule command names a sound and a variant
index, never a path.
- `holdMs` is how long the sound holds its voice for the cap. It is declared, not read from the
  file, so a local recording of another length never changes the schedule (OPEN ruling 13).
- `files` is a pattern under each folder in turn. The first folder holding any variant supplies all
  of them, as footsteps do today.

## The step: the schedule (`ecs/audio_schedule.{hpp,cpp}`)

**Installation.** `ecs::installAudio(World&, assets)` adds the `audio_schedule` simulate system.
The CLI context and `LiveHost` call it after `installAnimation`, so it is the step's last system.
It reads the step's events:
- physics footsteps and landings;
- combat;
- animation clip events.

From them, the listener and the director's own state, it appends commands to a log of 4,096 with
`seq` and `step`, read like the other logs (`eventsSince`).

**Which sound.** The rules of `soundsFor`, `soundsForAnim` and footsteps' round robin move here
unchanged. Enemies' footsteps now come from clip events: `foot_l` and `foot_r` added to the enemy
graph's locomotion clips (OPEN ruling 15). Physics' distance footsteps stay for the player, and for
any character without a clip event.

**The listener at the step.** It is the input-driven player's head at the step's pose, else the
first active `Camera`'s world transform. It is never the render pose. Panning in the device follows
the frame's camera; priority, distance and occlusion in the schedule use the step's.

**Placement.**
- `shape: ear` plays non-spatially at the listener (the player's own fire, hurt, reload and
  handling).
- Otherwise it plays at the event's point on its named curve.
- `gain` is the bank's volume times any per-event scale. The footstep's speed scale stays as today.

**Occlusion** (Feel's ray, polished; `occlude: true` sounds only, never the player's own):
- One `raycastFrom` listener → source per cast, passing through characters (as today, at most 4
  hops). The walls hit are counted: 0 → none, 1 → −8 dB and 1.2 kHz, 2 or more → −14 dB and 700 Hz
  (OPEN ruling 12).
- **Re-casts:** a voice still holding its voice after 6 steps (`holdMs` > 100) is re-cast every 6
  steps, as an `occlusion` command for that voice. The device ramps to it over 3 steps.
- **Ray cap:** at most 16 rays a step. Casts past the cap reuse the voice's last answer and are
  counted. A new sound past the cap counts as unoccluded (counted), so the cap never changes a
  command's existence, only its parameters.

**Voices: caps, priority, stealing.** The director keeps the virtual voice table: every started
voice with its bus, priority, audible gain at the listener, and end step (`start + holdMs`,
rounded up to steps). A voice ends at its end step, or when stolen or stopped. A new voice:
1. Over its bus cap, or the global `voices`: the candidate victims are the voices with lower
   priority. If there are none, the voices with equal priority and a lower audible gain.
2. The lowest priority goes, then the quietest, then the oldest by `seq`.
3. With no victim the new one is **virtual**: written as `dropped`, never played, and counted.

All of it is integer and float arithmetic on schedule state in id and `seq` order, so the same
fight makes the same choices.

**Ducking.** A `ducks` rule fires when its sound is scheduled. The director keeps each bus's duck
envelope in steps (attack, hold, release) and writes a `bus` command when a bus's target gain
changes. Several ducks on one bus take the deepest.

**Snapshots.** One active snapshot, chosen in the step from the round:
- `dead` from the player's `death` until the `restart`;
- `won` from `win` until the `restart`;
- else `default`.

A change writes a `snapshot` command with its blend in steps (`inSteps` of the one entered). The
device blends bus gains and the bus low-pass over that time, in decibels for gain and in log
frequency for the cutoff. No snapshot changes the schedule's voice choices: they are mix levels
only (OPEN ruling 9).

**Reverb zones.** The listener's zone is the highest-priority `ReverbZone` whose box contains the
listener at the step, else the bank's `defaultZone`. A change writes a `zone` command (preset,
blend 30 steps). The zone's `bed` loops on the ambience bus, crossfaded over the same blend.

**The restart.** A `restart` event writes `stop: world` before the restored step's commands. It
ends every world voice in the table and stops them in the device with a 3-step fade. The voice
table, duck envelopes, snapshot and zone are the director's state at the round's start (below).

**Snapshots and replay.** The director's state goes into the World's `SnapshotHook` beside
physics': the voice table, round-robin counters, duck envelopes, snapshot, zone and the log's next
`seq`. It is runtime: never saved, never hashed. A restore, a round restart and an
`input.replay` resume the same schedule. The log itself is not snapshotted (as with combat's), so a
restore writes new `seq`s for the replayed commands, with the same content.

## The frame: the player (`runtime/audio_player.{hpp,cpp}`)

A window host's `AudioPlayer` replaces `Footsteps` and `CombatSounds` (forward only: both are
deleted). Once a frame, after the frame's steps, it reads the schedule since its cursor and drives
`AudioSystem`.

**The clock.** The player anchors step `s0` to engine frame `f0` the first time it plays.
- Step `s` plays at `f0 + (s − s0) × rate / 60 + horizon`. The horizon is one step (800 frames at
  48 kHz, OPEN ruling 7).
- A command whose time has passed (a frame later than the horizon: a stall, a pause) starts at once
  and is counted `late`.
- When the timeline pauses, steps in lockstep, or the anchor drifts more than one step from the
  device clock, the anchor is re-taken. Lockstep plays each command at once plus the horizon.

**Voices.** A `play` command starts the bank's preloaded, decoded sound (`MA_SOUND_FLAG_DECODE`
at bank load; one decoded copy per variant; voices are `ma_sound`s copied from it):
- in its bus's `ma_sound_group`;
- with `ma_sound_set_start_time_in_pcm_frames`;
- at its position on its curve;
- through a per-voice low-pass when occluded;
- with its reverb send.

`dropped` plays nothing. `steal` stops the victim with a 2 ms fade first.

**Buses** are nested `ma_sound_group`s, each with a gain and a bus low-pass node (for snapshots).
`reverb` is a return: a send node per world bus feeds a Freeverb-style node (OPEN ruling 10), whose
output goes to master. Bus, snapshot and zone commands ramp at their step's time.

**Mute.** `ohao_game --mute` still builds the schedule and plays nothing. The player and its
device are not created, as today.

## The agent-facing surface

- **`audio.info {since?, max?}`** returns:
  - `{device, sampleRate?, horizonMs, snapshot: {name, blend}, zone, listener: {pos},
    buses: [{name, gain, db, muted, solo, voices, cap}], voices: {playing, virtual, dropped, stolen},
    rays: {cast, capped}}`;
  - the schedule since the cursor: `{commands: [{seq, step, op, sound?, variant?, bus?, prio?, gain?,
    pos?, occl?, send?, outcome?, victim?, snapshot?, zone?}], next, dropped?}`.

  It answers in every host. A headless host has `device: "none"` and the schedule, which is what
  tests and agents compare. It stays under 2 KB for the play scene mid-fight with `max` 16 (a
  `serve_bench` budget).
- **`audio.mix {bus?, db?, mute?, solo?, snapshot?, clear?}`** returns as `audio.info`. It sets a
  session override for tests and auditioning: a bus's gain, mute or solo, or a forced snapshot.
  - It is not world state: not saved, not hashed, not snapshotted, and absent from the schedule.
  - The schedule records the game's mix; the override applies in the device only, and `audio.info`
    reports it.
  - `clear: true` drops every override.
- **`render.stats`** gains `audio {mixMs, voices, late}` from a window's player (the callback's
  measured cost over the frames read).
- **Two new tools** against `tools/list`'s 24 KB cap, which is at 24,137 bytes now: OPEN ruling
  16.
- **`ohao schema`** exports `.audio.json`'s schema, and `level-extras` gains `verb`.

## Components and hints

- **`AudioMix { AssetRef asset; }`** is saved, a singleton like `Round`: the bank and mix of the
  world. Its name moves every `world.hash` expectation once (saved types hash their names).
- **`ReverbZone { std::string preset; int32_t priority = 0; }`** is saved, and its box is its
  entity's `Collider` box (world space, at the step).
  - In a scene it is authored as passthrough beside a `Collider` with `trigger: true`.
  - In a level, a node's hint `"verb": "<preset>"` gives `ReverbZone{preset}`, and makes the node a
    trigger box collider unless it says otherwise.
  - The preset must exist in the world's `.audio.json`; an unknown one is `W_AUDIO_PRESET` at load,
    and the zone counts as the default.
- **The play scene** gets zones authored in `blockout_play.scene.json`: `room` over the room's
  interior, plus the bank's default `outdoor` outside. The level file is not rebuilt (OPEN ruling
  17). `tools/level/blockout_room.py` gains the hint's support for later levels, and the extras
  schema and its test gain the key.

## Performance budgets

The display targets are 240 Hz (4.17 ms a frame, this laptop's panel) and 144 Hz (6.94 ms,
Frank's). Audio's main-thread work must stay a sliver of either. The mix runs on miniaudio's device
thread, in 10 ms periods at 48 kHz (480 frames); its budget is a share of that period.

| Measure | Bench | Budget | Gated from |
|---|---|---|---|
| `audio_schedule` system, the play scene mid-fight (2 enemies firing, footsteps, handling) | `audio_bench schedule_step_ms` | ≤ 0.02 ms a step | Task 3 |
| `audio_schedule`, 16 enemies firing and running (ray cap reached) | `audio_bench schedule16_step_ms` | ≤ 0.10 ms a step | Task 3 |
| The player's per-frame submission, the play scene mid-fight | `audio_bench submit_frame_ms` | ≤ 0.05 ms a frame | Task 3 |
| Mix cost: 32 voices playing, every world bus filtered, reverb on, 48 kHz stereo | `audio_bench mix_10ms_ms` (deviceless render, median of 300 periods) | ≤ 0.50 ms per 10 ms of audio (5% of a core) | Task 1 |
| Reverb alone (the Freeverb node, stereo) | `audio_bench reverb_10ms_ms` | ≤ 0.15 ms per 10 ms | Task 2 |
| One occlusion ray | `physics_bench occlusion_ray_us` (SP9's) | ≤ 5 µs | kept |
| Bank load: decode every committed sound of the slice | `audio_bench bank_load_ms` | ≤ 150 ms; reported, gated from Task 3 | Task 3 |
| `audio.info` result, max 16 | `serve_bench audio_info_bytes` | ≤ 2 KB | Task 4 |
| Look latency (no regression) | `feel_bench` | `look_latency_ms` ≤ 15 ms | Task 4 |

Notes on the table:
- **Main thread:** at 240 Hz the schedule and submission together are ≤ 0.07 ms a frame with one
  step (1.7%). The 16-enemy case is ≤ 0.15 ms (3.6%).
- **Caps:** the voice caps per bus are the bank's (proposal: weapons 12, impacts 8, foley 10,
  ambience 4, voice 2, UI 4, music 2; 32 in all). Exceeding a cap is a stolen or virtual voice in
  the schedule, never a cost spike.
- **Memory** is reported, not gated: the decoded bank (the slice's generated set is under 4 MB at
  48 kHz mono 16-bit, decoded to f32).
- **Gating:** `audio_bench` is CPU-only, with `scene_bench`'s calibration and busy-process probe.
  Noisy runs are neither a pass nor a fail. Its budgets are proposals until Task 1–3's measurements
  (OPEN ruling 11).
- **Look latency:** `look_latency_ms` keeps SP9's budget. Pacing stays under SP9's open ruling 21,
  reported, not gated here.

## Determinism

- **The schedule is a function of the step.** It depends only on:
  - the saved world and the step's event logs (themselves deterministic);
  - the listener at the step;
  - rays from the physics scene at the step;
  - the bank's declared values.

  Two runs of the same fight, a restore mid-fight, a round restart and an `input.replay` give the
  same commands (every field but `seq` after a restore). Window and headless hosts give the same
  commands for the same steps, because nothing in the schedule reads the frame, the device or a
  file's length.
- **The director's state** (voice table, round robin, ducks, snapshot, zone) is runtime, carried
  by snapshots, never saved or hashed. `world.hash` is unchanged by playing a fight with sound.
- **The mixed signal is presentation.** The device's timing, the frame's panning and the files'
  contents vary by machine. A deviceless render of the same schedule with the same files is
  bit-identical run to run (single-threaded float mixing in a fixed order), which the mixer tests
  rely on. No committed golden is made of rendered audio (OPEN ruling 18).
- **Hashes:** the new saved types (`AudioMix`, `ReverbZone`) move the serve and CLI `world.hash`
  expectations once, with that reason. No render golden moves: nothing drawn changes.

## Test plan

Tests are numbered after SP10's (test 25):
- 26: the mixer;
- 27: zones and occlusion;
- 28: the schedule;
- 29: agents and the slice.

- **Test 26** (`audio_system_tests`, deviceless, rendering through `ma_engine_read_pcm_frames`):
  - `t26_bus_tree`: the bank's buses are nested groups; a bus's gain scales only its subtree's
    output (RMS within 0.1 dB of the expected).
  - `t26_mute_solo`: mute silences a bus; solo silences every bus that is not the soloed one, its
    ancestors or its descendants.
  - `t26_start_time_sample_accurate`: two voices scheduled 800 frames apart start 800 frames apart
    in the render (onset detected to the frame).
  - `t26_duck_envelope`: a duck's gain curve in the render follows attack, hold and release in
    steps (within 0.2 dB at each step boundary).
  - `t26_snapshot_blend`: `dead`'s −9 dB and 900 Hz reached after `inSteps`. Energy above 2 kHz
    falls by at least 12 dB; the gain is linear in dB over the blend.
  - `t26_steal_fade`: a stolen voice fades over 2 ms, without a click (no sample step over a
    threshold).
  - `t26_render_repeatable`: the same schedule rendered twice is bit-identical.
- **Test 27** (`audio_system_tests`, `audio_schedule_tests`, `level_physics_tests`,
  `check_level_assets.py`):
  - `t27_reverb_preset_tail`: an impulse through each preset decays with its RT60 within 20% of
    the preset's design value; `outdoor` is shorter than `room`.
  - `t27_reverb_send_per_bus`: dry voices have no tail; the send scales it.
  - `t27_zone_containment`: the listener in, out, on the edge, and in two overlapping zones (the
    higher priority wins; ties go by id).
  - `t27_zone_change_command`: crossing a zone's edge writes one `zone` command at that step, and
    its bed crossfades.
  - `t27_verb_hint`: a node with `"verb": "room"` becomes a trigger box with
    `ReverbZone{room}`; an unknown preset is `W_AUDIO_PRESET`; the extras schema accepts `verb`.
  - `t27_occlusion_graded`: listener and source with 0, 1 and 2 walls between give 0, −8 and
    −14 dB and none, 1.2 kHz and 700 Hz.
  - `t27_occlusion_recast`: a long voice re-cast every 6 steps as the listener steps behind a
    wall; one `occlusion` command at the change.
  - `t27_behind_cover_render`: the play scene, an enemy firing at a fixed spot, the listener in the
    open, then behind the pillar.
    - The schedule's command changes from none to occluded.
    - Rendered, the shot's energy above 2 kHz falls by at least 8 dB behind cover, and its broadband
      level by at least 6 dB.
    - This is the row's "occlusion audibly changes behind cover", in numbers.
  - `t27_ray_cap`: 20 sounds in one step cast 16 rays, and 4 reuse or default (counted); the
    commands still exist.
- **Test 28** (`audio_schedule_tests`, a new CPU suite; `combat_tests`; `api_tests`):
  - `t28_rules_moved`: every case of `sp9_t21_*` and `sp10_t23_*` (which sound, where, which
    curve, the player's own at the ear, occlusion only for enemies) holds through the schedule.
    `runtime_tests`' copies are deleted with the code they tested.
  - `t28_priority_and_steal`: over a bus cap, a lower priority is stolen first, then the
    quietest, then the oldest. A new voice that would lose is `dropped`. The player's shot is never
    dropped for an impact.
  - `t28_hold_declared`: swapping a sound's file for one of another length changes no command.
  - `t28_duck_rules`, `t28_snapshot_from_round`: `dead` on the player's death, `default` on the
    restart, `won` on `win`, each with its blend.
  - `t28_same_fight_same_schedule`: the done demo's scripted fight, twice from a fresh load: the
    schedule dumps are identical.
  - `t28_restore_mid_fight`: a snapshot mid-burst, restored and stepped the same, gives the same
    commands (every field but `seq`).
  - `t28_restart_like_start`: the commands after a round restart equal those after the round's
    start, through the same inputs.
  - `t28_replay`: `input.record` then `input.replay` gives the same schedule.
  - `t28_window_equals_headless` (`check_live.py`): a hidden window's schedule for an injected
    fight equals a headless session's.
  - `t28_enemy_footsteps_on_feet`: an enemy's run gives footsteps at its clip's `foot_l` and
    `foot_r` steps, on its surface, occluded by the walls between.
  - `t28_hash_unchanged`: `world.hash` after a fight is the same with and without
    `installAudio`.
- **Test 29** (`runtime_tests`, `api_tests`, `check_serve.py`, `e2e_mcp.py`, `check_live.py`,
  `audio_bench`):
  - `t29_player_clock`: two steps in one frame start their sounds one step apart on the engine
    clock; a late command starts at once and counts `late`; a pause re-anchors.
  - `t29_preloaded`: no file is opened by a play after the bank loads (a counting file hook).
  - `t29_audio_info`, `t29_audio_mix`: the shapes, the size under 2 KB, the cursor, an override in
    `audio.info` and not in the schedule, `clear`, and `E_STATE` without `installAudio`.
  - A serve session `tests/serve/sessions/audio.jsonl`; `tools/list` within its cap
    (OPEN ruling 16).
  - `e2e_mcp.py` reads `audio_info` mid-fight, mutes a bus and solos one.
  - `audio_bench --check tests/bench/audio_budgets.json`: every budget above.
- Every existing suite stays green; `check.py --all`.

**Listening artifacts** (the audio form of look shots, for Frank and the change records):
`audio_bench --record` writes the done demo's fight rendered deviceless to
`C:\Users\djmax\ohao_refs\listen\sp11_audio\stepN_*.wav` (local only, never committed). Each comes
with a JSON of the schedule and a PNG of its bus levels over time.

## Hand-test acceptance (from Frank)

Empty until Frank plays `build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json`
with headphones and with speakers. Each item is closed by his note, a fix with its test, or a ruling
that it stays.

| # | Item | What to try | Frank's note | Status |
|---|---|---|---|---|
| U1 | Direction and distance | Walk around a firing enemy: left/right is clear, far is quieter and duller | | open |
| U2 | Behind cover | Step behind the pillar while an enemy fires: muffled; step out: clear again, without a jump | | open |
| U3 | Room vs outside | Fire inside the room, then outside the doorway: the room's tail, then the open air's short slap; the bed changes | | open |
| U4 | Ducking | Hold fire: the ambience and footsteps sit under your shots and come back after (ruling 8) | | open |
| U5 | Death | Die: the world goes muffled and distant (`dead`); the restart brings it back | | open |
| U6 | Win | Clear the room: the mix settles (`won`) | | open |
| U7 | Heavy fire | Both enemies firing while you fire and reload: your shots never vanish; enemy footsteps stay audible (priorities, ruling 11) | | open |
| U8 | Timing | Full-auto bursts sound even, not doubled or bunched, at 240 Hz and with the frame rate capped low (the horizon, ruling 7) | | open |
| U9 | Overall balance | Weapons against impacts, foley, voice and ambience: anything too loud or buried | | open |
| U10 | Local recordings | With the Somnabulate footsteps staged in `assets/local/audio/`, they play instead of the generated ones and the schedule is unchanged | | open |

## Done criteria

- The fight on the play scene plays through buses with caps, priorities, ducking, the `dead` and
  `won` snapshots, the room's and the outdoor reverb, and graded occlusion, each sound at its
  step's time.
- `t28_same_fight_same_schedule`, the restore, restart, replay and window-against-headless cases
  pass: the same fight gives the same voice schedule.
- `t27_behind_cover_render` passes: occlusion changes the rendered sound behind cover by the stated
  numbers.
- The mix-cost and schedule budgets are gated in `audio_bench`.
- Tests 26–29 pass. Every simulation test and render golden is unchanged; the `world.hash`
  expectations move once for the new saved types; `check.py --all` is green.
- The hand-test table is closed by Frank's notes.

## Build order

1. **The mixer graph** (on the existing `AudioSystem`):
   - buses as nested sound groups from the bank, with gain, mute, solo, a bus low-pass, and a
     reverb send node per bus (the reverb itself is a pass-through placeholder);
   - `.audio.json` parsing, validation and schema, and `AudioMix`;
   - the preloaded decoded bank with local-folder-first files;
   - start times and steal fades in `AudioSystem`;
   - deviceless render helpers;
   - test 26, `audio_bench mix_10ms_ms`.

   Feel's behaviour is kept through the default bank; nothing in the game changes yet.
2. **Reverb zones and occlusion:**
   - the Freeverb-style node and the presets;
   - `ReverbZone`, the `verb` hint, schema and level loader;
   - zone selection and the bed;
   - graded occlusion, re-casts and the ray cap;
   - the play scene's zones;
   - test 27, `reverb_10ms_ms`.
3. **The schedule and the player:**
   - `installAudio` and the `audio_schedule` system with its log and `SnapshotHook` state;
   - the rules moved from `runtime/`, which is deleted;
   - priority and stealing, ducks, snapshots, the restart;
   - enemies' footsteps from clip events;
   - `AudioPlayer` with the step clock and the horizon;
   - test 28;
   - the schedule and submit budgets and `bank_load_ms`;
   - the ambience beds generated and committed with licence notes.
4. **Agents, budgets, close:**
   - `audio.info`, `audio.mix`, `render.stats` `audio`;
   - the serve session, `check_live`, `e2e_mcp`, test 29;
   - every budget gated;
   - `check.py --all`;
   - the listening artifacts;
   - the change records, `AGENTS.md`, `ARCHITECTURE.md`, the guides, the monograph `audio` units;
   - the roadmap row and Frank's hand test.

Each step ends with the engine running, every gate green, a change record
(`docs/changes/YYYY-MM-DD-audio-stepN.md`) and listening artifacts `stepN_*.wav`.

## Risks

- **The device clock against the step clock.** The two drift (the device runs on its own crystal),
  and a stall or a pause breaks the anchor. Re-anchoring is bounded to one step of error and
  counted. If the bunching it causes is audible, the horizon grows (ruling 7), at the cost of
  latency.
- **Latency.** One step of horizon adds 16.7 ms between a shot's step and its sound, on top of the
  device period (10 ms). CoD-class shooters keep the player's own shot tight. If Frank hears it,
  the player's own `ear` sounds can play at horizon 0, since they are in the frame's newest step
  (OPEN ruling 7's alternative).
- **miniaudio's groups and graph.** Nested groups, per-voice low-passes and send nodes are
  supported, but the graph's cost grows with nodes. The mix budget is measured in Task 1, before
  zones and occlusion add nodes. A voice's low-pass is created only when occluded, as today.
- **The reverb's licence and quality.** miniaudio's extras ship a reverb node built on a
  third-party library, which is not in the core header. Our own node implements Jezar's Freeverb
  (public-domain algorithm), and its quality is Frank's ear (U3).
- **The dependency rule.** `ecs/audio_schedule` must not include `ohao/audio` (miniaudio) or the
  API. The schedule's types are plain data in `ecs/`, and `runtime/` bridges them to the device.
  The source lints guard it.
- **`tools/list`.** Two tools over a nearly full cap (ruling 16).
- **Content.** Generated ambience beds may read as synthetic. CC0 beds can replace them, each with
  its source note (ruling 19).
- **Gameplay drift.** Nothing here may change a simulation result. `t28_hash_unchanged` and every
  combat and physics expectation hold it.

## Review focus

- Two steps in one frame, one of them a restart: the stop and the restored step's commands in
  order, the old voices faded, no command left from the old timeline (`t28_restart_like_start`,
  `t29_player_clock`).
- A voice stolen in the same step it would end; and a bus cap and the global cap reached in the
  same step: one victim each, the same choice every run (`t28_priority_and_steal`).
- A snapshot change during another's blend (`dead` then `restart` mid-blend): the blend restarts
  from the current levels, without a jump (`t26_snapshot_blend`).
- The listener exactly on a zone's face, and two zones of equal priority: one zone, chosen the
  same way every run (`t27_zone_containment`).
- A sound name with no file in any folder: the schedule writes it, the player skips it with one
  warning per name, never a crash (`t29_preloaded` missing case).

## As built (step 1)

The mixer graph (plan Task 1, `docs/changes/2026-10-28-audio-step1.md`). What differs from the
design above, and why:

- **A bus is a group and a node of ours.** Each bus is an `ma_sound_group` (its volume is the old
  category volume) feeding an `AudioBusNode`: a gain linear in dB, ramped; a Butterworth low-pass
  ramped in log frequency (coefficients every 16 frames; 0 Hz = bypassed); a 2 ms mute/solo gate;
  a second output that sends to the return bus, attached only while the send is above 0. The plan
  named `ma_lpf_node` per bus and a splitter per world bus; neither ramps in dB, and one node does
  all four. Parameters cross threads through atomics and a version counter.
- **Start times and stop fades are the voice's own.** miniaudio 0.11.21 places an
  `ma_sound` start time inside a processed block at (block end − start) instead of (start − block
  begin) (`ma_node_read_pcm_frames`), and `ma_sound_stop_with_fade_in_pcm_frames` cut a voice at
  once. A bank voice reads its decoded PCM through `AudioVoiceSource`, which plays
  `startFrame − engineFrame()` frames of silence first and fades a stop itself. Without a device
  this is exact to the frame; with one, `startFrame` is relative to the engine clock read on the
  calling thread (step 3's schedule adds its horizon on top).
- **No pitch anywhere on the bank's path.** Voices and bus groups are made with
  `MA_SOUND_FLAG_NO_PITCH`: the bank is decoded at the engine's rate, and the pitch resampler (on by
  default) delayed a voice by a frame per node it crossed. Nothing in the engine sets pitch.
- **Decoding** is `ma_decode_file` into f32 at the engine's rate, every variant at `loadBank`, not
  `MA_SOUND_FLAG_DECODE` per voice: a voice copies nothing, it reads the shared buffer.
- **Interfaces as built:** `parseAudioBank(json)` returns `Result<AudioBank, std::vector<BankError>>`
  (path and message per error); `loadBank(bank, root)` searches the bank's own `folders` under
  `root`; `play` returns a `SoundHandle`; `render` returns the frames rendered. Bank voices are
  outside the category API's 24-voice cap: the bank's caps are the schedule's (step 3).
- **The slice's bank** (`assets/audio/slice.audio.json`) names footsteps per surface
  (`footstep_concrete` … `footstep_default`, files `footsteps/<surface>_{n}`) instead of the
  `footstep_*` pattern above: a schedule command names one sound. The reverb presets carry no
  `bed` until step 2 generates the ambience beds. Volumes are Feel's (fire, reload, empty, hurt,
  death 0.85; enemy fire 0.7; impacts 0.55; handling 0.6; body fall 0.8; footsteps by speed, at the
  call). The game still plays through the category API, so nothing it sounds like changed.
- **Mute and solo ramp even before a sound plays:** a solo set the frame a burst starts lets the
  first 2 ms of the other buses through (about −30 dB in `step1_buses.wav`'s soloed segments).
- **`mix_10ms_ms`** measures 32 voices without the reverb, which step 2 adds to the same mix.

## As built (step 2)

Reverb zones and occlusion (plan Task 2, `docs/changes/2026-10-28-audio-step2.md`). What differs
from the design above, and why:

- **A zone is a volume, not a trigger body.** `ReverbZone { preset, priority, size }`: `size` is
  half extents about the entity, in its frame; 0 fits the zone to the entity's mesh (its
  `CollisionMesh`, else its `MeshSource` model, cached). The design put a zone beside a trigger
  `Collider`; in the play scene that sensor would have shown in `physics.contacts`, `physics.info`
  and the trigger events the serve sessions print, so a scene zone carries its own size and
  physics never sees it. A level node's `verb` hint still gets a trigger box (unless it names its
  own `col`), decided in `parseHints` before the scene's `col` default; containment reads the box
  either way, never the sensor.
- **`listenerZone` returns `std::string`** (empty: outside every zone), and its listener is
  `AudioListener { position, body }`: the input-driven player's head (the root's pose times the
  head's local position, the camera child with the lowest id), else the first active camera. A
  listener exactly on a face is inside.
- **The reverb is on the bank's first return bus**, stereo: group → `AudioReverbNode` (Freeverb,
  `ohao/audio/reverb_node.*`) → the bus node → master. `loadBank` starts it at the bank's
  `defaultZone` preset; `AudioSystem::setReverb(preset, frames)` crossfades. The new preset goes
  into the quieter of two cores, cleared first; a crossfade costs two cores.
- **Measured RT60s fall 14–18% below the design value** (`Reverb::designRt60`, the combs' feedback
  at DC over their mean delay): outdoor 0.730 s (design 0.887), room 1.658 s (1.966), corridor
  2.338 s (2.731). The damping shortens the high end, and the T20 fit sees it. All within the test's
  20%.
- **`ohao_audio_bank` is its own library** (the parser and the bank types, simdjson, no
  miniaudio), so `ohao_ecs` reads a world's bank without linking the mixer.
- **The schedule installs with zone and occlusion commands only.** `installAudio(world, assets)`
  adds `audio_schedule`, the step's last simulate system. Each step it writes a `zone` command
  (preset, bed, 30-step blend) when the listener's zone changes, then re-casts held voices. Voices
  enter through `AudioSchedule::trackVoice(source, holdMs, occlude)`, the hook step 3's play
  commands use. Nothing plays from the schedule yet, the snapshot hook holds no schedule state, and
  the presets' `bed` stays empty: all three are step 3's.
- **Rays are counted per cast**, 16 a step. A voice past the cap counts as unoccluded and a re-cast
  past it keeps its last answer; both are counted in `stats().raysCapped`. A wall is passed by
  ignoring the hit entity and casting again from the same origin, characters passed the same way,
  4 hops at most.
- **Teardown cuts every link first.** miniaudio 0.11.21's `ma_node_uninit` detaches only the first
  node feeding each input (`ma_node_detach_full` reads `pNext` after the detach cleared it). The
  return, destroyed first, left the other senders linked to its freed group: `audio_bench` faulted
  in teardown about one run in ten. `destroyBuses` detaches every output before any node goes.
- **A sound's `send` is parsed but not yet applied per voice**: a bus sends at one level. Step 3's
  player sets it per voice.

## As built (step 3)

The schedule and the player (plan Task 3, `docs/changes/2026-10-28-audio-step3.md`). What differs
from the design above, and why:

- **The log is read with `commandsSince(since, max, dropped)`** (and `nextSeq()`, `steps()`), a log
  of 4,096; `audioCommands(world)` is the step's own. `describeAudioCommand` prints one line per
  command (`play step 41 enemy_fire#2 weapons prio 220 ... played`), what the tests and the trace
  compare.
- **What a snapshot carries:** the voice table, the next voice id, the round-robin counters, each
  bus's duck target, release and hold, the zone, the snapshot and the schedule's step counter (so a
  restart's step numbers are the round start's). The log's next `seq` is not restored: a replayed
  step writes new `seq`s with the same content, as the design says.
- **A restore resends the mix as `resync` commands.** After a restore (a restart is one) the step
  writes `stop_world` (`resync` unless it is the restart's), then the zone, the snapshot and every
  ducked bus as the restored state has them, marked `resync`, then exactly the commands a recording
  wrote. A player already in that zone or snapshot ignores the command. The design said only that
  the schedule resumes; without the resync the device kept the dead snapshot after a restart.
  Tests compare schedules with `resync` lines left out.
- **The snapshot's blend** is the entered snapshot's `inSteps`, and back to `default` the left
  one's `outSteps` (the design gave only `inSteps`; `default` has none).
- **Ducks hold `holdMs` of the ducking sound** past its start; the deepest active rule a bus has
  wins; a `bus` command goes out when the target changes, over the rule's attack going down and the
  release of the rule that set the level going up.
- **Footsteps:** a character whose `Animator` graph has `foot_l`/`foot_r` events plays its steps
  from them (`footstep_<surface>` from `PhysicsScene::playerInfo(character).surface`, at the entity,
  scaled by its ground speed), and its physics footsteps are skipped; the player keeps physics'.
  The enemy graph's walk and walk_aim clips plant at 0.08 s (`foot_r`) and 0.60 s (`foot_l`), run
  and run_aim at 0.12 s and 0.49 s (each foot's plant, by forward kinematics over the glb).
- **`trackVoice` stays** as the schedule's entry for a voice started outside a rule (tests).
- **The player** (`runtime/audio_player.*`): `LiveConfig::sound` (renamed from `footsteps`);
  `ohao_game --mute` leaves it off, and the schedule still runs. The clock is as designed. A bed is
  its preset's bank sound (`bed_outdoor`, `bed_room`: the ambience bus, `shape: ear`, looped,
  faded in over the zone's blend). `setVoiceOcclusion` ramps a voice's level; its low-pass moves at
  once. A bus sends at the largest `send` of the sounds on it (a per-voice send would need a send
  node per voice); in the slice that sends the player's `fire` at `enemy_fire`'s 0.35 (its own:
  0.15) and the foley at `body_fall`'s 0.15 (the rest: 0.1 or none).
- **`OHAO_AUDIO_TRACE=1`** makes hosts log every command at Info on the `audio` channel
  (`schedule: ` + the line), so `check_live` compares a window's schedule with a headless session's
  through `log.read` without Task 4's `audio.info`.
- **Beds and folder notes:** `tools/audio/make_ambience.py` writes `assets/audio/ambience/outdoor.wav`
  and `room.wav` (8 s seamless loops, 48 kHz, random-phase spectra, seeds 1101 and 1102); OPEN 19
  stays generated.
- **Budgets measured** (`audio_bench`, step 3): `schedule_step_ms` median 0.0006 ms (p95 0.0034),
  `schedule16_step_ms` 0.0005–0.0027 ms (p95 0.013–0.019, 8 rays capped), `submit_frame_ms` 0.0009 ms
  (p95 0.0035), `bank_load_ms` 8.5–11.4 ms; the proposals stand (OPEN 11).

## As built (step 4)

Agents, budgets and the close (plan Task 4, `docs/changes/2026-10-28-audio-step4.md`). What differs
from "The agent-facing surface", and why:

- **`audio.info` is compact.** A bus is `{name, db?, cap?, voices?, mix?}`: `db` (the game's level
  from the snapshot and its duck) only when not 0, `voices` the schedule's held voices only when not
  0, `mix` the session's override. There is no `gain`, `muted` or `solo` of the game's own (the game
  never mutes; overrides are under `mix`). Voices are `{playing, dropped, stolen}`, no `virtual`
  (the schedule has none). A `play` command leaves out variant 0, a gain equal to the bank's volume,
  no occlusion and outcome `played`; `bus`, `prio` and `send` are the bank's and left out. The design's
  shape measured 2,118 bytes mid-fight at `max` 16, over the 2 KB budget; this one 1,903 headless
  (`t29`) and 1,844 in `serve_bench` (`audio_info_bytes`).
- **The device's half** (a window with `--sound`): `sampleRate`, the device's `horizonMs`, `late`,
  `mixMs`, `voices.device`, and per bus `out` (the level the device drives it to, when not the
  game's) and `playing` (its voices sounding). `snapshot.forced` names a forced snapshot.
- **`audio.mix` checks everything before changing anything:** `db`, `mute` or `solo` without a
  `bus`, or a `bus` alone, is `E_COMMAND`; an unknown bus or snapshot `E_NOT_FOUND` at its pointer;
  `clear` applies first. `snapshot: ""` gives the schedule's back. The session hands the whole set
  to `Host::setAudioMix`; the player re-applies it on a bank load. It is the session's: a
  `world.restore` keeps it.
- **`render.stats.audio.mixMs`** is the median over the newest 512 device periods of the engine's
  read in the device callback (a callback of ours around `ma_engine_read_pcm_frames`), scaled to
  10 ms of audio; `voices` the device's sounding voices, `late` the commands played late.
- **`ohao serve --window --sound`** opens a device (a serve window was mute before; `ohao_game`
  always had sound unless `--mute`).
- **OPEN 16, `tools/list`:** 24,137 bytes before (36 tools); 25,463 with the two methods; 24,452
  after trimming nine methods' descriptions (`render.capture`, `render.settings`, `render.stats`,
  `input.inject`, `input.record`, `input.replay`, `world.diff`, `player.info`, `world.summary`).
  The cap stays 24,576 (24 KB); not raised.
- **The catalogue's two methods** move `schemaHash` (9806972b70f2e94c → 0480e4b562132cde) in every
  serve and CLI expectation; nothing else in them changed.
- **The outdoor ground (ruling 21, taken).** Frank's 80 × 80 m `Ground` pad (6a14fa25) had its top
  level with the Floor; its top is now 5 cm below it (`tools/level/blockout_room.py`: a box from
  −0.25 to −0.05 m, rebuilt byte for byte with Blender 5.2.2). At −0.02 m a crate resting 5 mm
  into the Floor would still have been inside Jolt's 2 cm speculative contact distance of it.
  Inside the room only the Floor is touched again (the crates' contacts, rays down, footstep
  surfaces as before the pad); out of the south doorway the player steps down onto Ground and
  stands (`t11_walks_out_the_doorway_onto_the_ground`). What one more node and one more static box
  move was re-baselined: counts (18 nodes, 14 meshes, 164 triangles, 13 boxes, 12 static
  bodies), the ids of the nodes after `Ground` in the exporter's name order, `world.hash` and
  snapshot sizes (every entity is hashed), and the three blockout goldens (RT GI rays leaving
  through the doorway now meet the ground: faces turned to it take a little less bounce light,
  max_abs 4 in the room, 20 on the gun). No simulation result moved.
- **The gate** (`check.py --all`, 2026-10-28, after the pad fix): 51 of 55. Every test, golden
  and doc check passes, `audio_bench` and `serve_bench` included (`tools_list_bytes` 24,452,
  `audio_info_bytes` 1,844); `check_live` 75/75 with validation on, the sound window included;
  `check_validation` clean; 30 of 30 goldens exact. The four that did not pass are benches on a
  machine never quiet that day (Frank's Grok Bot and its webview at 1.1–1.8 cores; `wait_quiet`
  gave up after 15 minutes): `scene_bench`, `physics_bench` and `render_bench` noisy on the run
  and its reruns (CPU calibration ×2.0–3.0, GPU ×1.2–1.3), no verdict; `feel_bench`
  `look_latency_ms` 31.5 ms in the gate's run, with every frame GPU-bound at 15.3 ms (GPU
  calibration ×1.29 after the benches before it), then 8.93 ms (≤ 15) on its rerun, flagged noisy.
  Audio's numbers on a noisy rerun (calibration ×2.03) still sit under every budget:
  `mix_10ms_ms` 0.119, `reverb_10ms_ms` 0.017, `schedule_step_ms` 0.0014, `schedule16_step_ms`
  0.0011, `submit_frame_ms` 0.002, `bank_load_ms` 17.1 (step 3's quiet run: 0.0625, 0.009,
  0.0006, 0.0005, 0.001, 9.28). `frame_p99_over_median` 2.41 against 1.5 on the rerun is SP9's
  open pacing gate (SP9 ruling 21), reported only. The three noisy benches want a quiet rerun.

## Rulings for Frank

PROVISIONAL 1–6 and OPEN 7–20 still await Frank, each with its recommendation below; 21 is taken.

PROVISIONAL (recommended by the post-SP7 roadmap; Frank deferred them to this row; the spec is
designed to them):
1. **The audio stack: stay on miniaudio for 11** (in the tree, public domain or MIT-0; buses,
   sample-accurate starts, custom nodes and deviceless rendering are all in its core). Steam Audio
   (Apache-2.0: HRTF, geometric occlusion, baked reverb), FMOD and Wwise (commercial, not
   redistributable in an MIT repo) are out of 11. The code shows no hard blocker.
   - **OPEN within it:** HRTF and Steam Audio for a later row. Recommended: revisit when the slice
     needs headphone localisation beyond panning, as a separate spatializer behind
     `AudioSystem`, not a new mixer.
2. **Determinism:** the voice schedule replays bit for bit from the step count and snapshots; the
   device's sample timing is presentation. The schedule is authoritative and tested without a
   device.
3. **Content:** committed sounds are generated (a `tools/audio/` script) or CC0, each folder with
   its licence and source notes. Real recordings (Somnabulate's included) live only under
   git-ignored `assets/local/audio/`, for listening, never committed. Local folder first, then
   committed, for every bank sound.
4. **Build on Feel:** its voice cap, curves and occlusion grow into buses, ducking, snapshots,
   priority, zones, scheduling, agents and a budget. They are not rewritten from scratch.
5. **Out of scope:** a music system, a dialogue pipeline, HRTF (ruling 1), a from-scratch occlusion
   rewrite, the HDRI flip, pacing, VFX.
6. **Order:**
   - (a) the mixer graph;
   - (b) reverb zones and occlusion;
   - (c) the schedule from step and animation events;
   - (d) agents, budgets and the close.

OPEN (judgment calls; recommendations given):
7. **The horizon: one step (16.7 ms) for every sound.** Sounds keep their steps' spacing at the
   cost of 16.7 ms more latency. Alternative: the player's own `ear` sounds at horizon 0.
   Recommended: one step for all, judged in U8 and U1. Move the player's own to 0 if Frank hears
   the delay.
8. **Ducking values** (the player's fire: ambience −6 dB, foley −3 dB, release 0.25 s; hurt:
   world −4 dB, 0.3 s). Recommended: first guesses, tuned by ear (U4).
9. **Snapshots: `default`, `dead`, `won` only.** No ADS snapshot, no menu. Snapshots set levels
   and filters, never voice choices. Recommended: an ADS "focus" mix is tempting but is taste;
   add it if Frank asks in U9.
10. **The reverb: our own Freeverb-style node** (8 combs and 4 allpasses a channel, Jezar's
   public-domain algorithm), one send bus following the listener's zone. Not miniaudio's extras
   reverb, whose library sits outside the core header. Recommended: about 200 lines, ours,
   deterministic and testable; convolution waits for a real level that needs it.
11. **Bus layout, caps and priorities** (the `.audio.json` example), and the budget numbers in the
   table: first proposals, to be confirmed from Tasks 1–3's measurements before they gate.
12. **Graded occlusion:** 0, 1, 2 or more walls → none, −8 dB and 1.2 kHz, −14 dB and 700 Hz;
   re-cast every 6 steps; at most 16 rays a step. Recommended: count walls rather than measure
   thickness, until materials carry acoustics (15).
13. **`holdMs` is declared in the bank,** not read from the file. The schedule never depends on
   which file a machine has. Recommended: a local recording longer than its hold may be stolen
   early; that is the price of a machine-independent schedule.
14. **One `.audio.json` per game, named by a saved singleton `AudioMix`.** The bank, buses, ducks,
   snapshots and reverb presets live in one data file, validated with JSON pointers like
   `.animgraph.json`. Recommended: mix data is authored together and reviewed together.
15. **Enemies' footsteps from clip events** (`foot_l`, `foot_r` added to the enemy graph's
   locomotion clips), closing SP10's OPEN ruling 17. The player keeps physics' distance
   footsteps (no legs in first person). Recommended: feet that land when the sound plays are the
   cheapest realism win in the slice.
16. **`tools/list` headroom:** `audio_info` and `audio_mix` do not fit in the 439 bytes left under
    24 KB. Recommended: trim every tool's description first; if that is not enough, raise the cap
    to 28 KB, as SP10's ruling 13 already proposed.
17. **The play scene's zones live in the scene file,** not a rebuilt level. The blockout's level
    file stays byte for byte (its Blender rebuild test). The `verb` hint is supported and tested on
    a fixture for later levels. Recommended: authoring zones in the scene keeps the level
    pipeline's byte-for-byte check untouched.
18. **No rendered-audio goldens.** Deviceless renders are compared run to run and measured (RMS,
    band energy, onset, RT60), never against a committed WAV. Recommended: a committed WAV golden
    would move with every bank tweak and teach nothing that the measured properties do not.
19. **Content beyond generated:** keep the generated weapons and footsteps; add generated
    ambience beds (`tools/audio/make_ambience.py`); allow CC0 replacements (for example
    Freesound's CC0 filter, Kenney), each with `LICENSE.md` and `README.upstream.md`. Royalty-free
    packs that are not CC0 (Sonniss GDC bundles and the like) stay in `assets/local/` only.
    Recommended: generated first, CC0 where it clearly beats them by ear.
20. **The stack question for later rows:** if a later row wants HRTF or baked acoustics, Steam
    Audio (Apache-2.0) behind `AudioSystem` is the candidate. FMOD and Wwise stay out of the MIT
    repo. Recommended: write it into row 11's successor when a need appears, not now.
21. **The outdoor ground pad (6a14fa25) and the expectations it moved.** TAKEN by Frank
    (2026-10-28): its top a few centimetres below the Floor, then re-baseline what it legitimately
    moves. As built: the top at −0.05 m (see "As built (step 4)"); with it level with the Floor a
    crate had landed on both, a ray down inside the room had hit `Ground`, and an edited Floor
    surface had not been what the footsteps reported.
22. **Player hurt SFX removed for mix clarity** (Frank, 2026-10-28 hand test). RULED. An enemy's
    round is hitscan, so when it hit, the player's `hurt` (at the ear, 0.85) and its `world` duck
    (−4 dB, held 300 ms, released over 18 steps) landed in the same step as the `enemy_fire` they
    answered: in the step 4 listen render all 22 enemy shots coincided with a `hurt`. The schedule
    no longer names a sound for damage (`combatSoundNames`); the bank's `hurt` sound and its duck
    rule are gone, and `assets/audio/weapons/hurt.wav` with its generator. The impacts of the
    player's rounds, the enemies' `death` and `body_fall` stay; the HUD's red vignette still marks
    a hit. Ruling 8's hurt duck is void with it.
