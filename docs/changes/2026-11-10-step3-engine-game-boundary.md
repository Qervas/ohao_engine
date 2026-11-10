# Step 3: the engine/game boundary (parts a to k)

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.3, 4.4 and 5 (step 3); plan
`docs/plans/2026-11-09-step3-engine-game-boundary.md` (each part's As-built notes hold its detail). **Status:** 3a-3k
built (3g merged into 3c), 2026-11-09 to 2026-11-10; merged to `master` up to 3j (`d027e2ff`), 3k on
`step3-engine-game-boundary`.

## 1. Summary

The engine holds no game code. A game is a `game::Module` in `games/<name>`. It is built as a shared library, loaded
at run time by the host executable, and swapped for a newer build of itself in a running session, the world kept
exactly (`game.reload`, about 4 ms). The game sees the engine through one header, `game/api.hpp`, and configure fails
when it includes any other engine header, or when an engine library links or includes a game.

Underneath:
- the world runs without the renderer;
- every system's state and the input are in snapshots and `world.hash`;
- one event log carries the whole simulation's events, and the engine reads a game's only as cues its data binds;
- restores happen between steps;
- one clock, and input per player;
- each phase's system order is computed from what the systems declare.

Two games build against the API: the slice (the fight the engine was built with) and the sample (the smallest game).

## 2. What was built

- **3a, the world without the renderer** (`f3784a47`, `c9c52e42`): `ohao_ecs` links no renderer, GPU or Vulkan;
  `ohao_world_render` joins the world to the renderer; the renderer sits at the world's level.
- **3b, complete state** (`518f93fc`): what a later step reads is in a snapshot and `world.hash`, through snapshot
  hooks (save, restore, hash): physics' simulation, combat's grenades and round, the audio schedule, vfx records, and
  the input. Entity references survive a restore (`createWithId` with a handle hint).
- **3c, one event log** (`abca6b62`, `b7618398`; 3g merged in):
  - A channel is a name and a payload type, with writer and reader handles that name their system. A step reads its
    own events, the window, which is state: snapshotted and hashed. The history keeps the last 8,192, by a seq that
    is never state.
  - The slice's ten channels are the game's. The engine reads them only as a `Cue` that the channel's projection
    makes, bound by an audio bank's and a vfx set's `events`.
  - `events.read` replaced `combat.events`, `physics.events` and the event halves of `animation.info` and
    `audio.info`.
- **3d, restores between steps** (`3629fc82`): `World::defer`. No snapshot or restore inside a step (both throw). A
  `world.restore` event, in the history only.
- **3e, one clock** (`730b06bb`): the World's step and dt. Durations in data are seconds or milliseconds, state counts
  whole steps of the dt. Animation graphs are version 2, with fades in seconds.
- **3f, input per player** (`c6576bd3`): `World::input(stream)`, up to 8 streams; `playerOf(world, stream)`.
- **3h, the declared scheduler** (`3b34953d`):
  - Each system declares `after`, `before` and `readsCues`. A channel's writers run before its typed readers, and a
    channel with a cue before every system that reads cues.
  - Ties go by name, so the order is a function of the set installed. A cycle throws at install, naming it.
  - `ecs/system_order.hpp` is deleted. Snapshot hooks run in name order.
- **3i, the slice as a game module** (`0ddf7f0c`, `4cacf188`, `5867dd64`):
  - The engine's leaks were replaced first:
    - physics asks the game whether a player takes its input (`setInputControl`) instead of reading `Health`;
    - the renderer draws first person by the engine's `FirstPerson`;
    - HUDs and presented spheres are functions a game registers;
    - the round hook is the engine's `WorldSnapshots`.
  - Then the move: combat, the enemy AI, the drivers, the view model, the HUD, 9 components, 7 payloads and 2
    methods moved to `games/slice`. A session serves the game's methods after the engine's.
- **3j, a shared module and hot reload** (`5b0bcb49`, `77b90e3f`):
  - The slice is `libohao_slice.so`. It is read into a memfd and `dlopen`ed (RTLD_NOW, RTLD_LOCAL); the host
    exports the engine's symbols (`ENABLE_EXPORTS`). `TypeKey` is a hash of the type's name.
  - `game.reload` loads the new build beside the old one (EnTT cannot remove a pool), and swaps the game's behaviour
    onto its data. The engine records what an install added (`game/install.hpp`).
  - A compile-time layout key (`core/layout_key.hpp`) on every reflected type and on each game hook's state refuses
    a build whose data would be misread.
- **3k, a second game and the API's header** (`332d6b4a`, and this record's commit):
  - `games/sample`: targets that spin until shot, then fall.
  - `game/api.hpp`, the only engine header a game includes, which is also the games' precompiled header.
  - `ohao_add_game`, which builds every game the same way.
  - The catalogue's `module`, the game that owns each entry.
  - `docs/games/README.md`. The temporary baseline tool is deleted.

## 3. Bugs found and fixed

- **The slice's kill restore at step 1 was inexact.** Found at 3c.2, it was already wrong at 3c.1. Fixed in 3d: the
  players spawn before the sync.
- **`events.read` wrote `world.restore`'s step as hex digits** (3d). A plain 64-bit field is now a number; an entity
  is an id and a name.
- **`events.read` wrote floats as `0.65000004`** (3c): now rounded in double.
- **`RestoreEvent.step` was registered `unitless`** since 3d, which the type registry records as a mistake for a
  64-bit field. Nothing ever read the registry's mistakes; 3j's reload, registering into a scratch registry, refused
  it. Fixed, and `ohao doctor` now fails on any registration error (`types.register`); planted again, it fails.
- **`game::decodeParams` was missing from the host** (3j's spike): only a game called it, so the linker never took
  its object into the executable. It is inline now. A missing symbol fails at load, named and demangled.

## 4. Spec corrections

Each is in the plan's "Spec corrections" section; the spec is corrected where it described the design:
- **4.3, the scheduler:** step 3 orders by declarations and event handles, serially. Component and resource accesses,
  the unordered-writer check and waves come with 4b's jobs.
- **4.3, events:** not double-buffered. A step reads its own events; a snapshot carries the next step's.
- **4.4, the module:** `registerTypes`, `install`, `uninstall` and `methods`, and one header, `game/api.hpp`.
- **4.4, hot reload:** the module is never unloaded. The new build reads the game's data in place, and a layout change
  is refused.
- **Method count:** section 3 counted 8 game methods; there were 5, and the slice now has 2.

## 5. Results

- **The fight**, recorded at every part and compared with the part before:
  - Logs (combat, physics, audio, vfx, animation) and scenes equal throughout.
  - Where `world.hash` moved, an A/B showed ordering only: 3h's snapshot-hook order, and 3i's type registration
    order (with the slice's types at their old places, the hash matched too).
  - From 3i's second commit to 3k the hash too is equal.
- **Gates:** every commit passed the gates its files reach, through the pre-commit hook. At 3f the hook blocked a
  commit while the GPU's firmware had faulted (below); after the full power-off it passed, and no run has logged a
  GPU fault since.
  - `replay_test` reloads the game before its midway restore and passes 12 of 12.
- **Planted bugs, each caught:**
  - **3b:** combat's hook restoring nothing; a restore without the input.
  - **3c:** a snapshot without the next step's events; the bank's `by` ignored (94 audio commands became 107).
  - **3d:** a round's snapshot taken inside a step (throws).
  - **3h:** a cycle (`spin -> physics -> combat -> spin`); combat without `after physics` (20 differences in the fight);
    `readsCues` removed (combat falls after the audio schedule and vfx).
  - **3i:** an always-in-control rule (a dead player walks 1.7 m and turns 60°); the view model without its
    `FirstPerson` signals (the play golden fails); an engine library linking the slice, and an engine source including
    one of its headers (configure fails).
  - **3j:** a registration mistake (doctor fails); a reload that drops combat's state (`replay_test` fails).
  - **3k:** an engine header in a game (configure fails).
- **Refused, the old build kept playing and `world.hash` unchanged** (3j): a field added to `Health`, and a member
  added to combat's hook state, which rolled back after the new build's install.
- **Costs** (medians on this machine):
  - The combat step was 0.0021 ms before step 3 and 0.0022 ms after 3c.
  - A round's restart restore was 0.0034 ms before step 3 and 0.0043 ms after 3d (budget 2 ms).
  - `events.read` of the last 16 events of the fight is 1,832 bytes, gated at 2 KB (Frank's ruling).
  - A 1-step play scene run is 21.5 ms with the module loaded and 19.1 ms without (spread 6 to 9 ms).
  - physics_bench and audio_bench were within budget after 3h, 3i and 3j.
- **The edit loop:**
  - `combat.cpp`'s damage doubled under a running server, built, and reloaded: edit to playing 4.87 s, then 4.25 s
    with the games' precompiled header (the build 4.24 s; the reload 4 ms, a 3.6 ms load and a 0.4 ms swap).
  - The new code played at once (a hit of 10 took 20), with `world.hash` unchanged by the reload.
  - Other slice files rebuild in 1.3 to 2.9 s.
- **The sample**, headless from a script: one press of `fire` downs the middle target, which rests at y 0.295 90
  steps later. The script twice gives one hash, and a reload inside it gives the same hash.

## 6. Known gaps

- **Layout changes:** an edit that changes a game type's or hook state's layout needs a new session. Reinstancing
  through reflection (new types per build, each value carried by its fields) is not built.
- **Docs and limits:** a game's reflected docs and limits keep the old build's until a new session.
- **Memory:** each reload maps another copy of the module, about its size (1.5 MB for the slice).
- **Component accesses:** their declarations and parallel waves are 4b's.
- **Windows:** not built since 2026-11-05, and out of scope since Frank's ruling of 2026-11-10 ("don worry about
  windows"). The module loading is Linux's (memfd, `dlopen`).
- **The window hosts with the module** (`ohao_game`, `ohao serve --window`) were checked statically: both export all
  140 symbols the slice needs. No window was run (no looping window programs).
- **`feel_bench` on this laptop:** `look_latency_ms` is 67.4 against 15, from submit to present 57 ms (the play scene
  renders at 20 ms a frame here). `frame_p99_over_median` 1.22 passes. The budget is Frank's to rule.
- **The GPU firmware:** a fault during 3f (Xid 31, 120, 154) needed a full power-off. It was not traced to OHAO's
  code, and no fault has come since.
