#pragma once
// The engine's modules (step 4b-6b; ecs/module.hpp) and their one install (system design 4.2): every host registers
// the modules' types (registerTypes) and installs them on each World (install), then its game's. Where each system
// runs is computed from what the systems declare (ecs::SystemDecl), not the call order, so hosts differ only by which
// modules they install. installRendering adds what a renderer needs once a host has one (at once in a window, at the
// first capture headless).
#include <initializer_list>
#include <span>
#include <string_view>

namespace ohao {
class BindlessTextureManager;
namespace reflect { class TypeRegistry; }
namespace render { class RenderScene; }
namespace ecs {
class World;
class AssetResolver;
class Module;
class Services;
} // namespace ecs
} // namespace ohao

namespace ohao::engine {

// Every engine module: ecs, visual, animation, physics, audio, vfx, level, world_render.
[[nodiscard]] std::span<const ecs::Module* const> modules();

// Every engine module's types, in module order, each under its name (TypeInfo::module). A game's follow
// (game::registerGameTypes).
void registerTypes(reflect::TypeRegistry& types);

// Installs the engine's modules on `world`, in module order: every one, or the core and the ones `only` names (a
// bench's: what it measures; a module's systems never need another's installed, ecs::installModules). An unknown
// name throws std::logic_error. A second call installs only what the first did not. No game system: a host that plays
// a game installs it after (game::installGame).
void install(ecs::World& world, const ecs::Services& services, std::initializer_list<std::string_view> only = {});

// Instantiate (MeshSources into meshes), the render extract, the effects' draw: what a renderer needs.
void installRendering(ecs::World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                      ecs::AssetResolver& assets);

} // namespace ohao::engine
