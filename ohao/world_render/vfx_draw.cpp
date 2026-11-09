#include "world_render/vfx_draw.hpp"

#include "ecs/asset_resolver.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/vfx.hpp"
#include "ecs/world.hpp"
#include "gpu/shared_layout.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <memory>

namespace ohao::ecs {

namespace {

// What `e` is drawn at: between steps (RenderPose) or at the step's pose. A decal follows its
// entity this way, as the impact marks did.
glm::mat4 drawnWorld(const entt::registry& reg, entt::entity e) {
    if (const auto* rp = reg.try_get<RenderPose>(e)) return rp->world;
    if (const auto* wt = reg.try_get<WorldTransform>(e)) return wt->world;
    if (const auto* t = reg.try_get<Transform>(e)) return t->matrix();
    return glm::mat4(1.0f);
}

// A flash as a light of the raster table: LightKind::Flash (a light with no mesh, which is what a
// flash is), whose range the table takes as its radius times 10 (light_table.cpp). Shadowless.
std::vector<render::LightDesc> flashLights(const render::VfxFrame& frame) {
    std::vector<render::LightDesc> out;
    out.reserve(frame.lights.size());
    for (const render::VfxFrameLight& l : frame.lights) {
        render::LightDesc d{};
        d.kind = render::LightKind::Flash;
        d.position = l.position;
        d.color = l.color;
        d.intensity = l.intensity;
        d.radius = l.range / 10.0f;
        d.castShadows = false;
        d.order = l.id;
        out.push_back(d);
    }
    return out;
}

} // namespace

render::VfxFrame vfxFrameOf(const World& world, std::shared_ptr<const vfx::VfxSet> set, std::vector<uint32_t> textures,
                            std::vector<uint32_t> normals) {
    render::VfxFrame frame;
    const VfxSystem* system = vfxSystem(world);
    if (!system || !set) return frame;
    frame.set = std::move(set);
    frame.setVersion = system->setVersion();
    frame.textures = std::move(textures);
    frame.normals = std::move(normals);
    const uint64_t steps = system->steps();
    const float dt = system->stepDt();
    const float alpha = world.renderAlpha();
    const auto& reg = world.registry();
    for (const VfxRecord& r : system->records()) {
        // Integer steps first: the same age for the same (steps, spawnStep, alpha) in every host.
        const auto whole = static_cast<float>(static_cast<int64_t>(steps) - static_cast<int64_t>(r.spawnStep) - 1);
        const float age = (whole + alpha) * dt;
        switch (r.kind) {
        case VfxRecord::Kind::Effect: {
            if (r.effect >= frame.set->effects.size()) break;
            render::VfxFrameEffect e;
            e.effect = r.effect;
            e.seed = r.seed;
            e.age = age;
            e.pose = r.pose;
            e.id = r.seq;
            frame.effects.push_back(e);
            if (const auto& proxy = frame.set->effects[r.effect].proxy)
                vfx::smokeSpheres(*proxy, r.seed, r.pose, age, frame.smoke);
            break;
        }
        case VfxRecord::Kind::Light: {
            // A flash: its light fades to nothing over its time; the record is gone at its end.
            if (r.effect >= frame.set->effects.size() || !frame.set->effects[r.effect].light) break;
            const vfx::FlashLight& l = *frame.set->effects[r.effect].light;
            if (age < 0.0f || age >= l.time) break;
            render::VfxFrameLight f;
            f.position = glm::vec3(r.pose[3]);
            f.color = l.color;
            f.intensity = l.intensity * (1.0f - age / l.time);
            f.range = l.range;
            f.id = r.seq;
            frame.lights.push_back(f);
            break;
        }
        case VfxRecord::Kind::Decal: {
            if (r.effect >= frame.set->decals.size() || age < 0.0f) break;
            // A decal with a life fades over its last tenth, and is gone at its end.
            float fade = 1.0f;
            if (r.life > 0.0f) {
                if (age >= r.life) break;
                fade = std::clamp((r.life - age) / (0.1f * r.life), 0.0f, 1.0f);
            }
            render::VfxFrameDecal d;
            d.decal = r.effect;
            d.fade = fade;
            d.pose = r.on != entt::null && reg.valid(r.on) ? drawnWorld(reg, r.on) * r.pose : r.pose;
            d.id = r.seq;
            frame.decals.push_back(d);
            break;
        }
        case VfxRecord::Kind::Tracer: {
            if (r.effect >= frame.set->tracers.size() || age < 0.0f) break;
            const vfx::TracerDesc& t = frame.set->tracers[r.effect];
            render::VfxFrameTracer f;
            f.tracer = r.effect;
            f.speed = t.speed;
            f.length = t.length;
            f.width = t.width;
            f.color = t.color * t.intensity;
            f.age = age;
            f.from = r.a;
            f.to = r.b;
            f.id = r.seq;
            frame.tracers.push_back(f);
            break;
        }
        default:
            break;
        }
    }
    // The set's proxy cap: the newest records' spheres stay, a cloud past it loses its oldest first.
    const std::size_t cap = std::min<std::size_t>(frame.set->caps.proxies, gpu::shared::kSmokeSpheresMax);
    if (frame.smoke.size() > cap) frame.smoke.erase(frame.smoke.begin(), frame.smoke.end() - static_cast<std::ptrdiff_t>(cap));
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
    std::vector<uint32_t> normals;

    void refresh(const VfxSystem& system) {
        if (set && version == system.setVersion()) return;
        version = system.setVersion();
        set = std::make_shared<const vfx::VfxSet>(system.set());
        textures.assign(set->textures.size(), gpu::shared::kVfxNoTexture);
        normals.assign(set->textures.size(), gpu::shared::kVfxNoTexture);
        if (!textureManager) return;
        for (std::size_t i = 0; i < set->textures.size(); ++i) {
            const auto path = assets.absolutePath(AssetRef{set->textures[i].file, {}});
            // Flipbook cells are sampled at mip 0 (a mip chain would bleed neighbouring cells).
            const BindlessTextureHandle h =
                path.empty() ? kInvalidBindlessTexture
                             : textureManager->loadTexture(path.string(), BindlessTextureType::Albedo, false);
            if (h.valid()) textures[i] = h.index;
            else OHAO_LOG(Warn, vfx, "vfx: texture '{}' not loaded: its emitters draw a soft dot", set->textures[i].file);
            // A decal's normal map is linear data: loaded as one, never as sRGB albedo.
            if (set->textures[i].normal.empty()) continue;
            const auto npath = assets.absolutePath(AssetRef{set->textures[i].normal, {}});
            const BindlessTextureHandle n =
                npath.empty() ? kInvalidBindlessTexture
                              : textureManager->loadTexture(npath.string(), BindlessTextureType::Normal, false);
            if (n.valid()) normals[i] = n.index;
            else OHAO_LOG(Warn, vfx, "vfx: normal map '{}' not loaded: its decals keep the surface's normal", set->textures[i].normal);
        }
    }

    void run(World& world) {
        // The test grenades in flight, between the last two steps (a runtime presentation: never saved).
        std::vector<render::RenderScene::PresentedSphere> grenades;
        if (const CombatSystem* combat = combatSystem(world))
            for (const CombatGrenade& g : combat->grenadesInFlight())
                grenades.push_back({g.prev + (g.pos - g.prev) * world.renderAlpha(), combat::kGrenadeRadius});
        scene.setPresentedSpheres(grenades);
        const VfxSystem* system = vfxSystem(world);
        if (!system) {
            if (!scene.vfx().effects.empty() || scene.vfx().set) scene.setVfx({});
            scene.setTransientLights({});
            return;
        }
        refresh(*system);
        scene.setVfx(vfxFrameOf(world, set, textures, normals));
        const std::vector<render::LightDesc> flashes = flashLights(scene.vfx());
        scene.setTransientLights(flashes);
    }
};

} // namespace

void installVfxDraw(World& world, render::RenderScene& scene, BindlessTextureManager* textures, AssetResolver& assets) {
    auto state = std::make_shared<VfxDraw>(VfxDraw{scene, textures, assets});
    world.addSync("vfx_draw", [state](World& w) { state->run(w); });
}

} // namespace ohao::ecs
