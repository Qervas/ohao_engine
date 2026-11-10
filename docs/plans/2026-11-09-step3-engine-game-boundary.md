# Step 3: the engine/game boundary

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.3, 4.4 and 5 (step 3). **Status:** 3a to 3j built (3g merged into 3c); 3k planned.

The slice (combat, the grenade, rounds, enemy AI, the arms and enemy drivers, the HUD, the view model) leaves the
engine libraries for `games/slice`, behind a game API that a second game also builds against.

The engine is refactored in place before anything moves, so each move carries code already in its final shape. There
are eleven parts, each its own commit with the hooks green.

## Where it starts

These are the facts the parts change, from a survey of the tree at `a98d34e2`.

- **Game code inside engine libraries.**
  - The game files are `ohao_ecs`'s `combat.*`, `combat_hud.*`, `enemy_ai.*`, `enemy_anim.*`, `arms_anim.*`,
    `view_model.*` and `round_hook.hpp`.
  - The 9 game components: Health, Weapon, WeaponState, Muzzle, ViewModel, WeaponTrigger, Enemy, EnemyState and
    Round. They are registered in `scene_types.cpp`.
  - The 5 game methods: `player.info`, `player.teleport`, `combat.info`, `combat.events` and `combat.damage`. The
    spec says 8; only 5 ever existed.
- **The engine reads the game.**
  - `installEngine` installs combat.
  - Animation hard-wires the two drivers.
  - The audio schedule maps combat events to sounds (`fire`/`enemy_fire`, `impact_<surface>`, `empty` then
    `reload`), and sets the `dead`/`won` mix from the round.
  - The vfx system builds tracers from shots and hits, and `vfx_set` accepts only `shot`, `hit` and `explode`.
  - `vfx_draw` draws the grenades.
  - The player controller stops a dead player (it reads `Health`).
  - `render_extract` flags ViewModel subtrees.
  - The hosts call `combatHud`.
  - The rule "the player is the input-driven PlayerController with the lowest id" is written out 7 times.
- **State outside the snapshot and the hash.**
  - Combat's grenades, cooldown, round phase, timer, round-start snapshot, counters and log.
  - The World's `InputMap`.
  - The step counters and logs of physics, animation and audio.
  - Hook states are snapshotted but not hashed.
  - Entity references inside snapshot columns (`Joint::other`, vfx records) are raw `entt::entity` values with no
    remap, so a restore that recreates the referenced entity leaves them stale.
- **A restart in mid-step.** A round restart restores the whole world inside combat's step. Seven consumers then
  skip the events before the step's last `Restart`: the arms and enemy drivers, vfx, the audio schedule, the view
  model, the HUD and the audio player.
- **60 Hz assumed.**
  - Animation ignores `dt`: `kStepSeconds = 1/60`, and fades are counted in steps.
  - The audio schedule ignores `dt`: `holdSteps`, blends in steps, `kStepMs`, and `AudioPlayer`'s `rate/60`.
  - Vfx's and combat's `dt` are not in their saved state.
- **One input stream** per World; two input-driven players would move alike.
- **Systems are ordered by a fixed name list** (`ecs/system_order.hpp`) that names game systems (`combat`,
  `view_model`). Nothing can remove a system.
- **`ohao_ecs` links `ohao_renderer` publicly.**
  - `components.hpp` includes `render/scene/handles.hpp`.
  - Instantiate, `render_extract`, `vfx_draw`, spawn, framing and the HUD use the renderer.
- **The libraries are all static,** with no PIC, no symbol visibility settings and no dynamic loading.

`replay_test` compares two runs of one build, so a refactor that changes the fight passes it. The **behaviour
baseline** catches that. `tests/replay/step3_baseline.py` (temporary, deleted when step 3 closes) plays
`replay_test`'s script and writes, at each checkpoint:
- the canonical scene;
- `world.hash`;
- every log: combat, physics, the audio commands and the vfx records;
- the player.

It was recorded at `a98d34e2` (`build/step3_baseline/base.json`); two runs are identical. A part that only moves
code must leave it unchanged. A part that changes behaviour or the shape of a log says what changed, and the
recording is redone after that part.

## 3a. The world without the renderer

- **A new library, `ohao_world_render`.** It takes the ECS files that use the renderer: instantiate, render extract,
  vfx draw, framing, spawn's model upload and the HUD's list.
- **The handles leave the renderer.** `render::MeshHandle` and `MaterialHandle` are plain ids; their header moves
  out of the renderer, so a component can hold one without the world linking it.
- **The layers.** `ohao_ecs` and `ohao_renderer` become one level, so the layer check forbids the edge, and the
  listed exception is deleted.
- **Vulkan through `ohao_asset`.** Whatever `ohao_ecs` still reaches of Vulkan through `ohao_asset` is measured and
  cut, or left with its reason.

*Done when* `ohao_ecs` links neither `ohao_renderer` nor `ohao_gpu_vulkan`, the layer check holds it, and the hooks
and the baseline are unchanged.

*As built (2026-11-09):*
- **The library.** `ohao/world_render/` holds instantiate, `render_extract`, `vfx_draw`, framing, `combat_hud`
  and `model_spawn`: the renderer half of `spawn`. `spawnLight` stays in the world.
- **Kept in the world:** `primitiveModel`, which physics fits colliders to (`ecs/primitive_model.*`), and
  `RenderSlot`, now in `components.hpp`.
- **The handles** are `core/render_handles.hpp`.
- **The layers.** The renderer moved to level 4, beside the world; the exception is gone. A planted
  `render/scene/render_scene.hpp` include in `ecs/spin_system.cpp` fails configure, naming the file.
- **Vulkan through `ohao_asset`** was the `Vertex` input descriptions, which now live in
  `render/scene/vertex_input.*`. `ohao_asset` links no Vulkan.
- **What the cut exposed.** The asset loaders called `stb_image` without defining it: `model_fbx.cpp` defined a
  private copy, and the shared one came from `ohao_gpu_vulkan` through the old link. There is now one
  definition, `external/stb_image.cpp` (`ohao_stb_image`), that both libraries link.
- **Proof.** `level_bench` (the world, the API, levels, physics) now links no renderer, no GPU library and no
  Vulkan. The gates pass, the goldens are exact, and the baseline is the same.

## 3b. Complete state

- **Hooks are hashed.** A snapshot hook gains `hash`, and `world.hash` covers every hook's state.
- **Combat registers its state:** grenades, cooldown, the round's phase, timer and start snapshot, its counters.
  Vfx's and combat's `dt` join their states. The physics, animation and audio step counters join theirs.
- **The World's input is state:** held keys, axes and the virtual device are saved, restored and hashed.
- **Snapshot columns store references as EntityIds,** remapped on restore, as the hash already does.
- **The replay gate restores the hard case.** `replay_test`'s midway snapshot moves to a point where a grenade is in
  flight and a key is held. Today that case breaks a restore.

*Done when* `replay_test` passes from the new midway point. The baseline's scenes and logs are unchanged; its hashes
are not, because the hash now covers more.

*As built (2026-11-09):*
- **Hooks are hashed.** `SnapshotHook` gains `hash`, fed through `ecs::StateHash`, an interface
  `world_hash.cpp` implements so systems need no reflection. Hooks for `combat` and `animation` (its step
  count) are added. Physics hashes its pending restore, if one waits for the next sync.
- **The snapshot** keeps each row's handle, and a restore recreates a missing entity with it
  (`createWithId(..., hint)`): references held as handles (`Joint::other`, systems' states) stay valid, with no
  remap.
- **The input.** The snapshot carries the World's `InputMap`. `world.restore` puts it back; a round's restart
  does not, since the player's keys are where their hands are (`RestoreInput::No`).
- **The restart keeps its history.** The restore inside it would now rewind combat's own state, so it keeps the
  round's start snapshot, the step count and the restart count.
- **Logs.** A log's seq is history: a restore never rewinds a log. So neither the counters nor the references
  into a log are hashed: an audio voice's command seq, which only breaks ties in an order a restore keeps, and
  a vfx record's combat seq. The first try hashed them and failed the replay.
- **The gate.** `replay_test` takes its midway snapshot with a grenade in flight, a strafe and the aim held.
  Planted, each caught: combat's hook restoring nothing, and a restore without the input. Each misses the
  midway hash, and the replay drifts.
- **The baseline.** The same 39 commands, the same scenes and logs; only the hashes differ.
  `step3_baseline.py` keeps its own copy of the original fight.

## 3c. One event log

Revised 2026-11-09, from a survey of the tree at `518f93fc`.

**Where it starts.**
- **Four logs.** Combat, physics, animation and the audio schedule's commands each keep their own:
  - their own ring (1,024 events, 1,024, 1,024 and 4,096 commands);
  - their own cursor rule, numbering from 0 or from 1;
  - their own `step`, each system's own counter.
- **Readers bound to one struct.** Every reader is written against one system's struct: four in the step (the two
  drivers, vfx, the audio schedule), three observers (the view model, the HUD, the audio player), four API methods,
  three benches and `replay_test`.
- **One struct for ten kinds.** `CombatEvent` holds ten kinds in one struct. Its `point` is the eye for a shot and
  the hit for a hit; its `amount` is a cone, a distance, hit points or ammo.
- **Lost events.** An event emitted between steps never reaches the step's readers. Combat clears its list when
  its next step starts. So a kill through `combat.damage` plays no death sound, and the enemy's animation driver
  never sees the death.

**Prior art, and what is taken from it.**
- **Bevy's `Events<T>`.**
  - How it works:
    - events are buffered across two updates;
    - each reader keeps its own cursor;
    - in 0.17 they are renamed `Message`, beside observers that run at once.
  - The cursor per reader is there because Bevy's schedule may run a reader before or after its writer.
  - Ours is ordered: today by the list, from 3h by the declared reads and writes, checked at install. So a step's
    events are read in that step, and a reader keeps no state.
  - Taken: the typed channel, and the reader and writer handles a system holds. Those handles also tell 3h what
    each system emits and reads, so there is no second list to drift from the code.
- **Photon Quantum's frame events.**
  - The view reads them after the frame.
  - An event is identified by its tick, its type and its content, so an event simulated again after a rollback is
    the same event.
  - Taken: here an event's identity is its step, its name, and its place among that step's events of that name. A
    restore and a replay keep it. Its `seq` is only its place in the history.
- **Not taken:**
  - Immediate observers: a callback inside an emitter's loop is reentrant, and the order then depends on who
    subscribed.
  - EnTT's dispatcher: it has no history, no order across types and no snapshot.

**The design.**
- **The boundary** (Frank, 2026-11-09: "combat is only for games with combat; games without it won't use it"):
  - Engine events (physics, animation, the audio commands, and `world.*` from 3d) are read by type, by any system.
  - A game's events reach the engine only as a **cue**: `at`, `to`, `normal`, `source`, `target`, `tag` and `n`,
    projected by a function the game registers with the channel (compiled, no reflection in the step).
  - The engine reacts to a cue through a binding in its data, by channel name: `{"on": "slice.shot", "play":
    "fire"}` in the audio bank, `{"on": "slice.hit", "spawn": "impact_{tag}"}` in the vfx set.
  - An engine system never includes a game's event type, and names no game channel in its code.
  - A world without a game installs no game system: `installEngine` installs only the engine.
- **The World's event log** (`ecs/events.hpp`), beside the input.
  - A channel is a name and a payload type: `world.events().writer<Shot>("slice.shot", "combat")` and
    `.reader<Shot>("slice.shot", "vfx")`.
  - The handles name their system, which 3h reads as its emits and reads.
  - A name taken with a second payload type throws.
- **Payloads** are plain reflected structs (`TypeRole::Event`), registered like components, with docs and units.
  They hold entities by id, since an event outlives what it names.
- **The World's step.**
  - The World counts its steps (`World::steps()`, saved and hashed).
  - Every event is stamped with the step that reads it.
  - The systems' own counters stay until 3e.
- **What a step reads.**
  - A reader takes its channel's events for this step: `read()` returns a span, oldest first.
  - An event emitted between steps belongs to the next step. Examples: `combat.damage`, and physics ending the
    contacts of a removed body.
  - A channel's writers run before its readers. Today's order does this; 3h checks it.
  - A system that needs an event emitted later in the previous step reads it with an explicit one-step delay. This
    is added when a system needs it; none does.
  - Step 4, with jobs: two writers of one channel in one wave each write their own buffer. The buffers merge at the
    wave's end in the wave's fixed order, so the order stays the serial one.
- **State, and restores.**
  - The events a step will read are state. A snapshot taken between steps carries the next step's events. A
    snapshot taken during a step (the round's start, until 3d) carries none.
  - `world.hash` covers those events and the World's step.
  - A restore drops the next step's events and puts the snapshot's back.
  - A restore during a step (the round's, until 3d) ends that step's events. What the step had emitted was undone
    with the world, so the systems after it read only what follows.
  - So the four readers in the step lose their "skip what came before the last restart" rule here.
- **History.**
  - Every event also enters one history of the last 8,192 (the four logs held 7,168 together).
  - It is ordered by `seq`, counted from 1, with no gaps across channels.
  - It is never rewound, hashed or snapshotted.
  - Observers read it by cursor: the HUD, the view model and the audio player each keep a `seq`.
  - Events leave the history only between steps, and never one a step has yet to read.
- **Cost.**
  - An emit is two appends; a read is a span.
  - Nothing allocates per step once the vectors have grown.
  - No reflection runs in a step: reflection formats an event for the API and hashes the next step's events for
    `world.hash`, both outside the step.

**What moves onto it.**
- **Combat**, the slice's ten channels, each with its cue:
  - The channels: `slice.shot`, `slice.hit`, `slice.damage`, `slice.death`, `slice.reload`, `slice.reloaded`,
    `slice.round_start`, `slice.restart`, `slice.win` and `slice.explode`.
  - Each has only its own fields, under names that say what they are.
  - A shot carries where its round stopped (its cue's `to`), so vfx no longer pairs a shot with the hit that
    follows it.
  - Its drivers (arms, enemy) are the game's, so they read these channels by type.
- **Physics:** `physics.contact_begin`, `physics.contact_end`, `physics.trigger_enter`, `physics.trigger_exit`,
  `physics.footstep`, `physics.land` and `physics.joint_break`. They keep today's sorted order.
- **Animation:** `anim.clip_event`.
- **The audio schedule's commands:**
  - The channels: `audio.play`, `audio.stop`, `audio.bus`, `audio.snapshot`, `audio.zone`, `audio.occlusion` and
    `audio.stop_world`.
  - Sounds and buses are named, not indexed.
  - The audio player reads them by cursor, like any observer.
  - A voice's tie-break by its play's `seq` stays. A restore keeps the order of seqs (3b), and one sequence for
    every channel keeps it too.
- **Vfx records stay vfx's state:** they live and expire, so they are not events.
  - A record's `sourceSeq` is the log's seq.
  - Its seed comes from the event's identity, not from the event's place in combat's list.
- **Deleted:**
  - `CombatEvent` and combat's log;
  - the logs of physics, animation and the audio schedule;
  - every `stepEvents`, `eventsSince`, `nextEventSeq` and `commandsSince`;
  - `combatEvents()` and `physicsEvents()`;
  - the cursor loops written against them.

**The API.**
- **`events.read {since = 0, max = 32, names?}`** returns `{events: [{seq, step, event, ...fields}], next, dropped?}`:
  - the events with seq > since, oldest first;
  - `names`: exact names or `prefix.*`. An unknown exact name is `E_NOT_FOUND`, which lists the channels;
  - `max` 0–64, where 0 returns only `next`, a cursor at the present;
  - `dropped`: how many events of any name the history no longer holds.
- **Fields are written from the payload's reflection.**
  - An entity is written as its id, and as its name while it lives.
  - A float is rounded to its unit's step: 1 mm, 0.01°, and so on.
- **Deleted:** `combat.events`, `physics.events`, `animation.info`'s `since`, `max` and `events`, and `audio.info`'s
  `since`, `max` and `commands`.
- **The catalogue** (`schema.get`) lists every channel: its name, the system that writes it, the systems that read
  it, and each field's doc and unit.

**The budget.** `serve_bench`'s `audio_info_bytes` (at most 2 KB) measured `audio.info` with 16 commands; after 3c
it measures `audio.info` alone. `events.read {max: 16}` mid-fight is held to the same bar, as `events_read_bytes`
(Frank's ruling, 2026-11-09).

**Combat leaves the engine's code paths** (the old 3g, here so the readers are written once):
- **The audio schedule** plays game channels from bank bindings. The rules that are code today become data:
  - fire or enemy_fire, by `"by": "player"`: the cue's source is the listener's player;
  - impact by surface: `"play": "impact_{tag}"`, falling back to `impact_default`;
  - empty and reload: `slice.reload` tags a reload from empty;
  - death and explode.
- **The round's mix** (`dead`, `won`): the game requests it; the schedule no longer reads `Round` or `Health`.
- **Vfx** spawns from set bindings:
  - the set's `shot`, `hit` and `explode` sections become `on` bindings, its version migrating the files;
  - a tracer runs from a cue's `at` to its `to`;
  - its cadence (`every`) counts on the cue's `n`, the shooter's shot count, in place of reading `WeaponState`.
- **Engine readers stay typed.** Physics footsteps and animation clip events stay typed reads in the audio
  schedule (engine reading engine).
- **The install.** `installEngine` drops `installCombat`, and animation no longer registers the drivers.
  `installSlice` installs combat, the view model and the two drivers (still in `ohao_ecs` until 3i moves it). Every
  host that plays the slice calls it.

**Proof.**
- **No game, no combat.** A session that never calls `installSlice` loads Cornell and the play scene, steps, and
  hashes. Its catalogue lists no `slice.*` channel.
- **The baseline.**
  - Its logs read the same through `events.read`; the tool maps each old event to its new name and fields.
  - The expected differences:
    - seqs renumber into one sequence;
    - every `step` is the World's;
    - vfx seeds change.
  - A kill through `combat.damage` now reaches the next step's readers, as a round's kill does. The baseline has
    none.
- **`replay_test`** passes, reading the fight through `events.read`.
- **A temporary check**, deleted after:
  - The sequence: `combat.damage` between steps, a snapshot, a step, the restore, then the step again.
  - It passes when both runs reach the same `world.hash`, and the step's readers saw the damage.
  - Planted: a snapshot without the next step's events. The check must fail.
- **Measured, not gated:** physics_bench's `eventsMs`, and the play fight's combat, vfx and audio step times, before
  and after.

*Done when* every event goes through the log, the four logs and their cursor loops are deleted, no engine system
names a game event or component, a world without the slice runs, and the proofs hold.

Two commits, each through the hook:
1. **The engine's events.**
   - The log, the World's step, and the channels of physics, animation and the audio commands, with their readers
     and observers.
   - `events.read` for them, replacing `physics.events` and the event and command halves of `animation.info` and
     `audio.info`.
   - Combat keeps its own log for this one commit.
2. **The game's events.**
   - Combat's channels and cues.
   - The audio bank's and vfx set's bindings, and the round's mix as a request.
   - `installSlice`.
   - `combat.events` deleted.
   - Done when no engine system names a game event or component (`audio_schedule`, `vfx`, `vfx_set`), and the
     baseline's audio commands and vfx records are unchanged apart from the seeds.

*As built, commit 1 (2026-11-09):*
- **The log** is `ecs/events.hpp`: `EventChannel<T>` (one vector of `{meta, value}`, contiguous, its front
  compacted as the history drops it), `EventWriter`/`EventReader` handles naming their system, and `EventLog`
  (the channels, one deque of channel indices by seq, the window's first seq). `World::simulate` brackets the
  systems with `beginStep`/`endStep`; `World::steps()` is the log's step.
- **Payloads** are reflected in `ecs/event_types.cpp` (`TypeRole::Event`, `.entity()` on an id field, the new
  `dB` unit). The type key moved to `core/type_key.hpp`, so a channel names its payload type without `reflect/`.
- **Physics** keeps its drain, collapse and sort; publishing emits into seven channels. A removed body's ends
  are emitted when the sync removes it, not held for the next step, so between steps they are the next step's
  events and a snapshot carries them.
- **The audio schedule's commands** are seven payload types, sounds and buses by name. A play carries only
  what the bank does not say: `pos` is absent at the ear, its bus, priority and send are its sound's. The
  player resolves the names; the API needs no bank. Deleted with the log: `AudioCommand`, the trace
  (`OHAO_AUDIO_TRACE`, for the deleted `check_live`), and `AudioSchedule::play`/`trackVoice` (no callers since
  2026-11-08).
- **The API's channel key is `event`**, not `name`: a payload may have a field `name` (`anim.clip_event`).
- **The catalogue** gains `events` (each channel's name, payload type, writers and readers), in the session's
  and in `ohao schema`'s (a world with the engine installed); `schemaHash` covers it.
- **The baseline.** Through `events.read`, the physics events and audio commands read the same as the
  recording made before step 3 (by meaning: seqs left out, floats within the old rounding). Combat, vfx, the
  scenes and the player are equal; only the hashes differ (the World's step is hashed).
- **The next step's events are state** (a temporary check, deleted): CrateB destroyed between steps ends its
  contact into step 31's events; a snapshot, a step, the restore: the same `world.hash`, then the same after a
  step, and the history holds the end twice. Planted, a snapshot without them: the restore's hash differs.
- **Cost**, measured before and after on this machine (median, ms): 1,000 contact begins in a step 0.1186 and
  0.1197 (p95 0.1256 and 0.1214), the blockout step 0.0084 and 0.0085, combat 0.0021 and 0.0021, the audio
  schedule 0.0005 and 0.0005, the audio player's frame 0.0007 and 0.0007.
- **Sizes**, the play scene mid-fight: `events.read` of its last 16 events is 1,832 bytes; `audio.info` is 465.
  `serve_bench` (Windows only) measures both; `events_read_bytes` is gated at 2 KB (Frank's ruling, 2026-11-09).

*As built, commit 2 (2026-11-09):*
- **Cues.** `ecs::Cue` (`at`, `to`, `dir`, `normal`, `source`, `target`, `tag`, `n`) is what a channel's
  projection (`EventChannel::setCue`, a function pointer the game registers) makes of a payload. `dir`, a
  round's direction, joined the plan's fields: vfx needs it for a decal on a character. An engine reader takes
  a channel by name (`EventLog::addReader`, so the catalogue names it) and walks the step's events of every
  bound channel in seq order (`visitWindow`), so sounds and records keep the order the events happened in.
- **The slice's ten channels** carry typed payloads (`ShotEvent` with `round` and the `hit` where it
  stopped, `HitEvent` with `dir`, ...); their reflection is in `ecs/event_types.cpp` until 3i moves them.
- **The bank's `events`** (`audio/audio_bank.hpp`, `EventSound`): `on`, `by` (`player`/`other`), `tag`,
  `play` (one or a list, `{tag}`), `fallback`, `at` (`ear`/`event`), `owner` (`source`/`none`). The first
  binding that holds plays. `slice.audio.json` gains the seven bindings that were `combatSoundNames` and
  `combatSoundAtEar`, both deleted.
- **The set's `events`** are keyed by channel name. `{surface}` became `{tag}`, and `kMuzzleAhead` became the
  binding's `ahead` (`slice.vfx.json`'s shot: 0.5). The default set binds nothing. One spawn path from a cue
  replaces `onShot`, `onHit` and `onExplode`. The tracer cadence counts on the cue's `n` instead of reading
  `WeaponState`. Seeds come from the event's identity.
- **The mix.** Combat calls `requestMixSnapshot` each step; the schedule reads a `MixRequest`, no longer
  `Round`. The schedule's restart test went: any restore (a restart's too) writes `stop_world`, whose
  `resync` flag went with the test (the player never read it).
- **`installSlice`** (`ecs/slice.hpp`): combat, the view model and the two drivers (moved out of
  `installAnimation`). `installEngine` installs no game system. `ohao serve` and `ohao run` take
  `--game slice|none`; `LiveConfig::slice`.
- **Deleted:** `CombatEvent` and combat's log, `combatEvents()`, `combatEventKindName`, `combat.events` and its
  params, the drivers', vfx's and the schedule's restart rules.
- **The baseline.** Every log reads the same as the recording made before step 3: combat 106 events at the
  last checkpoint, the audio commands 94 (now from the bank's bindings), the vfx records 12 (from cues),
  physics, scenes and the player. Only the hashes differ. Planted, the bank's `by` ignored: 94 audio commands
  become 107, and every checkpoint differs.
- **The temporary checks**, deleted after:
  - `combat.damage` at step 30 makes step 31's events, and that step's schedule plays `death`. A snapshot,
    the step, and the restore reach the same `world.hash`, before and after the step.
  - `ohao serve --game none` loads Cornell and the play scene, steps 60 and hashes. Its catalogue lists 15
    channels, none `slice.*`, and `combat.info` is `E_STATE`.
- **Left for 3i:** the audio schedule's `characterOf` still tests `Enemy`, a game component. It is the one game
  name left in `audio_schedule`, `vfx` and `vfx_set`. A dead enemy loses its `PlayerController` (`die()` in
  `enemy_ai.cpp`), so `Enemy` is what still marks its body for occlusion rays and foot sounds. 3i's control flag
  on `PlayerController` replaces it.
- **Found:** a restore whose bodies changed since the snapshot, while bodies still move, is not exact.
  - The case: the kill above at step 1, when the crates are still settling. The dead enemy loses its
    character in the next step; on restore, physics takes its components' path (teleports at rest,
    `BodyMotion`), and the crates come out micrometres off.
  - It is the same on 3c.1, where the kill's events were not yet in the log. A snapshot with the bodies
    asleep (step 30) replays exactly.
  - It belongs to 3d's physics restore work.
- **Cost** (median, ms; pre-3c, 3c.1, 3c.2): combat step 0.0021, 0.0021, 0.0022; vfx step 0.0002 in all three;
  the audio schedule 0.0005 in all three.
  - The physics-only measures moved about 5% with no physics change in this commit: the 1,000-contact step
    0.1186, 0.1197, then 0.122 to 0.129 over four runs; the blockout step 0.0084, 0.0085, 0.0089 to 0.0091.
  - The calibration loop stayed at 26.3 to 26.6 ms throughout.

## 3d. The round restart as a request

- **A request, carried out between steps.**
  - Combat asks for a restore (`ecs::requestRestore(world, snapshot)`). The World carries it out after the step's
    last simulate system.
  - The round's start is also taken between steps, at the end of the round's first step. No snapshot or restore
    then happens inside a step.
- **The events.**
  - Any restore emits `world.restore` into the next step's events; the game's `slice.restart` follows it.
  - The audio schedule's `restored` flag and its restart test become one read of `world.restore`.
  - The HUD and the view model reset on it.
- **What goes.**
  - The restore during a step, and with it 3c's ending of a step's events by a restore.
  - The observers' "drop what came before a restart" in a frame.
  - Physics' restored-player path, cut down to what a restore between steps needs.

*Done when* no snapshot or restore runs inside a step, and no reader has a restart rule.

- **A temporary restart check:** a death, the restart, and the world equal to the round's start.
- **Measured,** not gated: `physics_bench`'s restart time.
- **Expected change in the baseline:** none, since its fight never restarts.

*As built (2026-11-09), deciding by AGENTS.md's "Deciding":*
- **`World::defer(op)`.**
  - During a step, `op` runs after the step's last system: the step's events are already history, so it runs
    between steps. Between steps it runs at once.
  - This is the deferred-commands pattern (Bevy's `Commands` applied at a sync point, flecs' deferred
    operations), with one sync point a step.
  - `takeSnapshot` and `restoreSnapshot` throw inside a step. The log's ending of a step's events by a restore
    during the step is deleted.
- **Combat** defers both the round's start snapshot (at the end of its first step, as its doc always said) and
  the restart (`restart()`).
- **`world.restore` is history only.** This differs from the plan, which had it in the next step's events.
  - First principles: a restore must leave the world hashing as its snapshot. An event in the next step's
    events is state, so it would break that (`replay_test` checks the restored hash).
  - A restore is a fact for observers, not an input to the simulation. So `restoreSnapshot` emits it
    (`{step}`: the step it went back to) and then ends the window, before the snapshot's events come back. The
    history holds it; no step reads it.
  - The channel exists from the World's construction, so the catalogue lists it.
- **The audio schedule keeps its restore hook's flag.** 3c.2 already deleted its restart test. Its `stop_world`
  is output, and the hook sees every restore, so no event is needed.
- **Observers.** The HUD and the view model reset on `world.restore`, any restore. The view model's "drop the
  rounds before a restart" is deleted.
- **Physics: nothing to cut.** The plan's premise was that it had a restored-player path for restores inside
  a step. It has none: that path serves every restore. What 3d fixed instead is 3c's finding.
  - The physics step synced bodies (and matched a restore's saved simulation to them) before spawning
    players, the opposite of `World::sync`'s order (`player_spawn` before `physics`).
  - So a restore across a death, whose character was parked, failed the identity check and took the inexact
    path.
  - The step now spawns players first. The step-1 kill, snapshot and restore replays exactly; it failed on
    3c.1 and on 3c.2.
- **The temporary restart check** (deleted after):
  - The player, killed between steps 2 and 3, restarts after `restartDelay`.
  - The history ends `world.restore` (`step: 1`) then `slice.restart`.
  - The canonical scene after the restart equals the one saved at the round's start, and the step after it
    equals the round's second step.
  - Planted, the round's start snapshot taken inside the step: the step throws.
- **Unchanged:** the baseline matches 3c.2 exactly, hashes included (its fight never restarts, and its
  players spawn at load). Every gate passes.
- **Cost:** the restart's restore takes 0.0043 ms median (budget 2 ms): 0.0034 before step 3, 0.0040 at 3c.2.
- **`events.read`** writes a non-entity `uint64` field as a number (`world.restore`'s `step`), not a
  16-hex-digit id.

## 3e. One clock

- **One step count.** The World's (3c). The counters of combat, physics, animation, the audio schedule and vfx go;
  each system reads the World's.
- **Animation in seconds.** Clip time and fades are in seconds: graph files carry seconds, migrated by their version.
- **Audio in time.** The audio schedule's holds and blends, the bank's attack, release and in/out times, and the
  audio player's horizon are in milliseconds.
- **No 60 Hz constants.** No code assumes 1/60 except the default clock.

*Done when* the baseline is unchanged at 1/60 s, and the same fight stepped at 1/120 s gives the same animation
and audio timings, measured.

*As built (2026-11-09), deciding by AGENTS.md's "Deciding":*
- **The decision: integer steps in state, time in data.**
  - State keeps counting whole steps: an Animator's steps and fade, a voice's start and end. A duration in
    data (a graph's fade, a bank's hold, a duck, a blend) is seconds or milliseconds, turned into steps of the
    World's dt where it is used.
  - This is how deterministic fixed-step simulations keep time: tick-counted timers, as Photon Quantum's.
    State stays exact and hashed alike, and seconds are the same at any rate.
  - Refused: float seconds in saved state. They would have moved the Animator's format, and their sums can end
    a fade a step off at 60 Hz, which would break the baseline.
- **The World's clock.** `World::stepDt()` is the step's dt (the last one between steps; 1/60 s before any). It
  is the host's configuration, never saved or hashed.
  - The counters of combat, physics, animation, the audio schedule and vfx are deleted, and so is vfx's and
    combat's copy of the dt. Every one reads `World::steps()` and `World::stepDt()`.
  - The `animation` snapshot hook held only its counter, so it is deleted: `kSnapshotOrder` has four entries.
  - Physics keeps a copy of the World's step (a pair's `since`), handed to it by its step, outside the
    snapshot.
- **Animation.** `kStepSeconds` is deleted; `stepLayer`, `stateTime`, `clipEnded` and `evaluatePose` take the
  dt.
  - A graph file is `"version": 2` with fades in seconds. `fadeSteps` rounds a fade to the nearest step, at
    least one.
  - A file without the version is refused, not read: its fades are steps, and read as seconds they would be
    60 times longer.
  - The three graphs moved (step n became n/60 s, 4 decimals), and `tools/anim/make_test_rig.py` writes the
    rig's. `rig.glb` is byte-identical.
- **Audio.** The bank's `attackSteps`/`releaseSteps` and `inSteps`/`outSteps` became `attackMs`/`releaseMs` and
  `inMs`/`outMs`. `slice.audio.json` moved (1 step to 17 ms, 15 to 250, 18 to 300, 30 to 500, 60 to 1000).
  - The schedule's constants are time: a 500 ms zone blend, a 50 ms re-cast ramp and restore fade, and a
    re-cast every 100 ms.
  - Holds round up to whole steps and periods to the nearest, both in double with 1e-6 of slack, so 60 Hz keeps
    its step counts exactly.
  - The commands carry `blendMs` (a new `ms` unit); `audio.info`'s snapshot gives `blendMs`.
  - The player turns milliseconds into frames, and steps into `rate × dt` frames.
- **Physics' 1/60 s** is Jolt's largest substep, a solver limit, now labelled as one.
- **The baseline at 1/60 s:** every log reads the same as before step 3 (the comparison turns `blendMs` back
  into steps); only the hashes differ.
- **At 1/120 s** (a temporary check, deleted after): the player's reload has the same events at 60 Hz and
  120 Hz, each within one 60 Hz step.
  - `reload` plays at 0 s at both rates.
  - `mag_out` (the clip event and its sound): 0.5833 s at 60 Hz, 0.5917 s at 120 Hz.
  - `mag_in`: 1.4833 s and 1.4917 s.
  - `reloaded`: 1.5 s at both.
  - At each rate the clip events land one step before the weapon's reload ends.
  - A whole fight is not comparable across rates: physics integrates differently, and the enemies' decisions
    follow.

## 3f. Input per player

- **One input stream per player.** `World::input(player)` gives each its own; `PlayerController` names its stream.
- **One function finds the player.** A single engine function replaces the seven copies of "the player".
- **The API.** `input.inject` and `input.record` take a `player`, which defaults to the first.

*Done when* a scene with two input players moves them independently (checked once, by hand), and the baseline is
unchanged.

*As built (2026-11-09), deciding by AGENTS.md's "Deciding":*
- **Why now, though singleplayer comes first:**
  - The requirement is the spec's "multiplayer later needs no redesign".
  - The game API (3i) is where input reaches a game. Keyed by player from the start, games written against it
    will not change when a second player arrives.
  - "The player" in seven places was a deletion due anyway.
- **Streams.** `World::input(stream)` holds one `InputMap` per stream, at most `kMaxInputStreams` (8).
  - A stream is made on first use with stream 0's bindings (`InputMap::freshCopy`). A stream never made reads
    as nothing held.
  - `stepInput` sends each event to its `InputEvent::stream` and steps every stream every step.
  - `InputActions` rebinds every stream.
  - The snapshot (`inputs`) and `world.hash` carry them all.
- **`PlayerController.stream`** (0 to 7, `omitDefault`, so files and hashes are unchanged).
  - A character moves by its own stream, and a weapon fires by its holder's.
  - The look lead belongs to stream 0's player: the window's mouse is stream 0.
- **`ecs::playerOf(world, stream = 0)`** (`ecs/player.hpp`) replaces the copies in the listener, the HUD, vfx,
  combat's round and its grenade.
  - The round's `restart` and the grenade read the player's stream.
  - The enemies' target stays their own rule, the first living input player on any stream: it is the game's
    targeting, and it moves with the game in 3i.
- **The API.** `input.inject {player}` and `input.state {player}` select a player's stream (`E_NOT_FOUND` for an
  entity that is no input-driven player). A recording writes `stream` past 0 and its replay reads it. Record and
  replay release every stream.
- **Checked once, by hand:** the play scene's player, duplicated onto stream 1. Stream 1's input walked the copy
  2.667 m while the first stayed put (0.000 m), and stream 0's the other way round.
- **The baseline** matches 3e's, apart from the hashes (which now count the streams). Every gate passes.

## 3g. Event names as data

Merged into 3c's second commit (2026-11-09). There it is written once with the event readers it changes, rather than
those readers being written against combat's types first. Left for 3i: the mix request and the cue as engine
services in `ohao/game`.

## 3h. The declared scheduler, serial

- **Each system declares itself:**
  - its phase;
  - the components it reads and writes;
  - the events it emits and reads, taken from the writers and readers it holds (3c);
  - any explicit `after`.
- **The order is computed.** It comes from the events (an emitter runs before its readers in a step) and the
  explicit `after`s, and it is checked at install: a cycle, or two systems that write the same component, or one
  writes what the other reads, with no order between them, fails there.
- **Waves.** Systems that do not conflict share a wave, and the waves run serially in a fixed order.
- **What goes.** `ecs/system_order.hpp` is deleted.
- **Removal.** `World` gains removal of a module's systems, for 3j.

*Done when* every engine system declares itself, and a planted pair of unordered writers fails install. The computed
order runs the baseline unchanged; where it differs from today's list, the plan says why.

*As built (2026-11-10):*
- **What orders a system.**
  - `ecs::SystemOrder {after, before, readsCues}`, given to `addSimulate`/`addSync`.
  - In simulate, also the event handles: a channel's writers run before its typed readers (`EventLog::writer` and
    `reader` name their system), and a channel with a cue runs before every system with `readsCues`.
  - A name not installed is no constraint. Hosts install different sets: headless installs rendering at its first
    capture.
- **Ties go to the lesser name** (Kahn's algorithm), not to install order. The order is then a function of the set
  installed: the window host (rendering at once) and the headless one (rendering at the first capture) agree, and
  3j's reload, which adds a module's systems again, cannot move them. Refuted: install order reproduced 2b's list
  with fewer declarations, but made the order a function of the host's history.
- **Recomputed** at each install and, in simulate, before a step whenever the log's topology changed since the last.
  The topology changes when a system is first named a channel's writer or typed reader, or a channel gets its cue.
  - A cycle throws `std::logic_error` naming it (`spin -> physics -> combat -> spin`) and installs nothing.
  - A simulate system added inside a step throws.
- **Cues order by code, never by data.**
  - A cue is declared with its writer: `EventLog::writer(name, system, cue)` replaces `setCue`.
  - The audio schedule and vfx declare `readsCues`.
  - First built as a check when a bank or set binds a channel: a cue reader before the writer threw. Replaced,
    because the error's advice would have the engine name the game's system, and an edge makes the case
    impossible.
  - Which channels a system binds stays in the catalogue (`addReader`).
- **The declarations**, each with its reason at the install:
  - simulate: spin and fly_controller before physics; combat after physics; animation after physics; vfx after
    animation, with readsCues; audio_schedule with readsCues.
  - sync: level before transform; instantiate after level; player_spawn after level and before transform; physics
    after transform; interpolate after physics; view_model after interpolate and before pose; pose after
    interpolate; render_extract after transform, interpolate, pose and instantiate; vfx_draw after render_extract.
  - The engine names no game system. The slice's view model declares itself before `pose`, and its drivers'
    readers put combat before animation.
- **Snapshot hooks run in name order.** `kSnapshotOrder` is gone with `ecs/system_order.hpp`. Each hook restores
  only its own system's state (physics' Jolt state, combat's grenades and round, the audio schedule's voices, vfx's
  records), so their order changes only how the hash is composed.
- **Deleted:** `World::simulateNames` and `syncNames`, which had no caller. `OHAO_LOG_LEVEL=debug` logs each order.
- **The order against 2b's list:**
  - Simulate: fly_controller, spin, physics, combat, animation, audio_schedule, vfx (2b: spin before
    fly_controller, vfx before audio_schedule). With `--game none`: the same without combat.
  - Sync: level, instantiate, player_spawn, transform, physics, interpolate, view_model, pose, render_extract,
    vfx_draw (2b: player_spawn before instantiate).
  - Why each swapped pair is free:
    - spin moves `Spin` entities, the fly controller `FlyController` ones;
    - vfx writes effect records and the audio schedule audio commands, and neither reads the other;
    - instantiate writes `MeshRenderer`, player_spawn characters and their transforms.
- **Results:**
  - **The baseline.** With the hooks still in 2b's order, equal to 3f's at all 5 checkpoints, `world.hash`
    included: `3h_a`, then `3h_b` with the cue edges. With the hooks in name order (`3h`), everything is equal but
    the hash, at every checkpoint.
  - **Planted bugs:**
    - A cycle (combat before spin) fails install: "the simulate systems' order has a cycle: spin -> physics ->
      combat -> spin".
    - Without combat's `after physics`, combat runs first, and the baseline differs at 20 points from checkpoint 3
      (an impact's place, then the round's events).
    - Without `readsCues`, with the drivers' edge taken away and combat renamed to sort last, combat runs after
      the audio schedule and vfx. With it, combat runs before them.
  - **Gates:** `check.py --changed` 6 of 6, no GPU fault. physics_bench and audio_bench are within budget.
  - **feel_bench**, run by mistake: it opens a window for 3 minutes.
    - `look_latency_ms` was 67.4 against 15, almost all of it submit to present (57 ms): the play scene renders at
      20 ms a frame on this laptop. The CPU stages were 0.2 and 1.5 ms.
    - `frame_p99_over_median` 1.22 passes.
    - The first feel_bench on this laptop (its last pass, 9.27 ms, was on Windows with the RTX 5070). Not this
      step's: 3h changes only the CPU order.
- **Not built, by the standard:**
  - **Component and resource accesses, their check (two unordered writers fail install), and waves:** to step 4b,
    with the jobs.
    - Run serially, a declared access changes nothing: the order is already total and deterministic.
    - Nothing here could hold a declaration to the code: systems reach the registry directly, deep in their
      implementations, and an unenforced list drifts from the code, which 3c refused for events.
    - In 4b a wrong declaration is a race, so the declarations come with what enforces them, written once.
      `SystemOrder` gains fields; nothing is rewritten.
    - The spec is corrected.
  - **Removing a module's systems:** to 3j, which needs it and can test it with an unload.
- **Known gap.** A system with an undeclared dependency on an unordered one could move if renamed, since ties go
  by name. 4b's accesses close it.

## 3i. The game API and `games/slice`

- **The game API** is the library `ohao/game`: a `GameModule` with `registerTypes`, `registerEvents`,
  `registerSystems`, `registerMethods` and `registerHud`, and the engine services a game calls: spawn, query,
  raycast, the player, cues, the mix request, presented shapes.
- **What moves to `games/slice`:**
  - combat, the grenade, rounds and the enemy AI;
  - the drivers, now registered through the API;
  - the HUD, the view model, the 9 components and the 5 methods.
- **Linked statically** in this part. Each host's `installSlice` call (3c) becomes the module's registration.
- **The leaks, replaced:**
  - The player controller's `Health` check becomes a `PlayerController` control flag that the game sets. A dead
    enemy keeps its `PlayerController`, with control off, so the audio schedule's `characterOf` needs no
    `Enemy` (3c left it).
  - The ViewModel render flag becomes an engine `FirstPerson` component.
  - The `combatHud` calls become the hosts drawing the registered HUDs.
  - The grenade spheres become presented shapes.
- **Names stay.** Component type names are unchanged (`Health`), so scene files need no migration. Types a game
  registers may not collide with the engine's: the registry fails.

*Done when* no engine library holds game code (`ohao_ecs`, `ohao_api`, `ohao_engine`, `ohao_runtime`, `ohao_vfx`
and `ohao_audio` name none). `replay_test` and the baseline pass with the slice as a module, and the play golden is
exact.

*As built, commit 1 (2026-11-10): the engine's leaks, in place.*
- **The dead player.**
  - A flag in `PlayerController`, as planned, would have been wrong: physics treats any edit of a PlayerController
    as a new character (`touchPlayer`, then `createPlayer`), so every death would rebuild the character in place
    and drop its velocity. A separate component would have been a second truth beside Health, which a restore and
    an API edit would have to keep in step.
  - Instead, physics asks the game: `PhysicsScene::setInputControl(fn)`, "does this input-driven character take
    its input this step?". It is asked where Health was read: the move, turn and jump, and the window's look lead.
  - The slice's rule is Health above 0, set with combat's install. No new state, so `world.hash` is unchanged.
  - An AI's MoveIntent is not gated, as before: the enemy AI zeroes it at death and removes the character.
- **`characterOf`'s `Enemy`, deleted, not replaced.** A dead enemy's character goes at its death, and no scene has
  colliders on an enemy's parts (the play scene, the only one with enemies, has none). So the clause never
  decided anything. A character is what holds a PlayerController; a corpse's parts, should a scene author some,
  occlude as any body does.
- **`FirstPerson`**, the engine's runtime tag (render only, never saved or hashed). The render extract draws an
  entity first person by it. The slice's view model adds it to every `ViewModel` from construction to removal (its
  install connects the registry's signals). The layout docs name it.
- **What a game presents** (`world_render/present.hpp`): HUD functions the hosts draw (`drawHuds`: the window,
  `render.capture {hud: true}`), and sphere sources that `vfx_draw` gives the renderer (`presentedSpheres`).
  `vfx_draw`, the window host and frame captures no longer name combat. Until the module (commit 2), the slice
  registers both with `installCombatPresentation`, called beside `installSlice` by the CLI context, the window
  host and render_bench.
- **`WorldSnapshots`** (`ecs/world_snapshots.hpp`, `installWorldSnapshots`) is the round hook's engine-neutral
  name: a whole-world save and restore for any system, through `World::defer`.
- **Results:**
  - **The baseline:** equal to 3h's, `world.hash` included, at every checkpoint.
  - **Gates:** `check.py --changed` 6 of 6.
  - **Three one-off checks**, for what the fight does not reach (its player has 100,000 hit points):
    - **The dead player:** after `combat.damage`, 30 steps of move and look input leave it where it was ([1.5, 0,
      2.2], yaw 0). Planted, a rule that always says "in control" walks it to [2.47, 0, 0.73], yaw -60.
    - **The HUD:** a capture with `hud: true` differs from one without in 337 pixels at 320x180 (the crosshair, the
      bars).
    - **The grenade in flight:** its sphere is 18 pixels, gone without the slice's sphere source.
  - **Planted:** the view model without its `FirstPerson` signals fails the play golden
    (`cli_blockout_play_deferred`).

*As built, commit 2 (2026-11-10): the slice as a module.*
- **The game API** (`ohao/game`, library `ohao_game_api`): `game::Module` with three entry points.
  - `registerTypes`: its components and event payloads, into the host's scene registry after the engine's.
  - `install`: its systems, channels, drivers, rules, snapshot hooks and presentation, on a World with the engine
    installed. A second call on one World does nothing.
  - `methods`: rows of `game::Method` (a doc, `MethodFn(Call&, params, ctx, out)`, `mutates`), whose params types
    live in the game's own registry. `game::decodeParams` decodes them, as the engine decodes its own:
    `reflect::readParams`, shared.
  - The plan's five entry points became three. Events are types, and their channels open at install; systems, HUDs
    and drivers are all registrations on a World. Two of the five would have been empty indirections.
- **The session serves the game.** `Session(world, types, game)`: its method rows are the engine's, then the game's,
  per session. Dispatch, the catalogue, `schemaHash` and MCP's `tools/list` read one list, `Session::methods()`. The
  free `api::methods()` is deleted. A game method with an engine method's name throws.
- **`games/slice`** (library `ohao_slice`, namespace `ohao::slice`, sources under `games/`):
  - Moved: combat, the enemy AI, the arms and enemy drivers, the view model and the HUD (`hud.*`), which git records
    as renames. Also the methods (`methods.cpp`, from `api/combat.cpp`, now against `game::Call`), and `ecs/slice.*`,
    which became `module.*` (`slice::module()`); these were rewritten past git's rename detection, so they show as
    new files.
  - Cut from the engine: the 9 components (`slice/components.hpp`, from `ecs/components.hpp`); their reflection and
    the 7 payloads' (`types.cpp`, from `ecs/scene_types.cpp` and `ecs/event_types.cpp`); and the methods' params
    (from `api/params.*`).
  - `slice/components.hpp` opens the namespace with `using namespace ohao::ecs`: the game is written against the
    engine's world, and its own names stay apart from the engine's.
- **Hosts:**
  - The CLI (`--game slice|none` gives the module or null), `LiveConfig::game` (`ohao_game` and feel_bench set the
    slice), and the benches that measure the slice, which link it.
  - The CLI's registry holds the slice's types under `--game none` too. The binary is built with the slice, so its
    components load as plain data in an engine-only world, which has none of the slice's systems or methods.
  - `ohao schema` documents the engine with the slice installed.
- **The boundary, checked.**
  - `cmake/ohao_layers.cmake` puts `ohao_slice` at level 9, above every engine library, and owns its headers by
    their path under `games/`.
  - `nm` over `ohao_ecs`, `ohao_api`, `ohao_engine`, `ohao_runtime`, `ohao_vfx`, `ohao_audio`, `ohao_world_render`
    and `ohao_level_world` finds no slice symbol defined or referenced. Two engine comments name the slice, as an
    example.
- **Results:**
  - **The baseline:** equal to commit 1's at every checkpoint but `world.hash`.
    - The cause is shown by A/B: with the slice's types registered at their old places in the engine's registry,
      every checkpoint equals commit 1's, `world.hash` included.
    - The hash covers saved components in registration order, and the slice's now follow every engine type.
  - **Gates:** `check.py --changed` 6 of 6. The play golden is exact.
  - **`--game none`:** the play scene runs 30 steps. `tools/list` has 36 tools and no `combat_*`; with the slice,
    38, `combat_info` and `combat_damage` among them.
  - **Planted:** `ohao_api` linking `ohao_slice` fails configure ("ohao_api (level 7) depends on ohao_slice
    (level 9)"); `ecs/world.cpp` including `slice/combat.hpp` fails it naming the file.
  - **Benches:** physics_bench and audio_bench are within budget.
- **The catalogue changes order.** The slice's methods follow the engine's, and its types follow the engine's, so
  `schemaHash` moves (clients key their cached catalogue on it).

## 3j. A shared module and hot reload

1. **A spike first, on Linux and on Windows.**
   - **Linux:** the executable exports its symbols (`ENABLE_EXPORTS`, `-rdynamic`) and the module is built `-fPIC`.
   - **Windows:** the executable exports its symbols (`ENABLE_EXPORTS`, `WINDOWS_EXPORT_ALL_SYMBOLS`) and the
     module links its import library.
   - **EnTT:** its type identity across the boundary (`type_hash` keys the registry's pools and `ctx()`) is checked
     there.
   - **Alternative:** if the spike fails, the engine becomes one shared library that both the executable and the
     game link.
2. **The module.** `games/slice` becomes a shared library, loaded by path (`--game`, default the slice).
3. **Hot reload** (`game.reload`):
   1. build the module's target;
   2. save the world to its canonical form, the game's registered state included, through reflection;
   3. remove the module's systems, methods, HUDs and types, and unload it;
   4. load the new build and register it;
   5. restore, and continue.

*Done when* a reload keeps `world.hash`, and a small edit is playing again in under 5 s, measured. `replay_test`
passes with the slice loaded as a shared library.

*As built, commit 1 (2026-11-10): the spike, and the slice as a module.*
- **Linux only** (Frank, 2026-11-10: "don worry about windows, if the linux greenlight then we'll keep building it
  here"; the Windows machine lacks the compute for several projects at once). The Windows half of the spike is not
  run.
- **The spike's answers, measured:**
  - **Linking.** The executable exports its symbols (`ENABLE_EXPORTS`, `-rdynamic`); the module (`ohao_slice`,
    `MODULE`) links no engine library, its engine headers taken with `$<COMPILE_ONLY:...>`.
    - The module needs 140 dynamic symbols, 76 of them the engine's. `ohao` and `ohao_game` export all 140.
    - One was missing at first: `game::decodeParams`, which only a game calls, so the linker never took its object
      out of the static library. It is inline now.
    - A missing symbol fails at load (RTLD_NOW), named and demangled.
    - `nm` finds no engine function defined in the module: the process has one engine.
  - **EnTT.** Pools and `ctx()` are keyed by `type_hash`, a hash of the type's name, and `type_info` compares by
    it, so the executable and the module agree.
    - EnTT 3.13 cannot remove a pool, so a module whose types have pools can never be unloaded. A reload loads the
      new build beside the old, which stays mapped; the old pools keep their code.
    - This is the way Unreal's Hot Reload worked (the old DLL stays loaded). Live++ patches in place, which needs a
      layout that does not change.
  - **Our type identity.** `TypeKey` was the address of a static: a second copy of the module would key the same
    type apart, and a re-registration would collide. It is a 64-bit FNV-1a of the type's name now
    (`__PRETTY_FUNCTION__`, at compile time), as EnTT's `type_hash`.
- **Loading** (`game/loader.hpp`).
  - The file is read into a memfd and `dlopen`ed from `/proc/self/fd/N`, RTLD_LOCAL so a later copy never binds to
    an earlier one's symbols.
  - The build can then replace the file while the game runs, and the next load of the same path is a new copy.
  - The memfd stays open: glibc matches loaded objects by name first, and a reused fd number would hand back the
    old copy.
- **Hosts:**
  - `--game slice|none|<path>` on every command of the CLI, taken out of argv before the command.
  - `ohao_game` loads the slice beside it.
  - The benches link the same objects statically (`ohao_slice_static`); the objects are compiled once.
- **Results:**
  - **The baseline:** equal to 3i's at every checkpoint, `world.hash` included.
  - **Gates:** `check.py --changed` 6 of 6, replay_test through the loaded module.
  - **`--game none`:** the play scene loads with its 13 slice components preserved.
  - **Cost:** a 1-step run of the play scene is 21.5 ms with the module, 19.1 ms without (medians of 7; spread 6 to
    9 ms). The load and install are inside the noise.
  - **Benches:** physics_bench and audio_bench are within budget.

*As built, commit 2 (2026-11-10): game.reload.*
- **What crosses, and what is rebuilt.**
  - The old build stays loaded, because its pools run its code. The new build reads in place what the engine holds of
    the game: its components (the pools), its channels and their events, and its snapshot hooks' states, handed from
    the old hooks to the new.
  - Everything else is the new build's own install: systems, drivers, presenters, the control rule, the session's
    method rows.
  - The plan's route cannot run, because EnTT cannot remove a pool. That route was: save the world to its canonical
    form through reflection, unload the module, load the new one, restore. So data whose layout is unchanged crosses
    in place, and a changed layout is refused (below).
  - A reload that rebuilt the World from a snapshot was refuted too: by its own contract, physics' state restores
    exactly only into the same bodies (`restoreSimulation`'s `savedPending`), and a new World has new ones.
- **The install is recorded** (`game/install.hpp`).
  - `installGame` diffs what the World holds before and after `Module::install`: systems, snapshot hooks, animation
    drivers, presenters, the physics control rule. `uninstallGame` removes exactly that.
  - The module's own `uninstall` undoes what the engine cannot see: its context entries (EnTT cannot list them) and
    its registry signals. For the slice that is four context entries and the view model's two signals.
  - Removal APIs came with it: `World::removeSimulate` and `removeSync` (the event log forgets the system as a
    writer, reader or cue giver: `EventLog::forgetSystem`), `removeSnapshotHook`, `AnimationSystem::removeDriver`,
    presenters by id, `PhysicsScene::hasInputControl`.
  - The alternative, each game writing an uninstall for everything it registered, was refuted: a game developer (an
    agent) would forget a registration, and the reload would then fail on "installed twice".
- **Layouts, checked before anything crosses.**
  - `core/layout_key.hpp`: a compile-time 64-bit key of a type's layout. For an aggregate it covers its members' keys
    in order, found by structured bindings over a brace-initialization field count (boost.pfr's technique); for
    containers, their element's.
  - `TypeInfo::layout` carries it for every reflected type. `SnapshotHook::layout` is declared by a game's hook (the
    slice's: `layoutKey<CombatSystem::Impl::State>()`).
  - Before the swap, the new build's types are registered beside the engine's in a scratch registry and compared
    with the live one: names, layouts, enum values. After the new install, each hook's declared layout is compared
    with the old one's, and a mismatch rolls back.
  - Refused, the old build is installed again with its state.
- **`game.reload {}`** reloads from the file the game was loaded from. A path parameter was cut: it would let any API
  caller load a library into the process. It runs between steps, and its method rows change after the call
  (`Session::Impl::pendingGame`), never while one runs. `E_RELOAD` is new.
- **A bug the reload found.** `RestoreEvent.step` (3d) was registered `unitless`, which the registry records as a
  mistake for a 64-bit field and nothing ever read. The scratch registry refused it. Fixed, and `ohao doctor` now
  fails on any registration error (`types.register`, the engine's types and the game's); planted again, it fails.
- **The edit loop.** A precompiled header of the slice's engine includes (EnTT, glm, the World, components, physics,
  events) cut `combat.cpp`'s rebuild from 4.85 to 4.24 s. Code generation is 66% of it (`-ftime-report`).
- **Results:**
  - **Edit to playing** (`combat.cpp`'s damage doubled under a running server, the module built, `game.reload`):
    4.87 s before the precompiled header, about 4.25 s after (the build 4.24 s, three runs alike; the reload 4 ms:
    load 3.6 ms, swap 0.4 ms). The new code played at once: a hit of 10 took 10 before, 20 after, and `world.hash`
    was unchanged by the reload. Other slice files rebuild in 1.3 to 2.9 s.
  - **Mid-fight, the same build reloaded with a grenade in flight:** `world.hash` unchanged, and the 200 steps after
    it equal to a run without the reload.
  - **Refused, the old build kept playing, `world.hash` unchanged:** a field added to `Health` ("Health's layout
    changed"), and a member added to combat's hook state (rolled back after the new install).
  - **replay_test now reloads** before its midway restore: `world.hash` unchanged, the snapshot taken under the old
    build restored under the new, the fight after it the same (12 of 12).
    - Planted, a reload that reports combat's state carried but drops it: replay_test fails.
  - **Gates:** `check.py --changed` 6 of 6. **The baseline:** equal to commit 1's, `world.hash` included.
    physics_bench and audio_bench are within budget.
- **Known gaps:**
  - A layout change needs a new session: reinstancing through reflection (new types per build, each value carried
    by its fields) is not built.
  - Reflected docs and limits keep the old build's until then.
  - Each reload maps another copy of the module, about 1.5 MB.

## 3k. A second game, and the game developers' docs

- **The second game.** `games/sample` is a minimal game against `ohao/game` headers only: targets that spin up and
  fall when shot. It builds and loads in place of the slice.
- **The catalogue** that `ohao schema` generates gains each module's types, methods and channels: what a game
  developer's agent reads. The engine's channels are listed from 3c.
- **The game developers' docs.** A short `docs/games/README.md` covers the API, a module's life, and hot reload.
  `AGENTS.md` keeps the engine's side.

*Done when* both games build against the same API, the sample loads and plays headless (a scripted session), and the
spec's step 3 "done when" holds. The baseline tool is then deleted.

## Spec corrections

- 4.4 and section 3 count 8 game methods; there are 5 (`player.info`, `player.teleport`, `combat.info`,
  `combat.events`, `combat.damage`).
- Section 3's "combat's runtime state is in no snapshot" understates the gap. Physics', animation's and audio's
  counters and the World's input are missing too, and snapshot columns hold raw entity handles. 3b covers all of
  them.
- **4.3, the scheduler (3h).** Step 3 orders by the declared `after`/`before` and the event handles, serially, as one
  total order. The component and resource accesses, the check of two unordered writers, and the waves come with
  4b's jobs, where a wrong access is a race and is enforced. A name not installed is no failure but no constraint,
  since hosts install different sets.
- **4.3, events (3c).**
  - Events are not double-buffered. A step reads its own events, and the schedule puts each writer before its
    readers. A buffer that waits a step is only for a reader that runs before its writer, and none does.
  - What a snapshot and `world.hash` carry is the next step's events, those emitted between steps, not the step's
    own.
