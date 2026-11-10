# Writing a game for OHAO

For a game developer (an agent, or a person): what a game is, what it may call, its life on a World, and the edit
loop. The engine's own side is `AGENTS.md` and `docs/ARCHITECTURE.md`; a game needs neither. The smallest whole game
is `games/sample/sample.cpp` (targets that spin until shot, then fall): read it first.

## A game is a module

- A directory `games/<name>/` whose `CMakeLists.txt` is `ohao_add_game(<name>)` (cmake/ohao_games.cmake), and a line
  `add_subdirectory(games/<name>)` in the root `CMakeLists.txt`. It builds `build/Release/libohao_<name>.so`.
- Every host plays one game: `ohao <command> --game build/Release/libohao_<name>.so` (the CLI, `ohao serve`),
  `--game slice` (the default), or `--game none` (an engine-only world).
- The library exports one function, `ohao_game_module`, returning its `game::Module` (`game/module.hpp`):

| | What it does |
|---|---|
| `name()` | The game's name: its channels' and methods' prefix, and what the catalogue calls it |
| `registerTypes(r)` | Its components (`r.type<T>("Name").component().host(makeComponentOps<T>())`, each field with a doc, a unit and limits) and its events' payloads (`.event()`; entities by id, `.entity()`), after the engine's |
| `install(world)` | Its systems, channels, drivers, rules, snapshot hooks and presentation, on a World with the engine installed. A second call does nothing |
| `uninstall(world)` | Only what the engine cannot see: its context entries (`registry().ctx()`) and its registry signals. The engine removes the rest itself |
| `methods()` | Its API methods (`game::Method`: a doc row, a handler `bool(Call&, params, ctx, out)`, `mutates`), after the engine's. Their params types live in the game's own `reflect::TypeRegistry`; decode them with `game::decodeParams` |

- A type's name must not be an engine type's, nor a method's an engine method's: the registry and the session refuse.

## What a game includes

`#include "game/api.hpp"`, its own headers (`"<name>/..."`), the standard library, glm and EnTT. Nothing else of the
engine: configure fails on any other engine header, naming the file. `game/api.hpp` lists what it gives, among them:

- the World (`ecs/world.hpp`): systems, events, `defer`, the clock, input per player; `playerOf`;
- the engine's components (`Transform`, `PlayerController`, `RigidBody`, `Spin`, ...);
- the physics scene (`raycastFrom`, `playerInfo`, `turnPlayer`, `setInputControl`);
- animation drivers, the audio mix (`requestMixSnapshot`), snapshot hooks, `WorldSnapshots`;
- presentation (`addHud`, `addSpheres`).

## The rules the engine holds a game to

- **Systems:**
  - `world.addSimulate(name, fn, {.after = {...}, .before = {...}})` for behaviour, once per fixed step;
    `addSync` for derived state, before a render.
  - A game names the engine's systems in its order (`after physics`), never the reverse.
  - The order is computed (ties by name), and a cycle throws at install.
- **Events:**
  - `world.events().writer<T>("<name>.<what>", "<system>", &cue)`. The cue is the projection the engine's data binds:
    an audio bank's or a vfx set's `events` play or spawn by channel name.
  - Readers take `world.events().reader<T>(channel, "<system>")`, and a step reads its own events.
  - The engine never includes a game's payload types.
- **State:**
  - What a later step reads is in components (saved, and hashed by `world.hash`), or in a snapshot hook: `save`,
    `restore`, `hash`, and `layout = layoutKey<State>()`.
  - A reload carries a hook's state only with its layout declared.
- **Time and input:**
  - Time is the World's: `world.stepDt()`, `world.steps()`; durations in data are seconds or milliseconds.
  - Input is read in simulate systems (`world.input(stream).pressed(...)`), never once per frame.
  - The player of a stream is `playerOf(world, stream)`.
- **Writes and restores:**
  - `Transform`, `MeshRenderer` and `Light` change through `registry.patch` or `replace`.
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
