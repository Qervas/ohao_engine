#pragma once
// The engine's modules (step 4b-6b; ecs/module.hpp) and their one install (system design 4.2): every host registers
// the modules' types (registerTypes) and installs them on each World (install), then its game's. Where each system
// runs is computed from what the systems declare (ecs::SystemDecl), not the call order, so hosts differ only by which
// modules they install. world_render needs the host's renderer (render::RenderScene among the services, and its
// BindlessTextureManager when it has one): a window host gives it at the first call, a headless one installs again
// with it at its first capture; until then world_render waits on the World (ecs::waitingModules).
#include <initializer_list>
#include <span>
#include <string_view>

namespace ohao {
namespace reflect { class TypeRegistry; }
namespace ecs {
class World;
class Module;
class Services;
} // namespace ecs
} // namespace ohao

namespace ohao::engine {

// Every engine module: ecs, visual, animation, physics, audio, footsteps, vfx, level, world_render.
[[nodiscard]] std::span<const ecs::Module* const> modules();

// Every engine module's types, in module order, each under its name (TypeInfo::module). A game's follow
// (game::registerGameTypes).
void registerTypes(reflect::TypeRegistry& types);

// Installs the engine's modules on `world`, in module order: every one, or the core, the ones `only` names (a bench's:
// what it measures) and every module they depend on. An unknown name throws std::logic_error. A second call installs
// only what the first did not, and what waited for services this call has. No game system: a host that plays a game
// installs it after (game::installGame).
void install(ecs::World& world, const ecs::Services& services, std::initializer_list<std::string_view> only = {});

} // namespace ohao::engine
