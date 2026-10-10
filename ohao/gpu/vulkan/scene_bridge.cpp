// TEMPORARY (sub-project 2a, Tasks 7-9): feeds the old Scene into RenderScene so the deferred and RT passes can move first. Deleted in Task 9.
#include "renderer_impl.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/model_upload.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/asset/model.hpp"
#include "scene/component/light_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/transform_component.hpp"

#include <algorithm>
#include <cstring>

namespace ohao {

namespace {

// The actor's MaterialComponent as a MaterialDesc, exactly as the old
// GBufferPass::execute read it (the branch for draws without a model material).
std::optional<render::MaterialDesc> actorMaterial(const Actor& actor, BindlessTextureManager* tm) {
    auto materialComp = actor.getComponent<MaterialComponent>();
    if (!materialComp) return std::nullopt;
    const Material& mat = materialComp->getMaterial();
    render::MaterialDesc d;
    d.baseColor = mat.baseColor;
    d.metallic = mat.metallic;
    d.roughness = mat.roughness;
    if (tm) {
        if (mat.useAlbedoTexture && !mat.albedoTexture.empty()) {
            auto handle = tm->findTexture(mat.albedoTexture);
            if (handle.valid()) d.albedoTex = handle.index;
        }
        if (mat.useNormalTexture && !mat.normalTexture.empty()) {
            auto handle = tm->findTexture(mat.normalTexture);
            if (handle.valid()) d.normalTex = handle.index;
        }
        if (mat.useRoughnessTexture && !mat.roughnessTexture.empty()) {
            auto handle = tm->findTexture(mat.roughnessTexture);
            if (handle.valid()) d.roughMetalTex = handle.index;
        }
        if (mat.useEmissiveTexture && !mat.emissiveTexture.empty()) {
            auto handle = tm->findTexture(mat.emissiveTexture);
            if (handle.valid()) d.emissiveTex = handle.index;
            d.emissiveStrength = glm::length(mat.emissive) > 0.01f ? 1.0f : 0.0f;
        }
    }
    return d;
}

// The path tracer's legacy tint for a material without an albedo texture (what
// uploadModel adds to model materials), so bridge-registered materials match.
render::MaterialDesc withLegacyRtTint(render::MaterialDesc d, BindlessTextureManager* tm) {
    if (d.albedoTex == render::kNoTexture && d.legacyRtTintTex == render::kNoTexture)
        d.legacyRtTintTex = render::legacyRtTintTexture(tm, d.baseColor);
    return d;
}

bool sameMaterial(const render::MaterialDesc& a, const render::MaterialDesc& b) {
    return a.baseColor == b.baseColor && a.roughness == b.roughness && a.metallic == b.metallic &&
           a.albedoTex == b.albedoTex && a.normalTex == b.normalTex && a.roughMetalTex == b.roughMetalTex &&
           a.emissiveTex == b.emissiveTex && a.emissiveStrength == b.emissiveStrength &&
           a.legacyRtTintTex == b.legacyRtTintTex;
}

render::LightKind lightKind(LightType t) {
    switch (t) {
    case LightType::Sphere: return render::LightKind::Point;
    case LightType::Directional: return render::LightKind::Directional;
    case LightType::Spot: return render::LightKind::Spot;
    case LightType::AreaRect: return render::LightKind::AreaRect;
    }
    return render::LightKind::Point;
}

} // namespace

void VulkanRenderer::bridgeSceneToRenderScene() {
    if (!m_renderScene) return;

    // Clear the instances; meshes and materials stay cached (no re-upload on re-calls).
    // Released slots are reused lowest-first, so the slot (= draw) order is again the
    // actor iteration order.
    for (const BridgedInstance& b : m_bridgeInstances) m_renderScene->destroyInstance(b.slot);
    m_bridgeInstances.clear();

    // Registers a material once per distinct description.
    const auto materialFor = [&](const render::MaterialDesc& d) {
        for (const auto& [desc, h] : m_bridgeMaterials)
            if (sameMaterial(desc, d)) return h;
        const render::MaterialHandle h = m_renderScene->registerMaterial(d);
        m_bridgeMaterials.emplace_back(d, h);
        return h;
    };

    BindlessTextureManager* tm = m_textureManager.get();
    if (m_scene) {
        // Same actor filter and order as the old updateSceneBuffers / GBufferPass.
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            if (!actor) continue;
            auto meshComp = actor->getComponent<MeshComponent>();
            if (!meshComp || !meshComp->getModel() || !meshComp->isVisible()) continue;
            const std::shared_ptr<Model> model = meshComp->getModel();

            std::optional<render::MaterialDesc> actorMat = actorMaterial(*actor, tm);
            if (actorMat) actorMat = withLegacyRtTint(*actorMat, tm);
            const render::MaterialDesc* overrideMat = actorMat ? &*actorMat : nullptr;
            const render::MaterialDesc defaultMat = withLegacyRtTint(render::MaterialDesc{}, tm);

            BridgedModel& cached = m_bridgeModels[model.get()];
            cached.model = model;
            if (!cached.uploaded) {
                cached.uploaded = render::uploadModel(*m_renderScene, tm, *model, actor->getName(), overrideMat);
                cached.emissive = cached.uploaded->emissive;
                cached.emissiveKnown = true;
                if (model->materialColors.empty() && !cached.uploaded->materials.empty())
                    m_bridgeMaterials.emplace_back(overrideMat ? *overrideMat : defaultMat,
                                                   cached.uploaded->materials[0]);
            }
            const render::UploadedModel& up = *cached.uploaded;
            if (!m_renderScene->meshInfo(up.mesh)) continue;   // mesh registration failed

            // The old draw-range rule: a range with a model material used it; any other
            // range (no materialColors, or an index past them) used the actor's
            // MaterialComponent, or the 0.8/0.5/0 defaults without one.
            const render::MaterialHandle fallback = materialFor(overrideMat ? *overrideMat : defaultMat);
            BridgedInstance b{};
            b.actorId = actorId;
            if (model->materialColors.empty()) {
                b.materials = {fallback};
            } else {
                b.materials = up.materials;
                uint32_t maxSlot = 0;
                for (const auto& sub : m_renderScene->meshInfo(up.mesh)->submeshes)
                    maxSlot = std::max(maxSlot, sub.materialSlot);
                if (b.materials.size() <= maxSlot) b.materials.resize(maxSlot + 1, fallback);
            }

            const glm::mat4 world = actor->getTransform()->getWorldMatrix();
            b.desc.mesh = up.mesh;
            b.desc.world = world;
            b.desc.prevWorld = world;   // the old G-buffer used the current model matrix for motion
            b.desc.visible = true;
            b.desc.giAlbedoPacked = render::giMaterialLegacyPacked(*model, overrideMat);
            b.desc.materials = b.materials;
            b.slot = m_renderScene->createInstance(b.desc);
            if (b.slot == render::kInvalidIndex) continue;
            m_bridgeInstances.push_back(std::move(b));
        }
    }

    syncBridgedScene();
}

void VulkanRenderer::syncBridgedScene() {
    if (!m_renderScene) return;

    // Transforms and visibility, read live every frame as the old passes did.
    std::unordered_map<uint64_t, Actor*> actorsById;
    if (m_scene)
        for (const auto& [id, actor] : m_scene->getAllActors())
            if (actor) actorsById.emplace(id, actor.get());
    for (BridgedInstance& b : m_bridgeInstances) {
        auto it = actorsById.find(b.actorId);
        Actor* actor = it != actorsById.end() ? it->second : nullptr;
        auto meshComp = actor ? actor->getComponent<MeshComponent>() : nullptr;
        const bool visible = meshComp && meshComp->isVisible();
        const glm::mat4 world = visible ? actor->getTransform()->getWorldMatrix() : b.desc.world;
        if (visible == b.desc.visible && std::memcmp(&world, &b.desc.world, sizeof(world)) == 0) continue;
        b.desc.world = world;
        b.desc.prevWorld = world;
        b.desc.visible = visible;
        b.desc.materials = b.materials;
        m_renderScene->updateInstance(b.slot, b.desc);
    }

    // Lights only while the bridge owns a scene: with none it must not stomp lights
    // another producer (the ECS extract) set. Its own lights are cleared once.
    if (!m_scene) {
        if (m_bridgeOwnsLights) {
            m_renderScene->setLights({});
            m_bridgeOwnsLights = false;
        }
        return;
    }

    // Lights, in actor iteration order: LightComponents, and the emissive proxy of every
    // mesh actor (the light table places proxies after the regular lights, as before).
    std::vector<render::LightDesc> lights;
    {
        uint64_t order = 0;
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            if (!actor) { ++order; continue; }
            if (auto lc = actor->getComponent<LightComponent>()) {
                render::LightDesc d;
                d.kind = lightKind(lc->getLightType());
                d.position = actor->getTransform()->getPosition();   // the old rule: local position
                d.direction = lc->getDirection();
                d.color = lc->getColor();
                d.intensity = lc->getIntensity();
                d.range = lc->getRange();
                d.radius = lc->getRadius();
                d.innerConeDeg = lc->getInnerConeAngle();
                d.outerConeDeg = lc->getOuterConeAngle();
                d.areaEdge1 = lc->getEdge1();
                d.areaEdge2 = lc->getEdge2();
                d.order = order;
                lights.push_back(d);
            }
            auto meshComp = actor->getComponent<MeshComponent>();
            if (meshComp && meshComp->getModel()) {
                const std::shared_ptr<Model> model = meshComp->getModel();
                BridgedModel& cached = m_bridgeModels[model.get()];
                cached.model = model;
                if (!cached.emissiveKnown) {
                    cached.emissive = render::emissiveProxy(*model);
                    cached.emissiveKnown = true;
                }
                if (cached.emissive) {
                    const glm::mat4 world = actor->getTransform()->getWorldMatrix();
                    render::LightDesc d;
                    d.kind = render::LightKind::EmissiveProxy;
                    d.position = glm::vec3(world * glm::vec4(cached.emissive->localCenter, 1.0f));
                    d.radius = cached.emissive->radius;
                    d.color = cached.emissive->color;
                    d.intensity = cached.emissive->intensity;
                    d.order = order;
                    lights.push_back(d);
                }
            }
            ++order;
        }
    }
    m_renderScene->setLights(lights);
    m_bridgeOwnsLights = true;
}

} // namespace ohao
