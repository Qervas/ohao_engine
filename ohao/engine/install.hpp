#pragma once
// The one install of a World's systems (system design 4.2, step 2b). Every host (the window, the CLI, ohao serve,
// the benches) calls installEngine, and installRendering once a renderer exists (at once in a window, at the first
// capture headless). Where each system runs is ecs/system_order.hpp's, not the call order, so hosts differ only by
// what they install.
namespace ohao {
class BindlessTextureManager;
namespace reflect { class TypeRegistry; }
namespace render { class RenderScene; }
namespace ecs {
class World;
class AssetResolver;
} // namespace ecs
} // namespace ohao

namespace ohao::engine {

struct EngineConfig {
    ecs::AssetResolver& assets;
    const reflect::TypeRegistry& types;
};

// Levels, physics, animation, the audio schedule, vfx: the engine's simulation and its snapshot hooks. No game
// system: a host that plays the slice calls ecs::installSlice (ecs/slice.hpp) after it.
void installEngine(ecs::World& world, const EngineConfig& config);

// Instantiate (MeshSources into meshes), the render extract, the effects' draw: what a renderer needs.
void installRendering(ecs::World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                      ecs::AssetResolver& assets);

} // namespace ohao::engine
