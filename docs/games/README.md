# Writing a game for OHAO

For a game developer (an agent, or a person): what a game is, what it may call, its life on a World, and the edit
loop. The engine's own side is `AGENTS.md` and `docs/ARCHITECTURE.md`; a game needs neither. The smallest whole game
is `games/sample/sample.cpp` (targets that spin until shot, then fall): read it first.

## A game is a module

- A directory `games/<name>/` whose `CMakeLists.txt` is `ohao_add_game(<name>)` (cmake/ohao_games.cmake), and a line
  `add_subdirectory(games/<name>)` in the root `CMakeLists.txt`. It builds `build/Release/ohao_<name>.dll`
  (`libohao_<name>.so` on Linux).
- Every host plays one game: `ohao <command> --game build/Release/ohao_<name>.dll` (the CLI, `ohao serve`),
  `--game slice` (the default), or `--game none` (an engine-only world).
- The library exports one function, `OHAO_GAME_ENTRY const ohao::game::Module* ohao_game_module()`, returning its
  `game::Module` (`game/module.hpp`). That is `ecs::Module` (`ecs/module.hpp`), the interface every engine module
  has too:

| | What it does |
|---|---|
| `name()` | The game's name: its channels' and methods' prefix, and what the catalogue calls it |
| `dependencies()` | The engine modules it uses, by name (the sample's: `physics`; the slice's: `physics`, `animation`, `audio`, `vfx`, `world_render`, `visual`). A World that installs none of one refuses the game, naming what is missing |
| `registerTypes(r)` | Its components (`r.type<T>("Name").component().host(makeComponentOps<T>())`, each field with a doc, a unit and limits) and its events' payloads (`.event()`; entities by id, `.entity()`), after the engine's |
| `install(world, services)` | Its systems, channels, drivers, rules, snapshot hooks and presentation, on a World with the engine's modules installed (`services`: the assets and types they were given). A second call does nothing |
| `uninstall(world)` | Only what the engine cannot see: its context entries (`world.context()`) and its registry signals (`world.disconnect<C, &fn>()`). The engine removes the rest itself: systems, snapshot hooks, and what it added to each module (animation drivers, presenters, the physics control rule) |
| `methods()` | Its API methods (`game::Method`: a doc row, a handler `bool(Call&, params, ctx, out)`, `mutates`), after the engine's. Their params types live in the game's own `reflect::TypeRegistry`; decode them with `game::decodeParams` |

- A type's name must not be an engine type's, nor a method's an engine method's: the registry and the session refuse.

## What a game includes

`#include "game/api.hpp"`, its own headers (`"<name>/..."`), the standard library, glm and EnTT. Nothing else of the
engine: configure fails on any other engine header, naming the file. `game/api.hpp` lists what it gives, by module:

- the core (`ecs/`): the World (`world.hpp`: systems with `SystemDecl`, `SystemContext` and `types<>()`, signals,
  events, `defer`, the clock, input per player), its components (`Transform`, `Parent`, `Spin`, `SpawnPoint`, input),
  channels (`events.hpp`), snapshot hooks, `WorldSnapshots`, `makeComponentOps`;
- physics: its components (colliders, bodies, joints, `PlayerController`), its channels, the physics scene
  (`raycastFrom`, `playerInfo`, `turnPlayer`, `setInputControl`), `playerOf`;
- animation: `Animator`, `SkinnedMesh`, `JointAttach`, drivers (`AnimationSystem::setDriver`);
- audio: the mix a game asks for (`requestMixSnapshot`);
- visual: what an entity looks like (`MeshSource`, `Material`, `Light`, `Camera`) and the scene view;
- world_render: what a game presents (`addHud`, `addSpheres`);
- `jobs::parallelFor` (`core/jobs/graph.hpp`), `layoutKey<T>()`, `OHAO_LOG`, and reflection for methods.

## The rules the engine holds a game to

- **Systems declare what they touch:**
  - `world.addSimulate(name, fn, decl)` for behaviour, once per fixed step; `addSync` for derived state, before a
    render. `fn` is `void(SystemContext&, float dt)` (a sync system's has no dt).
  - `decl` is an `ecs::SystemDecl`: `.after`/`.before` (a game names the engine's systems, `after physics`, never the
    reverse), `.readsCues`, `.reads` and `.writes` (`ecs::types<PlayerController, Target>()`), `.readsResources` and
    `.writesResources` (named state that is not a component: `"physics.scene"`), and `.exclusive = true` for a system
    that creates or destroys entities or calls a subsystem with registry use of its own. The sample's `shoot`:
    `{.after = {"physics"}, .reads = types<PlayerController, Parent, Target>(), .writes = types<Target, Spin,
    RigidBody>(), .readsResources = {"physics.scene"}}`.
  - Every system declares something. Install throws on a declaration of nothing, on a cycle, and on two writers of one
    component or resource with no order between them (name the order with `after` or `before`).
  - A system reaches components through its `SystemContext`, with EnTT's names: reads are `get`, `try_get`,
    `all_of`, `any_of`, `view<const A>()`; writes are `patch`, `replace`, `emplace`, `emplace_or_replace`, `remove`,
    `view<B>()` with a mutable type, and `get_mut`/`try_get_mut` to mutate in place. `ctx.world()` gives the World,
    `ctx.idOf(e)` an entity's id. An exclusive system may use `world.registry()`; any other gets a throw from it in
    the check mode.
  - Consecutive systems that conflict in nothing run as one wave, those of several on the job pool. Their events wait
    in each system's stage and are flushed in the serial order, so `world.hash` is the same at any thread count.
    Inside a wave of several, `emit` returns 0: the seq is taken at the flush, so never keep it.
  - A signal's handler: `world.onConstruct<C, &fn>(effect)` (or with an instance, `onConstruct<C, &Class::fn>(this,
    effect)`; `onUpdate`, `onDestroy`), where `effect` is the `SignalEffect` it writes (`{.writes = types<X>()}`); it
    joins the access of every system that writes `C`. `world.disconnect<C, &fn>()` in `uninstall`. A raw EnTT
    connection is refused by `tools/check.py`.
  - Per-entity work over many entities is a `jobs::parallelFor(begin, end, grain, fn)`: write per index, reduce in
    index order after it, never add a float atomically.
- **Checking a game's declarations** (environment variables, read when a World is made):
  - `OHAO_ACCESS_CHECK=1`: every context call is checked against the declaration; an access outside it throws, naming
    the system and the component.
  - `OHAO_WAVE_ORDER=reverse`: each wave runs backwards; a dependency the declarations miss (a static, a context
    entry) shows as a different `world.hash`. Run a scene at `--threads 1` and at the default, reversed, and compare.
  - `OHAO_WAVE_INLINE_US=0`: every system of a wave but its longest goes to a worker (it exists to measure; by
    default a system under 5 us runs on the calling thread).
- **Events:**
  - `world.events().writer<T>("<name>.<what>", "<system>", &cue)`. The cue is the projection the engine's data binds:
    an audio bank's or a vfx set's `events` play or spawn by channel name.
  - Readers take `world.events().reader<T>(channel, "<system>")`, and a step reads its own events.
  - The engine never includes a game's payload types.
- **State:**
  - What a later step reads is in components (saved, and hashed by `world.hash`), or in a snapshot hook: `save`,
    `restore`, `hash`, and `layout = layoutKey<State>()`.
  - A reload carries a hook's state only with its layout declared.
  - The World's context entries are `world.context()` (never `registry().ctx()`): a game's own, erased in
    `uninstall`.
- **Time and input:**
  - Time is the World's: `world.stepDt()`, `world.steps()`; durations in data are seconds or milliseconds.
  - Input is read in simulate systems (`world.input(stream).pressed(...)`), never once per frame.
  - The player of a stream is `playerOf(ctx, stream)` in a system, `playerOf(world, stream)` elsewhere.
- **Writes and restores:**
  - `Transform`, `MeshRenderer` and `Light` change through `patch` or `replace`.
  - Nothing snapshots or restores the world inside a step: ask with `world.defer`.

## The edit loop: `game.reload`

1. Edit the game, then `cmake --build build --config Release --target ohao_<name>`: 1 to 4 s for one file, with the
   game API precompiled.
2. Call `game.reload` (MCP `game_reload`): about 4 ms. The running session takes the new build between steps, the
   world kept: `world.hash` is unchanged.

What crosses is read by the new build in place: the game's components, its channels' events, and its snapshot
hooks' states. So a build that changes the layout of one of its types or of a hook's state is refused (`E_RELOAD`),
the old build left playing: start a new session for it. A changed doc or limit applies from the next session. Each
reload keeps the old build mapped (EnTT cannot remove a component pool), about the library's size.

## What an agent reads

- `ohao schema` (or `schema.get`): the catalogue. Every type, enum, method and channel a game owns carries
  `"module": "<name>"`.
- `events.read {names: ["<name>.*"]}`: the game's events, each field by name, entities as id and name.
- `docs/agents/ohao-cli.md` and `ohao-serve.md`: driving a session.
