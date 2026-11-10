#include "engine/install.hpp"

#include "ecs/animation.hpp"
#include "ecs/audio_schedule.hpp"
#include "world_render/instantiate.hpp"
#include "ecs/physics_scene.hpp"
#include "world_render/render_extract.hpp"
#include "ecs/vfx.hpp"
#include "world_render/vfx_draw.hpp"
#include "level/level_system.hpp"

namespace ohao::engine {

void installEngine(ecs::World& world, const EngineConfig& config) {
    level::installLevels(world, config.assets, config.types);
    ecs::installPhysics(world, config.assets);
    ecs::installAnimation(world, config.assets);
    ecs::installAudio(world, config.assets);
    ecs::installVfx(world, config.assets);
}

void installRendering(ecs::World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                      ecs::AssetResolver& assets) {
    ecs::installInstantiate(world, scene, textures, assets);
    ecs::installRenderExtract(world, scene);
    ecs::installVfxDraw(world, scene, textures, assets);
}

} // namespace ohao::engine
