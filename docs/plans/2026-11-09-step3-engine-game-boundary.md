# Step 3: the engine/game boundary

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.3, 4.4 and 5 (step 3). **Status:** 3a and 3b built; 3c-3k planned (3c revised).

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
- **`events.read {since = 0, max = 32, names?}`** returns `{events: [{seq, step, name, ...fields}], next, dropped?}`:
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

**For Frank: one budget.** `serve_bench`'s `audio_info_bytes` (at most 2 KB) measures `audio.info` with 16
commands; after 3c, `audio.info` has no commands. Proposed: the same bar on `events.read {max: 16}` mid-fight, as
`events_read_bytes`. A budget changes only by his ruling.

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

## 3e. One clock

- **One step count.** The World's (3c). The counters of combat, physics, animation, the audio schedule and vfx go;
  each system reads the World's.
- **Animation in seconds.** Clip time and fades are in seconds: graph files carry seconds, migrated by their version.
- **Audio in time.** The audio schedule's holds and blends, the bank's attack, release and in/out times, and the
  audio player's horizon are in milliseconds.
- **No 60 Hz constants.** No code assumes 1/60 except the default clock.

*Done when* the baseline is unchanged at 1/60 s, and the same fight stepped at 1/120 s gives the same animation
and audio timings, measured.

## 3f. Input per player

- **One input stream per player.** `World::input(player)` gives each its own; `PlayerController` names its stream.
- **One function finds the player.** A single engine function replaces the seven copies of "the player".
- **The API.** `input.inject` and `input.record` take a `player`, which defaults to the first.

*Done when* a scene with two input players moves them independently (checked once, by hand), and the baseline is
unchanged.

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
  - The player controller's `Health` check becomes a `PlayerController` control flag that the game sets.
  - The ViewModel render flag becomes an engine `FirstPerson` component.
  - The `combatHud` calls become the hosts drawing the registered HUDs.
  - The grenade spheres become presented shapes.
- **Names stay.** Component type names are unchanged (`Health`), so scene files need no migration. Types a game
  registers may not collide with the engine's: the registry fails.

*Done when* no engine library holds game code (`ohao_ecs`, `ohao_api`, `ohao_engine`, `ohao_runtime`, `ohao_vfx`
and `ohao_audio` name none). `replay_test` and the baseline pass with the slice as a module, and the play golden is
exact.

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
- **4.3, events (3c).**
  - Events are not double-buffered. A step reads its own events, and the schedule puts each writer before its
    readers. A buffer that waits a step is only for a reader that runs before its writer, and none does.
  - What a snapshot and `world.hash` carry is the next step's events, those emitted between steps, not the step's
    own.
