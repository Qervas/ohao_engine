// asset.info and asset.reload (system design step 5h): the session's asset resolver, and the entities whose meshes
// and colliders use a model file. A reload rebuilds them through the World's sync systems (levels, transforms, bodies;
// never instantiate or the render extract), which do nothing when nothing changed.
#include "api/internal.hpp"

#include "ecs/components.hpp"
#include "visual/components.hpp"
#include "physics/components.hpp"

#include <map>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

// The derived state a reload rebuilds: the level's nodes, their world transforms, the bodies.
void syncPhysics(Session::Impl& s) {
    s.world.runSync("level");
    s.world.runSync("transform");
    s.world.runSync("physics");
}

} // namespace

// The entities whose MeshSource or CollisionMesh is the model file `path` (every model file when empty).
template <class F> void forModelUsers(Session::Impl& s, std::string_view path, F&& fn) {
    auto& reg = s.world.registry();
    for (const entt::entity e : reg.view<ecs::EntityId>()) {
        const auto* ms = reg.try_get<ecs::MeshSource>(e);
        const auto* model = ms ? std::get_if<ecs::ModelAsset>(&ms->source) : nullptr;
        const auto* cm = reg.try_get<ecs::CollisionMesh>(e);
        const bool mesh = model && (path.empty() || model->asset.path == path);
        const bool collision = cm && (path.empty() || cm->source.asset.path == path);
        if (mesh || collision) fn(e, mesh ? model->asset : cm->source.asset);
    }
}

bool methodAssetInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AssetInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    constexpr std::size_t kMaxModels = 32;
    std::map<std::string, uint32_t> users;
    forModelUsers(s, {}, [&](entt::entity, const ecs::AssetRef& ref) { ++users[ref.path]; });
    std::vector<ecs::AssetResolver::Loaded> loaded;
    if (s.assets) loaded = s.assets->loaded();
    out.beginObject();
    out.key("models");
    out.beginArray();
    for (std::size_t i = 0; i < loaded.size() && i < kMaxModels; ++i) {
        const auto& l = loaded[i];
        const std::string file = l.path.substr(0, l.path.find(" (meshes)"));
        const auto it = users.find(file);
        out.beginObject();
        out.key("path"); out.string(l.path);
        out.key("entities"); out.uint64(it == users.end() ? 0 : it->second);
        out.key("meshes"); out.uint64(l.meshes);
        out.key("cookedBytes"); out.uint64(l.cookedBytes);
        out.endObject();
    }
    out.endArray();
    if (loaded.size() > kMaxModels) { out.key("more"); out.uint64(loaded.size() - kMaxModels); }
    out.key("cache"); out.string(".ohao/cooked");
    out.endObject();
    return true;
}

bool methodAssetReload(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AssetReloadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!s.assets) {
        ctx.error("E_COMMAND", params, "this session has no asset resolver", "a host that loads assets (ohao serve, ohao run)");
        return false;
    }
    if (p.path) s.assets->forget(ecs::AssetRef{*p.path, {}});
    else s.assets->forgetAll();
    // The users' meshes and bodies are rebuilt from the file as it is now.
    auto& reg = s.world.registry();
    std::vector<entt::entity> touched;
    forModelUsers(s, p.path ? std::string_view(*p.path) : std::string_view{}, [&](entt::entity e, const ecs::AssetRef&) { touched.push_back(e); });
    for (const entt::entity e : touched) {
        if (reg.all_of<ecs::MeshSource>(e)) reg.patch<ecs::MeshSource>(e);
        if (reg.all_of<ecs::Collider>(e)) reg.patch<ecs::Collider>(e);
    }
    syncPhysics(s);
    out.beginObject();
    out.key("entities"); out.uint64(touched.size());
    out.endObject();
    return true;
}

} // namespace ohao::api
