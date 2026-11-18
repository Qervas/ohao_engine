# Tidewall: the mission demo, its design

**Plan:** `docs/plans/2026-11-18-tidewall.md`. Builds on the system design (`docs/specs/2026-11-08-system-design.md`)
and the engine modules design (`docs/specs/2026-11-18-engine-modules-design.md`, rulings R4-R7). **Status:** designed
2026-11-18 (documents only) on branch `step4b-jobs` at `8694c1cd`, read-only: nothing was built or run. Facts are
from the tree, with `file:line`; every number without a source is an *estimate*, to be measured by the part that
builds it. The build starts after step 4b closes (4b-7 to 4b-10 are open, `docs/plans/2026-11-18-step4b-jobs.md:544-568`).

## Rulings (Frank, 2026-11-18)

- **F1. A real demo level, as game code.** A new game under `games/`; the slice stays, and the gates keep its play
  scene (`tests/replay/replay_test.py:42`). What the demo pulls out of the engine that is general is an **engine
  module**, one feature each (R4), additive by proof (R5); the mission, its behaviours, its layout and its art are the
  game's.
- **F2. CoD-like first:** a linear first-person mission of about 10-15 minutes. Original: the genre's mechanics, never
  a specific game's names, characters, levels or look.
- **F3. Navigation on Recast/Detour.** The dependency is approved in principle; the download is pinned and waits under
  "Needs Frank".
- **F4. Blockout, but not all blockout:** one part at real high quality, to prove the engine can do it.
- **F5. The bar** is production AAA (`docs/ROADMAP.md:18`), and the game is built by agents through `ohao serve`/MCP,
  with deterministic replay (`docs/ROADMAP.md:35-37`).

## 0. What must be true (the requirement)

1. **A player can play a whole mission:** start, about 13 minutes of beats, an end screen; die and resume at the last
   checkpoint, the world exactly as it was there.
2. **Enemies fight like the genre's:** they move through the level on paths, see and hear, take cover, flank, suppress,
   throw grenades, react to the player's teammates; nobody walks into a wall (today they walk straight at the last
   seen place, `games/slice/enemy_ai.cpp:215`).
3. **Everything the mission adds is an engine module or game code, never an edit of a module it does not own** (R4,
   R5), and each module is deletable with the engine still building and its gates green.
4. **An agent can build it through MCP:** author the level's layout, the encounters, the mission's script, the
   checkpoints; ask the engine why an enemy did what it did; replay a fight exactly.
5. **Determinism holds:** the same `world.hash` at 1 thread and at N (the 4b rule,
   `docs/plans/2026-11-18-step4b-jobs.md:85-90`), from a checkpoint and in a new process.
6. **One part reaches AAA quality**, measured by the frame's cost, by the path tracer as referee, and by Frank's eye.
7. **The edit loop stays seconds** (AGENTS.md "Seconds"): a layout edit, a navmesh rebuild, a mission edit and a
   behaviour edit each reach a running session in seconds.

## 1. Measured: what the engine has, what the mission lacks

| Capability | Today | Source |
|---|---|---|
| Character movement | Jolt `CharacterVirtual`, the player's and enemies' (`input: false`, moved by `MoveIntent`) | `ohao/physics/components.hpp:91-126` |
| Queries a game can call | `raycast`, `raycastFrom` only; shape casts and overlaps are private to the backend | `ohao/physics/physics_scene.hpp:200-203`; `ohao/physics/private/backend/physics_backend.hpp:516-529` |
| Collision layers | 16 defined (DEBRIS, PROJECTILE, RAGDOLL, USER_0-3); the scene assigns only CHARACTER; no public choice | `physics_backend.hpp:71-90`; `physics_scene_players.cpp:121` |
| Motion type change | `setMotionType` in the backend | `physics_backend.hpp:487` |
| Joints | six types, `breakForce`/`breakTorque`, a break disables (never removes) the constraint, `physics.joint_break` | `ohao/physics/components.hpp:55-84`; `docs/notes/simulation.md:113-127` |
| Ragdolls | specified, step 1 (joints) built, the death ragdoll (step 2) not built | `docs/ROADMAP.md:74`; no `struct Ragdoll` in `ohao/` |
| Navigation | none: no navmesh, Recast or Detour anywhere; enemies walk straight | `games/slice/enemy_ai.cpp:215-220`; `docs/specs/2026-10-26-level-pipeline-design.md:86` (out of scope) |
| Enemy AI | the slice's: `Idle, Chase, Attack, Dead` state machine, sight by one ray to the chest in a cone while idle, rays on `jobs::parallelFor`, woken by damage, no hearing | `games/slice/components.hpp:103-128`; `enemy_ai.cpp:82, 149-199` |
| Hearing's sources | `physics.footstep`/`physics.land` (`FootEvent`), `slice.shot`, `slice.explode`; the AI reads none | `ohao/physics/events.hpp:18-19, 36-45`; `games/slice/combat.hpp:27-38` |
| Weapons | the slice's hitscan rifle with spread, bloom, recoil, ADS, magazine (reserve unlimited); a test grenade with no damage | `games/slice/components.hpp:27-62`; `combat.cpp:215, 355-395` |
| Health | `Health{max, current}`, `damage()`; rounds hit the enemy's capsule (no head) | `games/slice/components.hpp:18-21`; `combat.hpp:136`; `docs/notes/simulation.md:334` |
| Triggers | `trig: 1` makes a sensor; `physics.trigger_enter`/`exit`; nothing scripts them | `ohao/level/level_hints.hpp:39`; `ohao/physics/events.hpp:16-17` |
| Checkpoints | none; the slice's round start is the one save point, a whole-world snapshot through `WorldSnapshots` and `World::defer` | `games/slice/combat.cpp:420-477`; `ohao/ecs/world_snapshots.hpp:12-15`; `ohao/ecs/world.hpp:210` |
| Event cues | an engine system reads a game's channel only as a `Cue` (at, to, dir, normal, source, target, tag, n), bound by channel name in data | `ohao/ecs/events.hpp:26-28, 59-70` |
| UI | the HUD pass: rects, a vignette, a 5x7 bitmap font of digits, A-Z, '/' and '.'; 2,048 items; no menu, no TTF | `ohao/render/hud/hud_list.hpp:18-51` |
| Prefabs, runtime spawning | none (`entity.create` and `entity.duplicate` are edit commands) | `docs/agents/ohao-cli.md:394-408` |
| Level authoring | Blender scripts headless (`bpy`), glTF with `extras` hints `col body mass fric rest trig surf vis spawn socket verb vfx`; one `Level{asset}` line in a scene; cooked on first load | `tools/level/blockout_room.py:1-13, 133-135`; `ohao/level/level_hints.hpp:6, 35-47` |
| The only level | `blockout_room.glb`: an 8 x 6 m room on an 80 x 80 m pad, 20 KB | `assets/levels/README.md:10` |
| Load at scale | kit2k (2,000 nodes, 1M triangles) loads in 88 ms, collision 12 ms, a ray 0.34 us | `docs/changes/2026-11-11-step5-assets.md:74-75` |
| The frame | the play scene's GPU frame at 1920x1080 on this 4060: 8.7 ms at render scale 1, 4.7 at 0.667, 3.2 at 0.5 | `docs/notes/renderer.md:549-550` |
| A large lit scene | Sponza 8.18 ms by day, 7.64 at night (37 lamps) at 1080p, `rt.gi` the largest pass (2.24 ms); measured 2026-10-31, before the Linux port, no budget | `docs/changes/2026-10-31-bench-sponza.md:19-28` |
| Many agents | the crowd (64 enemies, a 1,000-box pile) steps in 0.98-1.01 ms at 8 workers; `enemies16_step_ms` budget 1.0 | `docs/plans/2026-11-18-step4b-jobs.md:365-368`; `tests/bench/physics_budgets.json:20, 27` |
| Characters and art | Quaternius' CC0 enemy (43 joints; idle, walk, run, aim, shoot, hit, death; no crouch, no cover clip); our blockout arms and rifle, flat colours, no textures | `assets/characters/toon_enemy/README.upstream.md:11-15`; `assets/characters/fps_arms/README.upstream.md:20-23` |
| Frank's assets | `assets/local/flat42/`: 4 Somnabulate props staged (PinnChair, KitchenTable, BookStack, Kettle, 17 MB, BC/N/ORM textures); 78 LOD0 props in the source; never committed | `assets/local/README.md:3-34`; `docs/specs/2026-10-26-level-pipeline-design.md:144-149` |

## 2. The mission (decision 1)

**The game: _Tidewall_** (`games/tidewall`, `libohao_tidewall.so`). Original premise, names and place.

**Premise.** Night, an estuary. The Tidewall is a tidal barrier with a pumping station and a sluice hall, guarding a
low town behind it. An armed group has taken it and means to open the sluices at the spring tide's peak. A three-person
recon team comes ashore below the barrier: the player (call sign *Wren*), the team lead *Marrow* and the gunner *Tolle*.
They must reach the sluice hall, jam the gear with a charge, and get out along the barrier's crest before it blows.

**The beats** (about 13 minutes; each exists because it pulls and proves an engine capability):

| # | Beat | Min | What happens | What it pulls and proves |
|---|---|---|---|---|
| B0 | **The culvert** | 1 | Ashore in a drainage culvert under the barrier; the team moves up; first objective, radio lines with subtitles; movement and ADS taught by doing | `mission` (objective, trigger, `say`), `ui` (objective line, subtitles), teammates following posts (`navigation`, `ai`), dialogue on the audio bank |
| B1 | **The pump yard approach** | 2 | Two patrols walk routes through the outer yard; the team can slip past along a pipe rack or engage; a shot, a body found or a seen teammate alerts the group | `perception` (sight cones, hearing from shots and footsteps through cues, memory, a group's shared alert), patrol routes on the navmesh, behaviours (patrol, investigate, alert) |
| B2 | **The yard** | 3 | 10-12 enemies among containers, pipe racks and a catwalk: they take low and high cover, flank, suppress, throw frags; a reinforcement wave through the north gate; a checkpoint when the yard is clear | `tactics` (cover points from the navmesh, scored position queries), `ai` (group tokens: who shoots, who flanks, one grenade at a time), off-mesh links (drops from the catwalk), `weapons` (frags both ways), the step's cost with 12-16 agents |
| B3 | **The night-shift quarters** (the hero area) | 2 | Into the staff block: a dim kitchen and break room, a television left on, rain on the windows; two enemies; close quarters; a door kicked open | the high-quality part (section 5): materials, practical lights, glass, the referee; doors as navmesh obstacles; close-range AI |
| B4 | **The sluice hall** | 3-4 | A breach charge blows a wall into a 40 x 24 x 12 m machine hall; clear it; plant the charge on the sluice gear and hold 90 s while waves come from two sides, one of them blowing its own way in through a second wall; checkpoint before the hold | `destruction` (pre-fractured walls, debris, the navmesh opening), `mission` (a timer, waves of dormant actors, signals), the most agents alive at once (24 *estimate*), lights and GI in a large interior |
| B5 | **The crest** | 2 | Out along the barrier's crest road (about 120 m) as the gear blows: a section collapses behind the team, a last stand at the far end, the extraction point, the end screen | a scripted destruction sequence, long exterior sightlines (the frame at distance), the mission's end and its screen |

- **Failure and resumption:** the player's death restores the last reached checkpoint (between steps, by
  `World::defer`, as the round restart does, `games/slice/combat.cpp:453-477`); teammates cannot die (the genre's
  practice; it removes a failure that teaches nothing).
- **Refuted, then kept:** *"A shorter mission (5 minutes) proves the same."* It proves the same modules but not the
  mission's scale: a level 300 m long, 60 or more enemies over its length, checkpoints across it, the frame at the
  crest's distances. Those are where an engine fails (memory, the navmesh's build, the frame at long view). *"Stealth
  is scope creep."* B1's stealth is perception's proof (hearing and alert propagation), which the yard needs anyway; it
  adds no system beyond `perception` and two behaviours.

## 3. The engine modules it pulls (decision 2)

**Decided:** eight new engine modules, each one feature (R4), plus two extracted from the slice. A module's shape is
the modules design's (its data, a pure core below where it has one, its systems with their declarations, its methods;
`docs/specs/2026-11-18-engine-modules-design.md:24-29`), its directory `ohao/<m>/`, `ohao_add_module(<m> DEPENDS ...)`.

| Module | Depends on | Feature | Public surface (headers) | Data | Methods |
|---|---|---|---|---|---|
| `navigation` | ecs, physics, level | navmesh, paths, path following, local avoidance, obstacles | `navigation.hpp` (queries, `followPath`), `components.hpp` (`NavAgent`, `NavPath`, `NavObstacle`, `NavLink`) | the cooked `.nav` (per tile), level hints `nav`, `navlink`, `navobst` | `nav.info`, `nav.path`, `nav.nearest`, `nav.reach`, `nav.show` |
| `perception` | ecs, physics | sight, hearing, memory, teams | `perception.hpp`, `components.hpp` (`Senses`, `Team`, `Awareness`), `events.hpp` (`perception.noticed`, `perception.lost`) | a `.senses.json` (stimuli bound by channel name and cue, team hostility) | `perception.info` |
| `tactics` | ecs, physics, navigation | cover points and scored position queries | `tactics.hpp` (`queryPositions`), `components.hpp` (`CoverPoint`, `Post`, `PostGroup`) | cover cooked with the navmesh; a `.tactics.json` (named queries) | `tactics.cover`, `tactics.query` |
| `behavior` | ecs | behaviour trees as data | `behavior.hpp` (the runtime, `registerLeaf`), `components.hpp` (`Behavior`, `BehaviorState`, `Blackboard`) | `.bt.json` | `behavior.info` |
| `ai` | behavior, navigation, perception, tactics, weapons, damage, animation | the standard combatant: its leaves (move, face, fire, take cover, throw, reload, bark), groups and tokens | `ai.hpp`, `components.hpp` (`Combatant`, `AiGroup`) | leaves' parameters in the trees; groups in the scene | `ai.info` |
| `mission` | ecs, physics | triggers, objectives, sequences, signals, checkpoints, dormant actors | `mission.hpp`, `components.hpp` (`Mission`, `Dormant`), `events.hpp` (`mission.signal`, `mission.objective`, `mission.say`, `mission.checkpoint`, `mission.end`) | a `.mission.json` | `mission.info`, `mission.checkpoint`, `mission.start` |
| `destruction` | ecs, visual, physics, navigation | pre-fractured clusters, damage to pieces, debris | `destruction.hpp`, `components.hpp` (`Destructible`, `Fragment`), `events.hpp` (`destruction.break`) | the cooked fracture (`.frac`), hint `frac` | `destruction.info` |
| `doors` | ecs, physics, navigation | doors that open, close and are kicked, as obstacles | `components.hpp` (`Door`) | hint `door`; opened by a signal's cue | (`world_query`) |
| `damage` (from the slice) | ecs, physics | health, hit zones, damage events, radial damage | `damage.hpp`, `components.hpp` (`Health`, `HitZones`), `events.hpp` | components | `damage.info`, `damage.apply` |
| `weapons` (from the slice) | ecs, physics, damage | hitscan weapons, thrown and fired projectiles, ADS, the magazine and reserve | `weapons.hpp`, `components.hpp` (`Weapon`, `WeaponState`, `WeaponTrigger`, `Thrown`), `events.hpp` | `.weapon.json` per weapon | `weapons.info` |
| `ui` | ecs, visual, world_render | text with a real font, panels, menus driven by input actions, subtitles | `ui.hpp` (a layout list on the HUD pass), `components.hpp` (`Menu`) | an MSDF font atlas cooked from a font file | (`render.capture`) |

**Not a module:** the mission's script, the trees, the encounters' tuning, the layout, the art and the HUD's design are
`games/tidewall`. The trees' leaves specific to the game (a takedown, a scripted fall) are the game's, registered with
`behavior::registerLeaf`.

**Why `damage` and `weapons` move out of the slice (the global optimum, AGENTS.md Deciding 3).** The slice's combat
is 603 lines of game code (`games/slice/combat.cpp`) that the second game needs whole: hitscan, spread and recoil
from counter-based random numbers, ADS, reloads, the round's damage. Copying it is two sources of one fact; writing
Tidewall's own is the same code written twice. Moving it once, in its final shape (projectiles, hit zones, radial
damage and a reserve added in the same part), is written once. The slice then uses the modules; its channels become the
modules' (`weapons.shot` for `slice.shot`), **a deliberate change of every `world.hash`**, named in the commit with
its reason, as R2 did. Refuted: *"weapons are design, engines do not ship them"* (Unreal and Unity ship none). The
genre's engines do (IW's and Source's weapons are engine code tuned by data files), and an AI-native engine wants a
weapon to be a data file an agent writes, not a C++ system per game. The tuning stays data; the slice's `Round`, HUD,
arms and view model stay the slice's.

### 3.1 `navigation` on Recast/Detour

**Requirement:** enemies and teammates reach any reachable point by the shortest walkable route, around one another,
through doors and breaches that open during play, the same at any thread count and after any restore.

**State of the art.** Recast/Detour (Mononen; the navmesh in Unreal, Unity's older system, Godot and most shipped
engines): voxelize the walkable collision, build regions and polygons per tile, query with A* over polygons and string
pulling (`findStraightPath`). Its dynamic parts: `dtTileCache` (compressed layers per tile, rebuilt in about a
millisecond when a box or cylinder obstacle is added or removed) and `DetourCrowd` (corridors, local boundary,
sampled-velocity avoidance, a serial update of every agent).

**Decided.**
- **Built at cook, from the level's static collision**, never at load: the cooker reads the level's collision
  triangles (what `col` hints and the default mesh collider give, `ohao/level/level_system.cpp:350`), minus nodes
  tagged `navobst` or `frac` (they are obstacles, below), and writes one `dtTileCache` layer set per 32 m tile into the
  cooked cache (`.ohao/cooked/`, `ohao/cook/cook.hpp:2-8`), each tile keyed by the hash of the triangles that overlap
  it plus the build settings. A layout edit rebuilds only its tiles; tiles build in parallel on the pool (each
  tile's output depends only on its input, so the order of their completion cannot reach the result).
- **Agent settings** are the characters': radius 0.35 m, height 1.8 m, step 0.35 m, slope 45 degrees (*estimate*,
  read from the enemy's `PlayerController` at cook, one agent class). Cells of 0.15 m (*estimate*; measured against
  build time and the doorway's clearance in part N1).
- **Obstacles, not tile rebuilds, for what changes in play.** A door, a breakable wall, a vehicle: an oriented box
  (`dtTileCache::addBoxObstacle`, Detour 1.6) present while the thing blocks. Their set is a function of components
  (`Door.open`, a `Destructible`'s intact pieces), and a step that changes it runs `dtTileCache::update` to completion
  before the next system reads the navmesh, so the navmesh at step S is a function of the state at S. A destructible
  is baked *out* of the navmesh and stands in as its obstacle: removing the obstacle is what "the breach opens a path"
  is.
- **No poly refs in saved state.** A path (`NavPath`, saved) is at most 32 world-space corners from
  `findStraightPath`, the step it was planned and the navmesh version it was planned on. An agent follows its corners
  and replans on a schedule (every 0.5 s, staggered by entity id) or when the version changes. So the navmesh never
  needs to be in a snapshot: after a restore it is rebuilt from the base tiles and the restored obstacle set, and
  whatever salts its tiles get, nothing saved names them. Its contribution to `world.hash` is the obstacle set's
  hash and the version.
- **Not DetourCrowd.** Taken from it: `dtPathCorridor`'s idea (follow the next visible corner) and
  `dtObstacleAvoidanceQuery`'s sampled-velocity avoidance, called per agent as a pure function of positions and
  velocities (the characters' `BodyMotion`). Not taken: `dtCrowd` itself. It integrates agents' positions, where the
  physics step is the one authority over where a character is (`docs/notes/simulation.md:128-139`); its state
  (corridors of poly refs, boundaries, neighbour lists) is not copyable into a snapshot; and its update is one serial
  loop over all agents. The step writes `MoveIntent` from the avoided velocity; Jolt's character moves it.
- **Determinism.** Detour's queries are pure functions of the navmesh and their arguments when each thread has its own
  `dtNavMeshQuery` (its node pool is cleared per query); the `navigation` system runs agents in `parallelFor` chunks
  (`ohao/core/jobs/graph.hpp:65`), each chunk with the query of its worker slot, results written per agent. Recast and
  Detour are compiled with `ohao_machine`'s flags (`cmake/ohao_machine.cmake`), as Jolt's are, so the float results
  are one build's. Proven by `replay_test`'s discipline: the same hashes at 1 and 31 threads, the waves reversed.
- **Links:** `navlink` hints (a drop from the catwalk, a vault over a low wall, a ladder later) are off-mesh
  connections, one-way or two-way; the leaf that crosses one plays its clip and moves the character along it.
- **Cost** (*estimates*, measured in N1): the whole level's tiles (about 300 x 120 m, 40 tiles) cook in under 5 s cold
  on the pool; load under 20 ms; a path query under 50 us p95; an obstacle change under 2 ms of tile cache update.

**Authored and checked through MCP.**
- `nav.info`: tiles, polygons, the obstacle set, the version, the build's time and settings, warnings (an island
  unreachable from the player's spawn).
- `nav.path {from, to}`: the corners and the length, or why there is none.
- `nav.reach {from, points}`: which of them are reachable and how far (an agent checks every spawn, post and objective
  of a mission at once).
- `nav.show {on}`: the navmesh, links and obstacles drawn into captures through the gizmo pass, so `render.capture`
  shows an agent the walkable space.

**Refuted, then kept.** *"Build the navmesh at load, from whatever the World holds; it is simpler."* At load it costs
every session start seconds (*estimate*) and is rebuilt even when nothing changed; at cook it is a cached file like a
mesh. *"Rebuild Recast tiles from current collision on every destruction; it handles anything."* A tile rebuild is
tens of milliseconds (*estimate*) and must complete inside the step that broke the wall to stay deterministic; the
obstacle model covers every change the mission has (doors, breaches, a collapsed crest is a removed obstacle plus
an added one) at a millisecond. Full rebuilds come when a change the obstacles cannot express appears.

### 3.2 `perception`

**Requirement:** an enemy knows what it has seen, heard or been told, how sure it is and how long ago; nothing knows
what it could not have sensed (no wallhacks).

**State of the art.** Thief's and The Last of Us's sense models (sight with cones and distance bands, sound with
loudness and occlusion, a decaying memory per target), Unreal's AIPerception (senses as configs, stimuli registered by
sources, a per-agent memory with age). Taken: senses as data, stimuli from sources, memory with age and certainty.
Not taken: Unreal's per-frame sense ticking for every agent (time-sliced here by step and id).

**Decided.**
- **A module of its own, apart from navigation**: it needs rays and positions, not paths; a turret or a camera has
  senses and no navmesh.
- **Sight:** a near cone (wide, short) and a far cone (narrow, long), each a band of how fast awareness rises; a ray
  from the eye to up to three points of the target (head, chest, pelvis) through `raycastFrom`; at most one sight
  check per agent per 4 steps, staggered by id, batched on the pool as the slice already does
  (`games/slice/enemy_ai.cpp:82`). Darkness and the target's stance scale the rise (light from the target's lit
  state is not read: the renderer is presentation; a `Visibility` factor per area, authored or from level hints, is).
- **Hearing:** stimuli are events of any channel bound in the `.senses.json` by name, with a loudness (metres of
  hearing range) and a cue's `at` and `source` (`ohao/ecs/events.hpp:59-70`): `weapons.shot` 60 m, `physics.footstep`
  8 m (sprinting 14), `weapons.explode` 80 m, `damage.impact` 10 m. Occlusion halves the range per wall counted by the
  audio schedule's rule (one ray, graded by walls, `docs/notes/simulation.md:386-387`). The engine never names a
  game's channel: the binding is data, the audio bank's pattern.
- **Memory:** per agent, up to 8 known targets: last seen position and step, last heard position and step, awareness
  0-1, the source of knowledge (seen, heard, told). Saved (`Awareness`), hashed. **Told:** a group shares what one
  member saw, after a delay of 0.5-1.5 s (seeded by the event's identity), as radio chatter does.
- **Teams:** `Team {id}` and a hostility table in the data. Teammates are the same machinery on the other side.
- **Events:** `perception.noticed`, `perception.lost` with cues, which the audio bank binds to barks ("contact!").
- **Methods:** `perception.info {entity}`: what it knows, each fact's source, age and certainty, and for its current
  target why it can or cannot see it now (which ray stopped where).

### 3.3 The AI: `behavior`, `tactics` and `ai`

**State of the art, compared.**
- *Finite state machines* (the slice today): cheap, but transitions grow with states squared.
- *Behaviour trees* (Halo 2, Isla GDC 2005; Unreal's): hierarchical, readable, reactive by re-evaluating from the root;
  weak at choosing between many options by score.
- *Utility* (Dave Mark's Infinite Axis Utility System, GDC 2013-2015): every option scored by curves of inputs; good at
  choosing, hard to author sequences.
- *GOAP* (F.E.A.R., Orkin GDC 2006): a planner over actions with preconditions; its squad behaviours were largely
  barks over simple goals; planning cost grows with actions, and authored encounters are hard to direct.
- *HTN* (Killzone 2 and 3's bots, Horizon Zero Dawn, Guerrilla): authored decompositions with planning; strong for
  rich autonomous agents, heavy to author and to debug.
- *Position evaluation*: Killzone's (Straatman, Beij, van der Sterren, GDC 2005: waypoints with precomputed lines of
  fire, positions picked by weighted evaluation functions), The Last of Us's (McIntosh, Game AI Pro 2, 2015: combat
  positions sampled around the agent and scored with amortized ray tests), Unreal's EQS (generators, then tests that
  filter and score).

**Decided: behaviour trees for what to do, utility-scored queries for where to be.**
- A linear mission is directed: its encounters are authored, and the designer (an agent here) must be able to read
  why an enemy acted. A tree in JSON is diffable and traceable node by node; a plan is not. The choices that need
  scoring (which cover, where to flank, who to shoot) are queries the tree's leaves call, and a utility selector node
  picks a child by score where a tree needs one.
- **`behavior` (engine, depends only on ecs):** the runtime. A `.bt.json` of composites (sequence, selector, parallel,
  utility selector), decorators (cooldown, limit, condition on the blackboard, timeout in seconds), and leaves by name,
  which modules and games register. State per agent is integers (the running node's index per level, its start step),
  like the animator's (`docs/notes/simulation.md:300-307`); the blackboard is a fixed table of typed slots declared in
  the tree file (entity, vec3, float, int, bool). Saved, hashed. Re-evaluated from the root every 4 steps, staggered,
  and on events that interrupt (damage, a new target). Errors at JSON pointers, as the animation graph's.
- **`tactics` (engine):** cover points and queries.
  - Cover points are cooked with the navmesh: along every navmesh boundary edge that meets a wall, every 1 m, a point
    with the wall's normal, a height class (low: 0.9-1.3 m, crouch; high: 1.6 m and up, stand) and corner flags (the
    wall ends within 0.5 m: lean or step out). A destructible's points carry its entity, and die with its pieces.
  - A query (`.tactics.json`, named): a generator (cover points in a radius, a ring of navmesh samples, a post group),
    filters (reachable within a path length, not reserved by another agent, not in a friendly's line of fire), scores
    (distance to the threat in a band, protection from the threat's direction: the cover normal against it, and a ray
    from the threat's eye to the head height, flank angle against the group's mean bearing, path length, distance from
    friends), each a curve with a weight. Rays are the cost: at most 32 candidates and 64 rays a query, at most 4
    queries a step (staggered), batched on the pool.
  - **Explainable:** `tactics.query {name, agent}` returns each candidate, each filter that dropped it, each score.
- **`ai` (engine): the standard combatant.** Leaves: `move_to` (a navigation path), `face`, `fire_at` (a burst, the
  weapon's), `take_cover` (a query and a reservation), `peek`, `reload`, `throw` (a frag's arc solved for the target's
  last known position), `investigate`, `patrol` (a route of posts), `follow_posts` (teammates), `bark`. **Groups and
  tokens** (Doom 2016's attack tokens; Killzone's squad roles): an `AiGroup` hands out at most N shooting tokens, one
  flank token, one grenade every 6 s, so 12 enemies do not all fire or all flank.
- **The game's:** the trees (`assets/tidewall/ai/*.bt.json`: rifleman, flanker, patrol, teammate), their tuning, and
  any leaf a beat needs alone.
- **Animation:** the enemy lacks crouch and cover clips (section 1); the leaves drive the animator's existing states
  (aim, fire, move, hit, death) and a crouch layer when a clip exists. The clips are content: "Needs Frank".
- **Refuted, then kept.** *"HTN is the state of the art (Guerrilla); take it."* It is for agents whose behaviour must
  be planned; a linear mission's enemies run a handful of authored patterns, where HTN's cost is authoring and
  debugging, and the scored choice it would plan is the queries. HTN can join later as one more leaf (`plan`), so the
  door is cheap. *"A tree re-evaluated every step is the reactive ideal."* At 24 agents it is the cost that grows;
  every 4 steps plus interrupts reacts within 67 ms at 60 Hz, under the human reaction the enemy's own
  `reactionTime` 0.35 s models (`docs/specs/2026-10-27-combat-design.md` "Enemies").
- **Cost** (*estimates*, measured in A1-A3): the step with 24 active combatants under 2.0 ms at the default threads,
  navigation and perception and trees and queries together (the crowd's 64 run in 1 ms without any of them).
- **Methods:** `behavior.info {entity}` (the active path through the tree, each node's status, the blackboard),
  `ai.info {entity}` (its target, its reservation, its tokens), and together with `perception.info` and
  `tactics.query` an agent answers "why did it stand there".

### 3.4 `mission`

**Requirement:** an agent authors a mission's flow as data (what the player must do, what happens when, where the
checkpoints are), the engine runs it in the step, and it replays exactly from any checkpoint.

**State of the art.** IW's level scripts (GSC: triggers, flags, waits, spawners, color chains for friendlies),
Unreal's Level Blueprint and Sequencer, Halo's mission scripting (HSC, encounter AI with squads and zones). Taken:
triggers and flags, waits on conditions, spawners as data, friendlies moved by trigger-activated position groups
(the color-chain idea, as post groups). Not taken: a scripting language (a game is C++, system design requirement 6;
a mission is data plus the game's C++ for anything else), and runtime spawning (below).

**Decided.**
- **A `.mission.json`**, named by the saved singleton `Mission {asset}`: objectives (an id, text, state), **flags**,
  and **sequences**, each a list of steps: `wait` (seconds, a flag, a trigger entered by the player or a team, a group
  dead or below N, an event's cue by channel name, all of or any of), `signal` (a name), `objective` (show, complete,
  fail), `say` (a line: a sound in the bank and its subtitle), `checkpoint` (an id), `wake` (a dormant group), `music`
  (a mix snapshot, `requestMixSnapshot`), `end` (won, failed). Errors at JSON pointers; `ohao schema mission` exports
  its schema, as the audio bank's (`docs/notes/simulation.md:350-353`).
- **Other modules react to `mission.signal` by its cue's tag, bound in their own data** (a door's `openOn`, a post
  group's `activeOn`, a destructible's `breakOn`, an `AiGroup`'s `wakeOn`): mission depends on no module above
  physics, and none depends on mission.
- **State:** each sequence's step index and the step it began, the flags, the objectives' states: saved (`Mission`'s
  runtime half in a snapshot hook with `layout`), hashed.
- **Dormant actors, not runtime spawning.** Every enemy of the mission is in the level from the load, parked: its
  character out of the broadphase (`setCharacterInWorld(false)`, `physics_backend.hpp:570`, the dead enemy's path,
  `docs/notes/simulation.md:204-209`), not drawn, its tree off; `wake` puts it at its spawn point and turns it on. The
  World's entity set is then fixed for the whole mission, so every checkpoint's restore finds the same bodies and
  characters and restores the simulation exactly, the condition `restoreSimulation` needs
  (`ohao/physics/physics_scene.hpp:223-227`; "a new character would leave resting crates 3e-5 m off",
  `docs/notes/simulation.md:207-209`). Cost: about 70 parked characters, memory only (*estimate*).
  Refuted: *"CoD spawns on the fly; a level with 70 enemies parked is a hack."* Spawners as data are taken; creating
  entities at runtime is not, because exact restore across a changed body set is not solved and the mission does not
  need it. A prefab spawn joins when a level needs more actors than it can park (streaming).
- **Checkpoints** are whole-world snapshots through `WorldSnapshots`, taken by `World::defer` at the end of the step
  that reached them, kept in memory (the last two); the player's death restores the newest after a delay, as the round
  does (`games/slice/combat.cpp:420-477`). On disk: not in this design ("in or out", below).
- **For agents:** `mission.info` (objectives, flags, every sequence's step and what it waits on, the checkpoints
  reached); `mission.checkpoint {restore: id}`; `mission.start {at: id}` jumps to a checkpoint by applying its authored
  start (the earlier groups dead and parked, flags set), so a beat is tested without playing what comes before
  (its `world.hash` is a function of the file and the id, not equal to a played arrival's, said in the result).
- **Replay:** the mission is in the step and in the snapshot, so `input.record` and replay, and a checkpoint's restore
  in a new process, reach the same hashes; the part that builds it proves it (plan, part M1).

### 3.5 `destruction`

**Requirement** (ROADMAP 19, `docs/ROADMAP.md:80`): a wall breaks under fire and a grenade, its pieces rest, it opens
a path, and it replays bit for bit from a snapshot mid-collapse.

**State of the art.** Pre-fractured geometry held as a connection graph: NVIDIA Blast (support graph, a stress
solver, chunks released when unsupported), Unreal's Chaos geometry collections (clusters with strain, broken off by
damage, connection graphs to anchors), Battlefield's Frostbite destruction (authored stages). Real-time fracture
(Voronoi at impact) is rarer and nondeterministic in cost. Taken: pre-fracture at cook, a support graph with anchors,
damage per piece, release by connectivity. Not taken: Blast's stress solver (structural load paths; the mission's
walls are slabs and the BFS to an anchor gives the falls), runtime fracture.

**Decided.**
- **Clusters with a support graph, not breakable joints.** ROADMAP 19 says pieces are "held by 13's breakable
  joints". Refuted: a wall of 60 pieces needs about 150 fixed constraints, stiff and iterated; a 20-link chain already
  stretches 2.6 mm a joint (`docs/notes/simulation.md:126-127`), so a wall would sag and wobble, and the solver's cost
  is paid while it stands. Instead an intact cluster is one static compound body; a broken piece becomes its own
  dynamic body (`setMotionType`, `physics_backend.hpp:487`, or a parked body per piece made at load, decided by
  measurement in D1); after each break, a breadth-first walk of the support graph from the anchors releases every
  piece no longer connected. No constraint exists until something breaks. Breakable joints stay for hinged things
  (a door torn off, a plank). The ROADMAP's row is corrected in the same commit.
- **The fracture tool** (`tools/destruction/fracture.py`, ours): Voronoi cells of seeded points clipped by the piece's
  convex parts (a wall, a slab and a pillar are convex or a few `UCX_` hulls), interior faces with an `interior`
  material, each cell's convex hull its collider, its volume its mass, the support graph from shared faces, anchors
  where cells touch other static geometry. Run by the cooker on a node with the hint `frac: {cells, seed}`; cached by
  content. Taken from Blender's Cell Fracture: the method; not taken: Blender at cook (the cooker does not run
  Blender).
- **Entities fixed at load:** every piece is an entity from the start (the dormant rule): drawn as part of its cluster's
  instances, each its own BLAS once (one per piece, built at load; pieces move by TLAS updates only). Debris that has
  slept for 10 s and is out of view parks (hidden, out of the broadphase), never destroyed.
- **Damage:** `damage` events with a point and a radius (a round's hit: the piece hit loses health; a frag's or a
  charge's: every piece in the radius, by distance); a piece at 0 breaks; `destruction.break` with its cue (the audio
  bank and vfx set bind dust and crumble). Breaks are processed in id order in the step.
- **Navmesh:** a destructible is baked out of the navmesh and is an obstacle while more than a set share of its
  pieces (the hint's `open`, default 50%) of a doorway-sized region remain; past that the obstacle goes (section 3.1).
  Cover points on it die with their pieces.
- **Cost** (*estimates*, measured in D1): a wall of 60 pieces breaking under a charge, the step under 1.5 ms while the
  pieces fall, under 0.1 ms once they sleep; settled within 3 s.
- **Methods:** `destruction.info {entity}`: pieces, health, which are broken, support, the obstacle's state.

### 3.6 `doors`, `damage`, `weapons`, `ui`

- **`doors`:** a hinged or sliding kinematic body moved by the step (`moveKinematic`, so it pushes), opened by a
  signal's cue, a use action or a kick (a scripted fast swing), its navmesh obstacle present while closed.
- **`damage`:** `Health`, `HitZones` (a capsule's top 0.25 m is the head, x2; rounds today hit the capsule only,
  `docs/notes/simulation.md:334`), `damage.dealt`/`damage.death` events, radial damage with one occlusion ray to each
  candidate's chest, armour as a factor. Friendly fire off between teammates and the player (data).
- **`weapons`:** the slice's weapon model moved whole (`games/slice/components.hpp:27-62`, `combat.cpp`), plus: a
  reserve, two slots and a swap, thrown projectiles (`Thrown`: a frag that bounces, rolls and explodes on a fuse,
  ballistic in the step against `raycastFrom`, as the test grenade's flight is, `combat.cpp:355-395`), a weapon file
  (`.weapon.json`) of its tuning. Out: bullet penetration, launchers (no beat needs them).
- **`ui`:** text from an MSDF atlas cooked from a font file (the font is a download: "Needs Frank"; the 5x7 bitmap
  font stays as the fallback, `hud_list.hpp:38-41`), panels, a menu stack navigated by input actions (pause: resume,
  restart from checkpoint, quit; the end screen), subtitles from `mission.say`. Drawn by the HUD pass. Reflection
  never runs per frame: layout is code (the game's) over the module's list, never data interpreted per frame.

## 4. What else a CoD-like mission needs: in or out

| Need | Decision | Why |
|---|---|---|
| Teammates (2) | **in** | the genre's core; the same AI on the other team; invulnerable (no failure that teaches nothing) |
| Death ragdolls (ROADMAP 13 step 2) | **in, pulled** | already specified (`docs/specs/2026-11-02-ragdoll-design.md`); a corpse that stands upright reads as a bug at the bar |
| Frag grenades, both sides | **in** | B2's flush-out; `weapons` + `damage` |
| ADS, recoil, spread | **in, exists** | `docs/notes/simulation.md:346-349` |
| Weapon swap, reserve ammo, pickups | **in** (swap, reserve); pickups **in** as dropped magazines | a 13-minute mission needs ammo economy |
| Bullet penetration | **out** | no beat depends on it |
| Melee, takedowns | **out** for the first build | B1 works with suppressed fire; the clips do not exist |
| Vehicles, turrets | **out** | no beat |
| Cinematics, cutscenes | **out** | first-person scripted moments only (the genre's practice); camera shake is the game's |
| Menus | **in, minimal** | pause, restart from checkpoint, quit, the end screen; no settings menu (`render.settings` exists for agents) |
| Subtitles, dialogue | **in** | `mission.say`; lines are content ("Needs Frank": voice) |
| Save at checkpoints, in memory | **in** | snapshots |
| Save to disk, continue later | **out** | snapshot hooks hold runtime state with no file format; a 13-minute mission is played in one sitting; the day a save is needed it is a feature of its own (a snapshot codec) |
| Level streaming | **out**, measured | the level fits (section 6's memory budget); measured at M4, reopened only if it misses |
| Gamepad, aim assist | **out** | keyboard and mouse first |
| Difficulty levels | **out** | one tuning, then by need |
| Weather (rain) | **in as presentation** | rain on the hero room's windows and a mist over the crest: vfx data, no simulation |

## 5. The high-quality part (decision 3)

**Decided: one room, B3's night-shift kitchen and break room** (about 9 x 7 x 3 m, with a corridor), at the bar,
everything else blockout.

- **Why a room and not the weapon or a character:** the arms and rifle are on screen all mission and would show more,
  but no asset exists and making one is sculpting and texturing work an agent cannot do to AAA today (generative
  tools are Frank's ruling, below); a character is the same, with skinning content too. A room is where real assets
  exist: Frank's Somnabulate props (a kitchen table, chairs, a book stack, a kettle are staged; 78 LOD0 props in the
  source, `docs/specs/2026-10-26-level-pipeline-design.md:144-149`), which fit a staff kitchen, and it exercises what
  the renderer is for: practical lights, emissive screens, glass, metal, wood, a dark interior lit by GI.
- **From which assets:** the props from `assets/local/` (staged by `tools/level/stage_flat42.py`, extended to the
  room's list); walls, floor, ceiling, cabinets and windows built in Blender by a script of ours with real PBR
  materials (from CC0 texture libraries, or our own procedural ones if Frank declines the download). **The hero room
  is local-only** (Frank's assets are never committed, AGENTS.md); the committed level has the same nodes as blockout,
  so the mission, the gates and other agents never depend on it.
- **What the renderer must do for it** (each checked in part H1, not assumed): texture mip bias under render scale (not
  done, `docs/notes/renderer.md:555`); RT GI in a dark interior lit by a few small sources (noise and the radiance
  cache's leaks through thin walls); the moon through blinds (ReSTIR DI on a directional light through small
  apertures); emissive triangles as lights (the television, `docs/notes/renderer.md:357-363`); glass with rain decals
  (transmission exists; decals are raster only, `docs/notes/simulation.md:468`).
- **"High quality" is measured three ways:**
  1. **Cost:** `render_bench tidewall_hero_1080p`, the frame within the mission's budget (section 6) at 1920x1080.
  2. **The referee:** at three fixed views, deferred against the path tracer within half a stop (the frontier's bar,
     `docs/ROADMAP.md:79`), the method of `docs/changes/2026-10-29-referee.md`.
  3. **Frank's look:** a hand-test list (a capture at each view, one visible window run), which closes it.
- **Generative content tools** (meshes, textures, motion, speech, music: the session lists a NIUA service with mesh,
  rig, motion, speech and music generation) are an **option for Frank to rule**, not a decision: their licences,
  their cost, whether their output may be committed, and whether "AAA by AI" is meant to include them.

## 6. The level (decision 4)

- **Size:** a route of about 350 m through a footprint of about 300 x 120 m: the culvert (60 m), the yard (60 x 40
  m), the staff block (two floors, 20 x 12 m), the sluice hall (40 x 24 x 12 m), the crest (120 x 12 m). About 70
  enemies over the mission, at most 24 awake (*estimate*).
- **How it is built:**
  - **A kit of parametric pieces** in Blender, by script (`tools/level/kit/`, ours): walls with openings, floors,
    stairs, ramps, low (1.0 m) and high (1.6 m) cover, containers, pipe racks, catwalks, doors, windows; each with its
    hints (`col`, `surf`, `nav*`, `frac`, `door`) set by the kit, never by hand.
  - **A layout file is the source** (`games/tidewall/level/layout.json`: pieces, their transforms and parameters,
    spawns, triggers, posts), turned into the `.glb` by `tools/level/build_level.py` (headless `bpy`, as
    `blockout_room.py` is, `tools/level/blockout_room.py:133-135`), byte-identical for the same layout. An agent
    edits the layout (a file it can diff), rebuilds (seconds, *estimate*), and the session's `level.reload` re-expands
    it in place (`docs/notes/simulation.md:45-53`); the navmesh's changed tiles recook.
  - **What lives in the scene, not the level:** enemies, teammates, groups, the mission, posts: entities an agent
    places through `world_apply` in a live session and saves.
- **Budgets** (the measured values replace the estimates when the part that adds each measures it; none is loosened):

| Budget | Value | Bench | Added by |
|---|---|---|---|
| The frame at 1920x1080, GPU, at every beat's fixed view | 11.1 ms (90 fps) at render scale 0.667 *estimate* target | `render_bench tidewall_<beat>_1080p` | M0, H1 |
| The step with 24 combatants awake, default threads | 2.0 ms | `physics_bench tidewall_b4_step_ms` (the hold) | A3 |
| Navmesh cook, whole level, cold | 5 s | `level_bench --only tidewall_nav` | N1 |
| Navmesh load | 20 ms | the same | N1 |
| A path query, p95 | 50 us | the same | N1 |
| An obstacle change | 2 ms | the same | N1 |
| A wall of 60 pieces breaking | 1.5 ms a step while falling | `physics_bench tidewall_wall_step_ms` | D1 |
| Level load to first frame | 1.0 s | `asset_bench` | M0 |
| Memory | 2 GB resident, 4 GB VRAM (the 4060 has 8) | `ohao run --frames` reports | M4 |

The render budgets' file is written for an RTX 5070 Laptop (`tests/bench/render_budgets.json:2`); the mission's are
measured and set on this machine's 4060, and the file says so.

## 7. Risks

- **Recast's float results across builds:** a compiler upgrade can move a vertex and so a path, as a Jolt upgrade
  moves a hash; the cooked navmesh is keyed by the cooker's version, and replay holds within one build only, as today.
- **AI cost grows with what agents author:** an agent that writes a query of 500 candidates. Every query and tree is
  capped in its file's schema (candidates, rays, depth), refused at load, never clamped silently.
- **Content, not code, is the long pole:** crouch and cover clips, voice lines, the hero room's dressing. Each is
  listed for Frank; the mission plays without them (stand-ins), and each lands as data.
- **The hash change of extracting `weapons` and `damage`:** every `world.hash` and the replay expectations move once,
  named in that commit, as R2's did.
- **Dormant actors' memory:** 70 parked characters plus fragments; measured at M4 against the memory budget.
- **The bar:** "AAA" is judged by Frank on one room; the referee and the budget make it measurable, the look does not.

## 8. Needs Frank

**Ruled (Frank, 2026-11-18):** the downloads 1, 3 and 4 approved (Recast/Detour v1.6.0, Inter, CC0 textures from
ambientCG / Poly Haven); 2, the name and premise, kept; 5-7 (animation clips, generative tools, voice lines): later.

1. **Recast/Detour:** `recastnavigation` **v1.6.0** (the latest release, 21 May 2023, on its releases page; zlib licence, C++98,
   CMake), fetched as Jolt is (`external/cmake/jolt.cmake:36-41`: a tag zip,
   `https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.zip`), with its demo and tests off,
   only Recast, Detour, DetourCrowd (for `dtObstacleAvoidanceQuery` and `dtPathCorridor`) and DetourTileCache built,
   and a `URL_HASH SHA256` (no pin in the tree has one today; this one should, and the others could follow). The
   approval: the download and the dependency.
2. **The name and premise** (*Tidewall*, section 2): original as far as searched; Frank's to keep or rename.
3. **The hero room's assets:** which Somnabulate props to stage (local only); CC0 texture libraries (ambientCG, Poly
   Haven) as a download, or our procedural materials.
4. **A font for `ui`:** an OFL font (Inter or Noto Sans) as a download, cooked to MSDF.
5. **Animation content:** crouch, cover, peek, throw and vault clips for the enemy (the Quaternius pack has none):
   another CC0 pack, a paid licence (not committable), generated motion, or Frank's own.
6. **Generative content tools** (meshes, textures, motion, speech, music): whether, which, and whether their output may
   be committed.
7. **Voice lines:** synthesized (ours, as the sounds are), generated speech, or recorded.
8. **Costly to undo:** none of the code decisions are (local commits). The hash change of extracting `weapons` and `damage` (section 3) is the standard's
   (named, with its reason), recorded for Frank to see, not asked.
