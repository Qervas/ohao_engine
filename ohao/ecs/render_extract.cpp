#include "ecs/render_extract.hpp"
#include "ecs/components.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ohao::ecs {

namespace {

struct RenderExtractInstalled {};

render::LightKind toKind(LightType t) {
    switch (t) {
        case LightType::Point: return render::LightKind::Point;
        case LightType::Directional: return render::LightKind::Directional;
        case LightType::Spot: return render::LightKind::Spot;
        case LightType::AreaRect: return render::LightKind::AreaRect;
    }
    return render::LightKind::Point;
}

uint64_t creationSeq(const entt::registry& reg, entt::entity e) {
    const auto* c = reg.try_get<CreationOrder>(e);
    return c ? c->seq : std::numeric_limits<uint64_t>::max();
}

// State shared by the system and the registry signals. Lives as long as the system
// (World::m_systems is destroyed before World::m_registry, so the destructor can
// still disconnect and release).
class ExtractState {
public:
    ExtractState(entt::registry& reg, render::RenderScene& rs) : m_reg(reg), m_rs(rs) {
        reg.on_construct<MeshRenderer>().connect<&ExtractState::markRenderDirty>(this);
        reg.on_update<MeshRenderer>().connect<&ExtractState::markRenderDirty>(this);
        reg.on_destroy<MeshRenderer>().connect<&ExtractState::onMeshRendererDestroy>(this);
        reg.on_destroy<RenderSlot>().connect<&ExtractState::onRenderSlotDestroy>(this);
        reg.on_construct<GiMaterialLegacy>().connect<&ExtractState::markRenderDirty>(this);
        reg.on_update<GiMaterialLegacy>().connect<&ExtractState::markRenderDirty>(this);
        reg.on_construct<Light>().connect<&ExtractState::markLightsDirty>(this);
        reg.on_update<Light>().connect<&ExtractState::markLightsDirty>(this);
        reg.on_destroy<Light>().connect<&ExtractState::markLightsDirty>(this);
        reg.on_construct<EmissiveProxyLight>().connect<&ExtractState::markLightsDirty>(this);
        reg.on_update<EmissiveProxyLight>().connect<&ExtractState::markLightsDirty>(this);
        reg.on_destroy<EmissiveProxyLight>().connect<&ExtractState::markLightsDirty>(this);
        // Installed into a world that already has renderables or lights (a host that
        // starts its renderer after loading): they are picked up on the first run.
        for (const entt::entity e : reg.view<MeshRenderer>()) reg.emplace_or_replace<RenderDirty>(e);
        m_lightsDirty = !reg.view<Light>().empty() || !reg.view<EmissiveProxyLight>().empty();
    }

    ~ExtractState() {
        m_reg.on_construct<MeshRenderer>().disconnect(this);
        m_reg.on_update<MeshRenderer>().disconnect(this);
        m_reg.on_destroy<MeshRenderer>().disconnect(this);
        m_reg.on_destroy<RenderSlot>().disconnect(this);
        m_reg.on_construct<GiMaterialLegacy>().disconnect(this);
        m_reg.on_update<GiMaterialLegacy>().disconnect(this);
        m_reg.on_construct<Light>().disconnect(this);
        m_reg.on_update<Light>().disconnect(this);
        m_reg.on_destroy<Light>().disconnect(this);
        m_reg.on_construct<EmissiveProxyLight>().disconnect(this);
        m_reg.on_update<EmissiveProxyLight>().disconnect(this);
        m_reg.on_destroy<EmissiveProxyLight>().disconnect(this);
        // The World is going away: take its instances and lights out of the scene.
        for (uint32_t slot = 0; slot < m_owner.size(); ++slot)
            if (m_owner[slot] != entt::null) m_rs.destroyInstance(slot);
        for (const auto& [e, slot] : m_releaseQueue) m_rs.destroyInstance(slot);
        if (m_lightsSet) m_rs.setLights({});
    }

    void run() {
        // 1. Releases queued by the destroy signals.
        for (const auto& [e, slot] : m_releaseQueue) {
            m_rs.destroyInstance(slot);
            if (!m_reg.valid(e)) continue;
            // MeshRenderer was removed (entity alive): drop the stale RenderSlot. Its
            // destroy signal finds the slot already unowned and does nothing.
            if (const auto* rs = m_reg.try_get<RenderSlot>(e); rs && rs->slot == slot)
                m_reg.remove<RenderSlot>(e);
            // RenderSlot was removed by hand while MeshRenderer stays: recreate.
            if (m_reg.all_of<MeshRenderer>(e)) m_reg.emplace_or_replace<RenderDirty>(e);
        }
        m_releaseQueue.clear();

        // 2./3. Every new or changed renderable carries RenderDirty: TransformSystem
        // tags first-time and moved entities, the MeshRenderer signals tag the rest.
        m_pending.clear();
        bool lightsTouched = false;
        for (const entt::entity e : m_reg.view<RenderDirty>()) {
            if (!lightsTouched && m_reg.any_of<Light, EmissiveProxyLight>(e)) lightsTouched = true;
            const auto* mr = m_reg.try_get<MeshRenderer>(e);
            const auto* wt = m_reg.try_get<WorldTransform>(e);
            if (!mr || !wt) continue;
            if (!meshUsable(mr->mesh)) continue;   // stale/unknown: skipped, logged once
            if (const auto* rs = m_reg.try_get<RenderSlot>(e)) {
                // Only touch slots this extract handed out to this entity.
                if (rs->slot < m_owner.size() && m_owner[rs->slot] == e)
                    m_rs.updateInstance(rs->slot, describe(e, *mr, *wt));
            } else {
                m_pending.emplace_back(creationSeq(m_reg, e), e);
            }
        }
        // Slots in creation order, independent of pool iteration order.
        std::sort(m_pending.begin(), m_pending.end());
        for (const auto& [seq, e] : m_pending) {
            const auto& mr = m_reg.get<MeshRenderer>(e);
            const uint32_t slot = m_rs.createInstance(describe(e, mr, m_reg.get<WorldTransform>(e)));
            if (slot == render::kInvalidIndex) {   // e.g. the scene was shut down
                warnOnce(mr.mesh, "createInstance failed");
                continue;
            }
            if (slot >= m_owner.size()) m_owner.resize(std::size_t{slot} + 1, entt::null);
            m_owner[slot] = e;
            m_reg.emplace<RenderSlot>(e, RenderSlot{slot});
        }

        // 4.
        m_reg.clear<RenderDirty>();

        // 5. Lights.
        if (lightsTouched) m_lightsDirty = true;
        if (m_lightsDirty) {
            rebuildLights();
            m_lightsDirty = false;
        }
    }

private:
    render::InstanceDesc describe(entt::entity e, const MeshRenderer& mr, const WorldTransform& wt) const {
        render::InstanceDesc d{};
        d.mesh = mr.mesh;
        d.materials = mr.materials;
        // Drawn between two simulation steps (render interpolation) or at the step's pose.
        if (const auto* rp = m_reg.try_get<RenderPose>(e)) {
            d.world = rp->world;
            d.prevWorld = rp->prevWorld;
        } else {
            d.world = wt.world;
            d.prevWorld = wt.prevWorld;
        }
        d.visible = mr.visible;
        d.viewModel = inViewModel(e);
        if (const auto* gi = m_reg.try_get<GiMaterialLegacy>(e)) d.giAlbedoPacked = gi->packed;
        return d;
    }

    // A first-person gun's part: the entity or an ancestor has a ViewModel.
    [[nodiscard]] bool inViewModel(entt::entity e) const {
        for (int depth = 0; e != entt::null && m_reg.valid(e) && depth < 64; ++depth) {
            if (m_reg.all_of<ViewModel>(e)) return true;
            const auto* p = m_reg.try_get<Parent>(e);
            e = p ? p->entity : entt::null;
        }
        return false;
    }

    void rebuildLights() {
        m_lights.clear();
        for (auto [e, l, wtStep] : m_reg.view<const Light, const WorldTransform>().each()) {
            // Where the frame draws it: between two steps, or with a first-person gun (a muzzle
            // light below a ViewModel follows the drawn gun), else the step's pose.
            const auto* rp = m_reg.try_get<RenderPose>(e);
            const WorldTransform wt = rp ? WorldTransform{rp->world, rp->prevWorld} : wtStep;
            render::LightDesc d{};
            d.kind = toKind(l.type);
            const glm::mat3 basis(wt.world);
            d.position = glm::vec3(wt.world[3]);
            d.direction = glm::normalize(basis * glm::vec3(0.0f, 0.0f, -1.0f));
            d.color = l.color;
            d.intensity = l.intensity;
            d.range = l.range;
            d.radius = l.radius;
            // A drawn pose that scales (a gun's view-space remap) scales the emitter mesh with it.
            if (rp) {
                const float stepVolume = std::abs(glm::determinant(glm::mat3(wtStep.world)));
                const float drawnVolume = std::abs(glm::determinant(basis));
                if (stepVolume > 0.0f && drawnVolume != stepVolume) d.radius *= std::cbrt(drawnVolume / stepVolume);
            }
            d.innerConeDeg = l.innerConeDeg;
            d.outerConeDeg = l.outerConeDeg;
            d.areaEdge1 = basis * l.areaEdge1;
            d.areaEdge2 = basis * l.areaEdge2;
            d.castShadows = l.castShadows;
            if (wt.prevWorld != wt.world) d.lastFrameMotion = wt.prevWorld * glm::inverse(wt.world);
            d.order = creationSeq(m_reg, e);
            m_lights.push_back(d);
        }
        for (auto [e, p, wt] : m_reg.view<const EmissiveProxyLight, const WorldTransform>().each()) {
            render::LightDesc d{};
            d.kind = render::LightKind::EmissiveProxy;
            d.position = glm::vec3(wt.world * glm::vec4(p.localCenter, 1.0f));
            d.color = p.color;
            d.intensity = p.intensity;
            d.radius = p.radius;
            d.order = creationSeq(m_reg, e);
            m_lights.push_back(d);
        }
        m_rs.setLights(m_lights);
        m_lightsSet = true;
    }

    // A mesh handle the scene does not know (never registered, rejected at
    // registration, or default MeshRenderer{}). The entity is skipped -- no slot --
    // and each distinct handle is reported once.
    bool meshUsable(render::MeshHandle h) {
        if (m_rs.meshInfo(h)) return true;
        warnOnce(h, "unknown or stale mesh handle");
        return false;
    }
    void warnOnce(render::MeshHandle h, const char* what) {
        const uint64_t key = (uint64_t{h.index} << 32) | h.generation;
        if (!m_warned.insert(key).second) return;
        OHAO_LOG(Warn, ecs,
                 "render_extract: {} (mesh index {}, generation {}); entities using it are not rendered",
                 what, h.index, h.generation);
    }

    void markRenderDirty(entt::registry& reg, entt::entity e) { reg.emplace_or_replace<RenderDirty>(e); }
    void markLightsDirty(entt::registry&, entt::entity) { m_lightsDirty = true; }

    void queueRelease(entt::entity e, uint32_t slot) {
        if (slot >= m_owner.size() || m_owner[slot] != e) return;   // not ours, or already queued
        m_owner[slot] = entt::null;
        m_releaseQueue.emplace_back(e, slot);
    }
    void onMeshRendererDestroy(entt::registry& reg, entt::entity e) {
        if (const auto* rs = reg.try_get<RenderSlot>(e)) queueRelease(e, rs->slot);
    }
    void onRenderSlotDestroy(entt::registry& reg, entt::entity e) {
        queueRelease(e, reg.get<RenderSlot>(e).slot);
    }

    entt::registry& m_reg;
    render::RenderScene& m_rs;
    std::vector<entt::entity> m_owner;                              // slot -> entity
    std::vector<std::pair<entt::entity, uint32_t>> m_releaseQueue;
    std::vector<std::pair<uint64_t, entt::entity>> m_pending;
    std::vector<render::LightDesc> m_lights;
    bool m_lightsDirty{false};
    bool m_lightsSet{false};
    std::unordered_set<uint64_t> m_warned;   // mesh handles already reported
};

} // namespace

void installRenderExtract(World& world, render::RenderScene& scene) {
    auto& reg = world.registry();
    if (reg.ctx().contains<RenderExtractInstalled>()) {
        OHAO_LOG(Warn, ecs, "installRenderExtract called twice on one World; ignored");
        return;
    }
    reg.ctx().emplace<RenderExtractInstalled>();
    auto state = std::make_shared<ExtractState>(reg, scene);
    // After the transform system, and after render interpolation when physics installed it first.
    const auto names = world.syncNames();
    // After the first-person gun's drawn poses too (view_model, after interpolate).
    const auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    const char* anchor = has("view_model") ? "view_model" : has("interpolate") ? "interpolate" : "transform";
    world.addSyncAfter(anchor, "render_extract", [state](World&) { state->run(); });
}

} // namespace ohao::ecs
