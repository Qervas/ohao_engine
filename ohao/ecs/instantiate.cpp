#include "ecs/instantiate.hpp"

#include "core/log.hpp"

#include "render/scene/model_upload.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace ohao::ecs {

namespace {

struct InstantiateInstalled {};

template <class... F>
struct Overloaded : F... { using F::operator()...; };
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

// Primitive parameters are padding-free PODs: their bytes are the cache key.
static_assert(sizeof(SpherePrim) == 12 && sizeof(CubePrim) == 4 && sizeof(PlatformPrim) == 12 &&
              sizeof(CylinderPrim) == 12 && sizeof(ConePrim) == 12 && sizeof(QuadPrim) == 72 &&
              sizeof(BoxPrim) == 36);

// One string per distinct MeshSource value: the alternative, then its bytes (or path).
std::string meshKey(const MeshSource& ms) {
    std::string key(1, static_cast<char>(ms.source.index()));
    std::visit(Overloaded{
                   [&](const ModelAsset& m) {
                       key += m.asset.path;
                       key.push_back('\0');
                       key += m.asset.guid;
                   },
                   [&](const auto& prim) { key.append(reinterpret_cast<const char*>(&prim), sizeof(prim)); },
               },
               ms.source);
    return key;
}

std::string debugName(const MeshSource& ms) {
    static constexpr const char* kNames[] = {"ModelAsset", "SpherePrim", "CubePrim", "PlatformPrim",
                                             "CylinderPrim", "ConePrim", "QuadPrim", "BoxPrim"};
    if (const auto* m = std::get_if<ModelAsset>(&ms.source)) return m->asset.path;
    return kNames[ms.source.index()];
}

std::shared_ptr<const Model> generate(const MeshSource& ms) {
    auto m = std::make_shared<Model>();
    std::visit(Overloaded{
                   [](const ModelAsset&) {},
                   [&](const SpherePrim& p) { PrimitiveMeshGenerator::generateSphere(*m, p.radius, p.sectors, p.stacks); },
                   [&](const CubePrim& p) { PrimitiveMeshGenerator::generateCube(*m, p.size); },
                   [&](const PlatformPrim& p) {
                       PrimitiveMeshGenerator::generatePlatform(*m, p.width, p.height, p.depth);
                   },
                   [&](const CylinderPrim& p) {
                       PrimitiveMeshGenerator::generateCylinder(*m, p.radius, p.height, p.segments);
                   },
                   [&](const ConePrim& p) { PrimitiveMeshGenerator::generateCone(*m, p.radius, p.height, p.segments); },
                   [&](const QuadPrim& p) { PrimitiveMeshGenerator::generateQuad(*m, p.a, p.b, p.c, p.d, p.normal, p.color); },
                   [&](const BoxPrim& p) { PrimitiveMeshGenerator::generateBox(*m, p.lo, p.hi, p.color); },
               },
               ms.source);
    return m;
}

render::MaterialDesc describe(const MaterialOverride& o) {
    render::MaterialDesc d;
    d.baseColor = o.baseColor;
    d.roughness = o.roughness;
    d.metallic = o.metallic;
    d.emissiveStrength = o.emissiveStrength;
    return d;
}

class RenderSceneBackend final : public InstantiateBackend {
public:
    RenderSceneBackend(render::RenderScene& scene, BindlessTextureManager* textures)
        : m_scene(scene), m_textures(textures) {}

    render::MeshHandle uploadMesh(const Model& model, std::string_view name) override {
        return render::uploadModelMesh(m_scene, model, name);
    }
    void releaseMesh(render::MeshHandle mesh) override { m_scene.releaseMesh(mesh); }
    std::vector<render::MaterialHandle> modelMaterials(const Model& model) override {
        std::vector<render::MaterialHandle> out;
        for (const render::MaterialDesc& d : render::modelMaterials(m_textures, model, nullptr))
            out.push_back(m_scene.registerMaterial(d));
        return out;
    }
    render::MaterialHandle material(const render::MaterialDesc& desc) override {
        return m_scene.registerMaterial(desc);
    }

private:
    render::RenderScene& m_scene;
    BindlessTextureManager* m_textures;
};

// State shared by the system and the registry signals (see render_extract.cpp).
class InstantiateState {
public:
    InstantiateState(World& world, std::shared_ptr<InstantiateBackend> backend, AssetResolver& assets)
        : m_world(world), m_reg(world.registry()), m_backend(std::move(backend)), m_assets(assets) {
        m_reg.on_construct<MeshSource>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_update<MeshSource>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_destroy<MeshSource>().connect<&InstantiateState::onSourceDestroy>(this);
        m_reg.on_construct<MaterialOverride>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_update<MaterialOverride>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_destroy<MaterialOverride>().connect<&InstantiateState::onOverrideDestroy>(this);
        // Installed into a world that already has sources (a host that starts its renderer
        // after loading, like `ohao run`): they are instantiated on the first run.
        for (const entt::entity e : m_reg.view<MeshSource>()) m_reg.emplace_or_replace<InstantiateDirty>(e);
    }

    ~InstantiateState() {
        m_reg.on_construct<MeshSource>().disconnect(this);
        m_reg.on_update<MeshSource>().disconnect(this);
        m_reg.on_destroy<MeshSource>().disconnect(this);
        m_reg.on_construct<MaterialOverride>().disconnect(this);
        m_reg.on_update<MaterialOverride>().disconnect(this);
        m_reg.on_destroy<MaterialOverride>().disconnect(this);
        // The World is going away: release every mesh it still holds.
        for (auto& [key, entry] : m_meshes) m_backend->releaseMesh(entry.mesh);
    }

    void run() {
        // 1. MeshSource removed (or the entity destroyed): drop the user, and take away
        // what instantiate made from an entity that lives on.
        for (const entt::entity e : m_sourceGone) {
            dropUser(e);
            if (m_reg.valid(e) && !m_reg.all_of<MeshSource>(e)) clearDerived(e);
        }
        m_sourceGone.clear();
        // 2. MaterialOverride removed from an entity that keeps its mesh: back to default.
        for (const entt::entity e : m_overrideGone)
            if (m_reg.valid(e) && m_reg.all_of<MeshSource>(e)) m_reg.emplace_or_replace<InstantiateDirty>(e);
        m_overrideGone.clear();

        // 3. Dirty entities, in creation order.
        m_dirty.clear();
        for (const entt::entity e : m_reg.view<InstantiateDirty>()) {
            const auto* co = m_reg.try_get<CreationOrder>(e);
            m_dirty.emplace_back(co ? co->seq : UINT64_MAX, e);
        }
        std::sort(m_dirty.begin(), m_dirty.end());
        for (const auto& [seq, e] : m_dirty)
            if (const auto* ms = m_reg.try_get<MeshSource>(e)) instantiate(e, *ms);
        m_reg.clear<InstantiateDirty>();
    }

private:
    struct MeshEntry {
        render::MeshHandle mesh;
        std::vector<render::MaterialHandle> modelMaterials;   // non-empty: the model has materials
        render::GiModelInputs gi;
        std::optional<EmissiveProxyLight> emissive;
        uint32_t users{0};
    };

    void instantiate(entt::entity e, const MeshSource& ms) {
        std::string key = meshKey(ms);
        auto it = m_meshes.find(key);
        if (it == m_meshes.end()) {
            std::shared_ptr<const Model> model;
            if (const auto* asset = std::get_if<ModelAsset>(&ms.source)) {
                auto loaded = m_assets.loadModel(asset->asset);
                if (!loaded) return fail(e, loaded.error().code, loaded.error().message);
                model = std::move(loaded.value());
            } else {
                model = generate(ms);
            }
            if (model->vertices.empty() || model->indices.size() < 3)
                return fail(e, "E_MESH_INVALID", "'" + debugName(ms) + "' has no triangles");
            MeshEntry entry;
            entry.mesh = m_backend->uploadMesh(*model, debugName(ms));
            if (!entry.mesh.valid())
                return fail(e, "E_MESH_INVALID", "'" + debugName(ms) + "' could not be uploaded to the render scene");
            if (!model->materialColors.empty()) entry.modelMaterials = m_backend->modelMaterials(*model);
            entry.gi = render::giModelInputs(*model);
            if (const auto proxy = render::emissiveProxy(*model))
                entry.emissive = EmissiveProxyLight{proxy->localCenter, proxy->radius, proxy->color, proxy->intensity};
            it = m_meshes.emplace(std::move(key), std::move(entry)).first;
        }
        MeshEntry& entry = it->second;

        // Switch the entity to this mesh (the old one may lose its last user).
        if (const auto user = m_userOf.find(e); user == m_userOf.end() || user->second != it->first) {
            ++entry.users;
            dropUser(e);
            m_userOf[e] = it->first;
        }

        const auto* overrideMat = m_reg.try_get<MaterialOverride>(e);
        const render::MaterialDesc desc = overrideMat ? describe(*overrideMat) : render::MaterialDesc{};
        std::vector<render::MaterialHandle> materials =
            entry.modelMaterials.empty() ? std::vector<render::MaterialHandle>{material(desc)} : entry.modelMaterials;
        const auto* previous = m_reg.try_get<MeshRenderer>(e);
        const bool visible = previous ? previous->visible : true;
        m_reg.emplace_or_replace<GiMaterialLegacy>(
            e, GiMaterialLegacy{render::giMaterialLegacyPacked(entry.gi, overrideMat ? &desc : nullptr)});
        m_reg.emplace_or_replace<MeshRenderer>(e, MeshRenderer{entry.mesh, std::move(materials), visible});
        if (entry.emissive) m_reg.emplace_or_replace<EmissiveProxyLight>(e, *entry.emissive);
        else m_reg.remove<EmissiveProxyLight>(e);
        m_reg.remove<InstantiateError>(e);
    }

    render::MaterialHandle material(const render::MaterialDesc& d) {
        const std::array<uint32_t, 6> key{std::bit_cast<uint32_t>(d.baseColor.r), std::bit_cast<uint32_t>(d.baseColor.g),
                                          std::bit_cast<uint32_t>(d.baseColor.b), std::bit_cast<uint32_t>(d.roughness),
                                          std::bit_cast<uint32_t>(d.metallic), std::bit_cast<uint32_t>(d.emissiveStrength)};
        if (const auto it = m_materials.find(key); it != m_materials.end()) return it->second;
        const render::MaterialHandle h = m_backend->material(d);
        m_materials.emplace(key, h);
        return h;
    }

    void fail(entt::entity e, const std::string& code, const std::string& message) {
        dropUser(e);
        clearDerived(e);
        m_reg.emplace_or_replace<InstantiateError>(e, InstantiateError{code, message});
        const uint64_t id = m_world.idOf(e);
        if (m_logged.insert(std::to_string(id) + code + message).second)
            OHAO_LOG(Error, ecs, "instantiate: entity {:#x}: {}: {}", id, code, message);
    }

    // What instantiate put on the entity.
    void clearDerived(entt::entity e) {
        m_reg.remove<MeshRenderer>(e);
        m_reg.remove<GiMaterialLegacy>(e);
        m_reg.remove<EmissiveProxyLight>(e);
        m_reg.remove<InstantiateError>(e);
    }

    void dropUser(entt::entity e) {
        const auto user = m_userOf.find(e);
        if (user == m_userOf.end()) return;
        const auto it = m_meshes.find(user->second);
        m_userOf.erase(user);
        if (it == m_meshes.end() || --it->second.users > 0) return;
        m_backend->releaseMesh(it->second.mesh);
        m_meshes.erase(it);
    }

    void markDirty(entt::registry& reg, entt::entity e) { reg.emplace_or_replace<InstantiateDirty>(e); }
    // Only queued: the entity may be in the middle of being destroyed.
    void onSourceDestroy(entt::registry&, entt::entity e) { m_sourceGone.push_back(e); }
    void onOverrideDestroy(entt::registry&, entt::entity e) { m_overrideGone.push_back(e); }

    World& m_world;
    entt::registry& m_reg;
    std::shared_ptr<InstantiateBackend> m_backend;
    AssetResolver& m_assets;
    std::map<std::string, MeshEntry> m_meshes;   // by meshKey
    ankerl::unordered_dense::map<entt::entity, std::string> m_userOf;
    std::map<std::array<uint32_t, 6>, render::MaterialHandle> m_materials;
    std::vector<entt::entity> m_sourceGone, m_overrideGone;
    std::vector<std::pair<uint64_t, entt::entity>> m_dirty;
    std::unordered_set<std::string> m_logged;
};

} // namespace

void installInstantiate(World& world, std::shared_ptr<InstantiateBackend> backend, AssetResolver& assets) {
    auto& reg = world.registry();
    if (reg.ctx().contains<InstantiateInstalled>()) {
        OHAO_LOG(Warn, ecs, "installInstantiate called twice on one World; ignored");
        return;
    }
    reg.ctx().emplace<InstantiateInstalled>();
    auto state = std::make_shared<InstantiateState>(world, std::move(backend), assets);
    world.addSyncBefore("transform", "instantiate", [state](World&) { state->run(); });
}

void installInstantiate(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        AssetResolver& assets) {
    installInstantiate(world, std::make_shared<RenderSceneBackend>(scene, textures), assets);
}

std::vector<entt::entity> instantiateErrors(const World& world) {
    const auto& reg = world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> found;
    for (auto [e, err] : reg.view<const InstantiateError>().each()) {
        const auto* co = reg.try_get<CreationOrder>(e);
        found.emplace_back(co ? co->seq : UINT64_MAX, e);
    }
    std::sort(found.begin(), found.end());
    std::vector<entt::entity> out;
    for (const auto& [seq, e] : found) out.push_back(e);
    return out;
}

} // namespace ohao::ecs
