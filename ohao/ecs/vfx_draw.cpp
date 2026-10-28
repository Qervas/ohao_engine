#include "ecs/vfx_draw.hpp"

#include "ecs/asset_resolver.hpp"
#include "ecs/vfx.hpp"
#include "ecs/world.hpp"
#include "gpu/shared_layout.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <memory>

namespace ohao::ecs {

render::VfxFrame vfxFrameOf(const World& world, std::shared_ptr<const vfx::VfxSet> set, std::vector<uint32_t> textures) {
    render::VfxFrame frame;
    const VfxSystem* system = vfxSystem(world);
    if (!system || !set) return frame;
    frame.set = std::move(set);
    frame.setVersion = system->setVersion();
    frame.textures = std::move(textures);
    const uint64_t steps = system->steps();
    const float dt = system->stepDt();
    const float alpha = world.renderAlpha();
    for (const VfxRecord& r : system->records()) {
        if (r.kind != VfxRecord::Kind::Effect || r.effect >= frame.set->effects.size()) continue;
        // Integer steps first: the same age for the same (steps, spawnStep, alpha) in every host.
        const auto whole = static_cast<float>(static_cast<int64_t>(steps) - static_cast<int64_t>(r.spawnStep) - 1);
        render::VfxFrameEffect e;
        e.effect = r.effect;
        e.seed = r.seed;
        e.age = (whole + alpha) * dt;
        e.pose = r.pose;
        e.id = r.seq;
        frame.effects.push_back(e);
    }
    return frame;
}

namespace {

struct VfxDraw {
    render::RenderScene& scene;
    BindlessTextureManager* textureManager;
    AssetResolver& assets;
    uint64_t version{0};
    std::shared_ptr<const vfx::VfxSet> set;
    std::vector<uint32_t> textures;

    void refresh(const VfxSystem& system) {
        if (set && version == system.setVersion()) return;
        version = system.setVersion();
        set = std::make_shared<const vfx::VfxSet>(system.set());
        textures.assign(set->textures.size(), gpu::shared::kVfxNoTexture);
        if (!textureManager) return;
        for (std::size_t i = 0; i < set->textures.size(); ++i) {
            const auto path = assets.absolutePath(AssetRef{set->textures[i].file, {}});
            // Flipbook cells are sampled at mip 0 (a mip chain would bleed neighbouring cells).
            const BindlessTextureHandle h =
                path.empty() ? kInvalidBindlessTexture
                             : textureManager->loadTexture(path.string(), BindlessTextureType::Albedo, false);
            if (h.valid()) textures[i] = h.index;
            else OHAO_LOG(Warn, vfx, "vfx: texture '{}' not loaded: its emitters draw a soft dot", set->textures[i].file);
        }
    }

    void run(World& world) {
        const VfxSystem* system = vfxSystem(world);
        if (!system) {
            if (!scene.vfx().effects.empty() || scene.vfx().set) scene.setVfx({});
            return;
        }
        refresh(*system);
        scene.setVfx(vfxFrameOf(world, set, textures));
    }
};

} // namespace

void installVfxDraw(World& world, render::RenderScene& scene, BindlessTextureManager* textures, AssetResolver& assets) {
    auto state = std::make_shared<VfxDraw>(VfxDraw{scene, textures, assets});
    const auto names = world.syncNames();
    const bool extract = std::find(names.begin(), names.end(), "render_extract") != names.end();
    if (extract) world.addSyncAfter("render_extract", "vfx_draw", [state](World& w) { state->run(w); });
    else world.addSync("vfx_draw", [state](World& w) { state->run(w); });
}

} // namespace ohao::ecs
