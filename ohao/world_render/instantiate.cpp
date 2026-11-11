#include "world_render/instantiate.hpp"

#include "ecs/primitive_mesh.hpp"

#include "core/log.hpp"

#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/model_upload.hpp"
#include "render/scene/render_scene.hpp"

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
                       key.push_back('\n');   // one GPU mesh per (file, mesh): every user of a glTF mesh shares it
                       key += m.mesh;
                   },
                   [&](const auto& prim) { key.append(reinterpret_cast<const char*>(&prim), sizeof(prim)); },
               },
               ms.source);
    return key;
}

std::string debugName(const MeshSource& ms) {
    static constexpr const char* kNames[] = {"ModelAsset", "SpherePrim", "CubePrim", "PlatformPrim",
                                             "CylinderPrim", "ConePrim", "QuadPrim", "BoxPrim"};
    if (const auto* m = std::get_if<ModelAsset>(&ms.source)) return m->mesh.empty() ? m->asset.path : m->asset.path + ":" + m->mesh;
    return kNames[ms.source.index()];
}

render::MaterialDesc describe(const Material& o) {
    render::MaterialDesc d;
    d.baseColor = o.baseColor;
    d.baseWeight = o.baseWeight;
    d.metallic = o.baseMetalness;
    d.diffuseRoughness = o.baseDiffuseRoughness;
    d.specularWeight = o.specularWeight;
    d.specularColor = o.specularColor;
    d.roughness = o.specularRoughness;
    d.specularIor = o.specularIor;
    d.emissiveStrength = o.emissionLuminance;
    d.emissionColor = o.emissionColor;
    d.subsurface = o.subsurfaceWeight;
    d.alphaMode = static_cast<render::AlphaMode>(o.alphaMode);
    d.opacity = o.geometryOpacity;
    d.specularAnisotropy = o.specularRoughnessAnisotropy;
    d.coatWeight = o.coatWeight;
    d.coatColor = o.coatColor;
    d.coatRoughness = o.coatRoughness;
    d.coatAnisotropy = o.coatRoughnessAnisotropy;
    d.coatIor = o.coatIor;
    d.coatDarkening = o.coatDarkening;
    d.fuzzWeight = o.fuzzWeight;
    d.fuzzColor = o.fuzzColor;
    d.fuzzRoughness = o.fuzzRoughness;
    d.tangentRotation = o.geometryTangentRotation;
    d.transmissionWeight = o.transmissionWeight;
    d.transmissionColor = o.transmissionColor;
    d.transmissionDepth = o.transmissionDepth;
    d.dispersionScale = o.transmissionDispersionScale;
    d.dispersionAbbe = o.transmissionDispersionAbbeNumber;
    d.thinWalled = o.geometryThinWalled;
    d.thinFilmWeight = o.thinFilmWeight;
    d.thinFilmThickness = o.thinFilmThickness;
    d.thinFilmIor = o.thinFilmIor;
    d.subsurfaceColor = o.subsurfaceColor;
    d.subsurfaceRadius = o.subsurfaceRadius;
    d.subsurfaceRadiusScale = o.subsurfaceRadiusScale;
    d.subsurfaceAnisotropy = o.subsurfaceScatterAnisotropy;
    return d;
}

// State shared by the system and the registry signals (see render_extract.cpp).
class InstantiateState {
public:
    InstantiateState(World& world, render::RenderScene& scene, BindlessTextureManager* textures, AssetResolver& assets)
        : m_world(world), m_reg(world.registry()), m_scene(scene), m_textures(textures), m_assets(assets) {
        m_reg.on_construct<MeshSource>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_update<MeshSource>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_destroy<MeshSource>().connect<&InstantiateState::onSourceDestroy>(this);
        m_reg.on_construct<Material>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_update<Material>().connect<&InstantiateState::markDirty>(this);
        m_reg.on_destroy<Material>().connect<&InstantiateState::onOverrideDestroy>(this);
        // Installed into a world that already has sources (a host that starts its renderer
        // after loading, like `ohao run`): they are instantiated on the first run.
        for (const entt::entity e : m_reg.view<MeshSource>()) m_reg.emplace_or_replace<InstantiateDirty>(e);
    }

    ~InstantiateState() {
        m_reg.on_construct<MeshSource>().disconnect(this);
        m_reg.on_update<MeshSource>().disconnect(this);
        m_reg.on_destroy<MeshSource>().disconnect(this);
        m_reg.on_construct<Material>().disconnect(this);
        m_reg.on_update<Material>().disconnect(this);
        m_reg.on_destroy<Material>().disconnect(this);
        // The World is going away: release every mesh it still holds.
        for (auto& [key, entry] : m_meshes) m_scene.releaseMesh(entry.mesh);
    }

    void run() {
        // 1. MeshSource removed (or the entity destroyed): drop the user, and take away
        // what instantiate made from an entity that lives on.
        for (const entt::entity e : m_sourceGone) {
            dropUser(e);
            if (m_reg.valid(e) && !m_reg.all_of<MeshSource>(e)) clearDerived(e);
        }
        m_sourceGone.clear();
        // 2. Material removed from an entity that keeps its mesh: back to default.
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
        // The textures the new materials name, in one batch.
        if (m_textures) m_textures->flushUploads();
    }

private:
    struct MeshEntry {
        render::MeshHandle mesh;
        std::vector<render::MaterialHandle> modelMaterials;   // non-empty: the model has materials
        uint32_t users{0};
    };

    void instantiate(entt::entity e, const MeshSource& ms) {
        std::string key = meshKey(ms);
        auto it = m_meshes.find(key);
        if (it == m_meshes.end()) {
            std::shared_ptr<const cooked::Mesh> mesh;
            if (const auto* asset = std::get_if<ModelAsset>(&ms.source)) {
                auto loaded = m_assets.loadMesh(asset->asset, asset->mesh);
                if (!loaded) return fail(e, loaded.error().code, loaded.error().message);
                mesh = std::move(loaded.value());
            } else {
                mesh = primitiveMesh(ms);
            }
            if (mesh->vertices.empty() || mesh->indices.size() < 3)
                return fail(e, "E_MESH_INVALID", "'" + debugName(ms) + "' has no triangles");
            MeshEntry entry;
            entry.mesh = render::uploadMesh(m_scene, *mesh, debugName(ms));
            if (!entry.mesh.valid())
                return fail(e, "E_MESH_INVALID", "'" + debugName(ms) + "' could not be uploaded to the render scene");
            // The mesh's own materials, their textures by content from the cooked cache.
            for (const render::MaterialDesc& d : render::meshMaterials(
                     m_textures, *mesh, [this](const ContentHash& name) { return m_assets.texture(name); }))
                entry.modelMaterials.push_back(m_scene.registerMaterial(d));
            it = m_meshes.emplace(std::move(key), std::move(entry)).first;
        }
        MeshEntry& entry = it->second;

        // Switch the entity to this mesh (the old one may lose its last user).
        if (const auto user = m_userOf.find(e); user == m_userOf.end() || user->second != it->first) {
            ++entry.users;
            dropUser(e);
            m_userOf[e] = it->first;
        }

        const auto* overrideMat = m_reg.try_get<Material>(e);
        const render::MaterialDesc desc = overrideMat ? describe(*overrideMat) : render::MaterialDesc{};
        std::vector<render::MaterialHandle> materials =
            entry.modelMaterials.empty() ? std::vector<render::MaterialHandle>{material(desc)} : entry.modelMaterials;
        const auto* previous = m_reg.try_get<MeshRenderer>(e);
        const bool visible = previous ? previous->visible : true;
        m_reg.emplace_or_replace<MeshRenderer>(e, MeshRenderer{entry.mesh, std::move(materials), visible});
        m_reg.remove<InstantiateError>(e);
    }

    render::MaterialHandle material(const render::MaterialDesc& d) {
        // Every value a scene material sets, bit for bit: two materials differing in any of them
        // are two table entries.
        const float values[] = {d.baseColor.r, d.baseColor.g, d.baseColor.b, d.roughness, d.metallic,
                                d.baseWeight, d.diffuseRoughness, d.specularWeight, d.specularColor.r,
                                d.specularColor.g, d.specularColor.b, d.specularIor, d.emissiveStrength,
                                d.emissionColor.r, d.emissionColor.g, d.emissionColor.b, d.subsurface,
                                static_cast<float>(d.alphaMode), d.opacity, d.specularAnisotropy, d.coatWeight,
                                d.coatColor.r, d.coatColor.g, d.coatColor.b, d.coatRoughness, d.coatAnisotropy,
                                d.coatIor, d.coatDarkening, d.fuzzWeight, d.fuzzColor.r, d.fuzzColor.g,
                                d.fuzzColor.b, d.fuzzRoughness, d.tangentRotation, d.transmissionWeight,
                                d.transmissionColor.r, d.transmissionColor.g, d.transmissionColor.b,
                                d.transmissionDepth, d.dispersionScale, d.dispersionAbbe,
                                d.thinWalled ? 1.0f : 0.0f, d.thinFilmWeight, d.thinFilmThickness, d.thinFilmIor,
                                d.subsurfaceColor.r, d.subsurfaceColor.g, d.subsurfaceColor.b, d.subsurfaceRadius,
                                d.subsurfaceRadiusScale.r, d.subsurfaceRadiusScale.g, d.subsurfaceRadiusScale.b,
                                d.subsurfaceAnisotropy};
        static_assert(std::size(values) == std::tuple_size_v<MaterialKey>, "a key holds every value");
        MaterialKey key{};
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = std::bit_cast<uint32_t>(values[i]);
        if (const auto it = m_materials.find(key); it != m_materials.end()) return it->second;
        const render::MaterialHandle h = m_scene.registerMaterial(d);
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
        m_reg.remove<InstantiateError>(e);
    }

    void dropUser(entt::entity e) {
        const auto user = m_userOf.find(e);
        if (user == m_userOf.end()) return;
        const auto it = m_meshes.find(user->second);
        m_userOf.erase(user);
        if (it == m_meshes.end() || --it->second.users > 0) return;
        m_scene.releaseMesh(it->second.mesh);
        m_meshes.erase(it);
    }

    void markDirty(entt::registry& reg, entt::entity e) { reg.emplace_or_replace<InstantiateDirty>(e); }
    // Only queued: the entity may be in the middle of being destroyed.
    void onSourceDestroy(entt::registry&, entt::entity e) { m_sourceGone.push_back(e); }
    void onOverrideDestroy(entt::registry&, entt::entity e) { m_overrideGone.push_back(e); }

    World& m_world;
    entt::registry& m_reg;
    render::RenderScene& m_scene;
    BindlessTextureManager* m_textures;
    AssetResolver& m_assets;
    std::map<std::string, MeshEntry> m_meshes;   // by meshKey
    ankerl::unordered_dense::map<entt::entity, std::string> m_userOf;
    using MaterialKey = std::array<uint32_t, 53>;   // material()'s values, one each
    std::map<MaterialKey, render::MaterialHandle> m_materials;
    std::vector<entt::entity> m_sourceGone, m_overrideGone;
    std::vector<std::pair<uint64_t, entt::entity>> m_dirty;
    std::unordered_set<std::string> m_logged;
};

} // namespace

void installInstantiate(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        AssetResolver& assets) {
    auto& reg = world.registry();
    if (reg.ctx().contains<InstantiateInstalled>()) {
        OHAO_LOG(Warn, ecs, "installInstantiate called twice on one World; ignored");
        return;
    }
    reg.ctx().emplace<InstantiateInstalled>();
    auto state = std::make_shared<InstantiateState>(world, scene, textures, assets);
    // A level's MeshSources, once it expanded them.
    world.addSync("instantiate", [state](World&) { state->run(); }, {.after = {"level"}});
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
