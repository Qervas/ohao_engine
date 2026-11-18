#pragma once
// The game API (step 3i; system design 4.4): what a game is to the engine. A game is a Module (ecs/module.hpp), the
// engine modules' own interface: the types it adds, what it installs on a World, and its API methods. A host plays one
// (`--game`, default the slice) or none (an engine-only world); no engine library names a game
// (cmake/ohao_layers.cmake puts games above every one). A shared library the hosts load (game/loader.hpp).
//
// A game calls the engine through the engine's own headers, never its internals:
//   - the World (ecs/world.hpp): systems with their declarations (SystemDecl), events with their cues (EventLog::writer),
//     defer, the clock (steps, stepDt), input per player; physics/player.hpp's playerOf;
//   - the physics scene (physics/physics_scene.hpp): raycasts, players, turnPlayer, setInputControl;
//   - animation drivers (animation/animation.hpp: AnimationSystem::setDriver), the audio mix (requestMixSnapshot);
//   - state a step reads (ecs/snapshot_hook.hpp), the whole world kept and restored (ecs/world_snapshots.hpp);
//   - what it presents (world_render/present.hpp: HUDs, spheres).
//
// Its install(World&, const Services&) runs on a World the engine's modules are installed on (engine::install), with
// the services they were given; a second call on one World does nothing. Hosts call game::installGame
// (game/install.hpp), which records what it added. Its uninstall undoes what install made that the engine cannot
// see: its context entries, its registry signals. The engine removes the rest itself (game::uninstallGame): systems,
// snapshot hooks, drivers, presenters, the control rule. After it the World holds none of the module's objects but
// its components' data and its channels' events, which the next build of it reads in place (a reload). Its methods
// follow the engine's in the catalogue.
#include "ecs/module.hpp"

namespace ohao::game {

using Module = ecs::Module;
using Method = ecs::Method;
using Call = ecs::Call;
using MethodFn = ecs::MethodFn;
using ecs::decodeParams;

} // namespace ohao::game
