# Step 3: the engine/game boundary

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.3, 4.4 and 5 (step 3). **Status:** 3a and 3b built; 3c-3k planned.

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

## 3c. Typed events and one event log

- **Typed events.** `ecs::Events<T>` lives in the World: emitted into this step's buffer, swapped at the step's
  start, and read through an `EventReader<T>` whose cursor is saved state, so each reader sees each event once.
- **Registration.** An event type is registered by name (`registerEvent<Shot>("slice.shot")`).
- **One bounded log** of every event, with a sequence number, replaces the per-system logs of combat, physics and
  animation.
- **One API method.** `events.read {since, max, names?}` replaces `combat.events` and `physics.events`. The
  animation log is read the same way.
- **Hashed.** The step's events are covered by `world.hash`.

*Done when* every combat, physics and animation event goes through `Events<T>`, the three logs are deleted, and the
baseline's logs read the same through `events.read`. Their sequence numbers may renumber: one log, not three.

## 3d. The round restart as a request

- **A request, carried out later.** Combat asks for a restore (`ecs::requestRestore(world, snapshot)`); it is carried
  out after the step's last simulate system. The next step starts with a `Restart` event.
- **One rule deleted.** "Skip what came before the step's last restart" goes from all seven readers.
- **Physics simplified.** Its restored-player path is cut down to what a restore at a step's boundary needs.

*Done when* the seven readers have no restart rule.

- **A temporary restart check:** a death, the restart, and the world equal to the round's start.
- **Measured,** not gated: `physics_bench`'s restart time.
- **Expected change in the baseline:** none, since its fight never restarts.

## 3e. One clock

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

- **A cue projection per event type.** Each registered event type gives, by a compiled function (no reflection per
  step), its cue: `at`, `to`, `normal`, `source`, `target`, `tag`.
- **Audio banks** bind by name: `{"on": "slice.shot.player", "play": "fire"}`, `"play": "impact_{tag}"`.
- **Vfx sets** bind by name: `{"on": "slice.hit", "spawn": "impact_{tag}"}`, and a tracer on a cue's `at` and `to`.
- **The game emits what differs** (a player's shot or an enemy's, a reload from empty) and the tracer's two ends.
  The engine names no game event.
- **The mix snapshot** (`dead`, `won`) becomes a request the game makes. Animation events bind the same way
  (`"on": "anim.mag_out"`).

*Done when* `audio_schedule`, `vfx` and `vfx_set` name no game event or component. The baseline's audio commands and
vfx records are unchanged.

## 3h. The declared scheduler, serial

- **Each system declares itself:**
  - its phase;
  - the components it reads and writes;
  - the events it emits and reads;
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
- **Linked statically** in this part, through one registration call per host.
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
- **The catalogue** that `ohao schema` generates gains the registered events and each module's types and methods:
  what a game developer's agent reads.
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
