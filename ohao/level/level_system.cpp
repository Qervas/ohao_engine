#include "level/level_system.hpp"

#include "core/log.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "level/gltf_structure.hpp"
#include "level/level_hints.hpp"
#include "reflect/registry.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ohao::level {

using ecs::World;
using reflect::Diagnostic;
using reflect::TypeInfo;

namespace {

constexpr uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// The node a hull named UCX_<target>[_NN] belongs to: <target> as written, else without its
// _NN suffix; null for any other name or a target the level does not have.
entt::entity ucxTarget(std::string_view name,
                       const ankerl::unordered_dense::map<std::string_view, entt::entity>& byName) {
    if (!name.starts_with("UCX_")) return entt::null;
    std::string_view target = name.substr(4);
    if (const auto it = byName.find(target); it != byName.end()) return it->second;
    const auto cut = target.rfind('_');
    if (cut == std::string_view::npos || cut + 1 == target.size()) return entt::null;
    for (const char c : target.substr(cut + 1))
        if (c < '0' || c > '9') return entt::null;
    const auto it = byName.find(target.substr(0, cut));
    return it == byName.end() ? entt::entity{entt::null} : it->second;
}

// A component value of a registered type, default-constructed in its own aligned buffer.
using Value = std::shared_ptr<void>;
Value makeValue(const TypeInfo& t) {
    const std::align_val_t align{t.align ? t.align : alignof(std::max_align_t)};
    void* p = ::operator new(t.size, align);
    t.construct(p);
    return Value(p, [&t, align](void* q) {
        t.destroy(q);
        ::operator delete(q, align);
    });
}

class LevelState;
struct LevelInstalled {
    std::weak_ptr<LevelState> state;
};

class LevelState {
public:
    LevelState(World& world, ecs::AssetResolver& assets, const reflect::TypeRegistry& types)
        : m_world(world), m_reg(world.registry()), m_assets(assets), m_types(types) {
        // The saved components a node's file state sets or clears on (re)expansion: all but the
        // node's own Name, Transform and Parent, and Level (a node never holds one).
        for (const TypeInfo* t : types.types())
            if (t->role == reflect::TypeRole::Component && t->serialized && ecs::componentOps(*t) && !t->alwaysPresent &&
                t != types.find<ecs::Parent>() && t != types.find<ecs::Level>())
                m_managed.push_back(t);
        m_reg.on_construct<ecs::Level>().connect<&LevelState::markDirty>(this);
        m_reg.on_update<ecs::Level>().connect<&LevelState::markDirty>(this);
        m_reg.on_destroy<ecs::Level>().connect<&LevelState::onGone>(this);
        for (const entt::entity e : m_reg.view<ecs::Level>()) m_reg.emplace_or_replace<ecs::LevelDirty>(e);
    }

    ~LevelState() {
        m_reg.on_construct<ecs::Level>().disconnect(this);
        m_reg.on_update<ecs::Level>().disconnect(this);
        m_reg.on_destroy<ecs::Level>().disconnect(this);
    }

    void run() {
        for (const entt::entity e : m_gone) {
            m_diags.erase(e);
            m_info.erase(e);
            if (!m_reg.valid(e) || !m_reg.all_of<ecs::Level>(e)) removeNodes(e);
        }
        m_gone.clear();
        m_dirty.clear();
        for (const entt::entity e : m_reg.view<ecs::LevelDirty>()) {
            const auto* co = m_reg.try_get<ecs::CreationOrder>(e);
            m_dirty.emplace_back(co ? co->seq : UINT64_MAX, e);
        }
        m_reg.clear<ecs::LevelDirty>();
        std::sort(m_dirty.begin(), m_dirty.end());
        for (const auto& [seq, e] : m_dirty) {
            if (!m_reg.valid(e) || !m_reg.all_of<ecs::Level>(e)) continue;
            const auto t0 = std::chrono::steady_clock::now();
            expand(e);
            m_info[e].loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (const auto it = std::find(m_rebuild.begin(), m_rebuild.end(), e); it != m_rebuild.end()) {
                m_rebuild.erase(it);
                touchColliders(e);
            }
        }
    }

    [[nodiscard]] std::vector<Diagnostic> diagnostics(entt::entity level) const {
        const auto it = m_diags.find(level);
        return it == m_diags.end() ? std::vector<Diagnostic>{} : it->second;
    }

    [[nodiscard]] std::optional<LevelSummary> summary(entt::entity level) const {
        const auto it = m_info.find(level);
        if (it == m_info.end()) return std::nullopt;
        const Info& info = it->second;
        LevelSummary out;
        out.path = info.path;
        out.loaded = info.structure != nullptr;
        out.loadMs = info.loadMs;
        if (!info.structure) return out;
        const GltfStructure& s = *info.structure;
        out.nodes = static_cast<uint32_t>(s.nodes.size());
        std::vector<bool> used(s.meshes.size(), false);
        for (const GltfNode& n : s.nodes) {
            if (n.mesh >= 0 && static_cast<std::size_t>(n.mesh) < s.meshes.size()) {
                ++out.meshes;
                out.triangles += s.meshes[static_cast<std::size_t>(n.mesh)].triangles;
                if (!used[static_cast<std::size_t>(n.mesh)]) {
                    used[static_cast<std::size_t>(n.mesh)] = true;
                    ++out.uniqueMeshes;
                }
            }
            out.lights += n.light >= 0 ? 1u : 0u;
            out.cameras += n.camera >= 0 ? 1u : 0u;
        }
        return out;
    }

    // Forgets the file (its structure here, its meshes in the resolver) and re-expands the level
    // at the next run; its nodes' colliders are touched then, so the physics system rebuilds them.
    void reload(entt::entity level) {
        const ecs::Level& l = m_reg.get<ecs::Level>(level);
        if (const auto path = m_assets.absolutePath(l.asset); !path.empty()) m_structures.erase(path.generic_string());
        m_assets.forget(l.asset);
        m_reg.emplace_or_replace<ecs::LevelDirty>(level);
        if (std::find(m_rebuild.begin(), m_rebuild.end(), level) == m_rebuild.end()) m_rebuild.push_back(level);
    }

private:
    struct Info {
        std::shared_ptr<const GltfStructure> structure;   // null: the file did not load
        std::string path;
        double loadMs{0.0};
    };

    // An update signal on each collider of the level's nodes, values unchanged.
    void touchColliders(entt::entity level) {
        std::vector<entt::entity> nodes;
        for (auto [e, ln] : m_reg.view<const ecs::LevelNode>().each())
            if (ln.level == level && m_reg.all_of<ecs::Collider>(e)) nodes.push_back(e);
        for (const entt::entity e : nodes) m_reg.patch<ecs::Collider>(e);
    }

    void markDirty(entt::registry& r, entt::entity e) { r.emplace_or_replace<ecs::LevelDirty>(e); }
    void onGone(entt::registry&, entt::entity e) { m_gone.push_back(e); }   // only queued: e may be mid-destroy

    // Every node entity of `level` (its subtrees go with each root).
    void removeNodes(entt::entity level) {
        std::vector<entt::entity> nodes;
        for (auto [e, ln] : m_reg.view<const ecs::LevelNode>().each())
            if (ln.level == level) nodes.push_back(e);
        for (const entt::entity e : nodes)
            if (m_reg.valid(e)) m_world.destroy(e);
    }

    std::shared_ptr<const GltfStructure> structure(const ecs::AssetRef& ref, std::string& code, std::string& message) {
        const auto path = m_assets.absolutePath(ref);
        if (path.empty()) {
            code = "E_ASSET_NOT_FOUND";
            message = ref.path.empty() ? "the level's asset path is empty"
                                       : "'" + ref.path + "' is not a relative path inside the project";
            return nullptr;
        }
        const std::string key = path.generic_string();
        if (const auto it = m_structures.find(key); it != m_structures.end()) return it->second;
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            code = "E_ASSET_NOT_FOUND";
            message = "no file '" + ref.path + "' in the project";
            return nullptr;
        }
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto read = readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
        if (!read) {
            code = read.error().code;
            message = "'" + ref.path + "': " + read.error().message;
            return nullptr;
        }
        auto s = std::make_shared<const GltfStructure>(std::move(read.value()));
        m_structures.emplace(key, s);
        return s;
    }

    template <class T>
    T& add(std::vector<std::pair<const TypeInfo*, Value>>& want, T value) {
        const TypeInfo* t = m_types.find<T>();
        Value v = makeValue(*t);
        T& slot = *static_cast<T*>(v.get());
        slot = std::move(value);
        want.emplace_back(t, std::move(v));
        return slot;
    }

    // Sets the node's managed components to exactly `want` (the last value per type wins),
    // touching only what differs.
    void apply(entt::entity e, const std::vector<std::pair<const TypeInfo*, Value>>& want) {
        for (const TypeInfo* t : m_managed) {
            const ecs::ComponentOps* ops = ecs::componentOps(*t);
            void* value = nullptr;
            for (const auto& [type, v] : want)
                if (type == t) value = v.get();
            const void* have = ops->tryGet(m_reg, e);
            if (value) {
                if (!have || !reflect::equalStruct(have, value, *t)) ops->emplace(m_reg, e, value);
            } else if (have) {
                ops->remove(m_reg, e);
            }
        }
    }

    void expand(entt::entity e) {
        const ecs::Level level = m_reg.get<ecs::Level>(e);
        const uint64_t levelId = m_world.idOf(e);
        std::vector<Diagnostic>& diags = m_diags[e];
        diags.clear();
        std::string code, message;
        const auto s = structure(level.asset, code, message);
        m_info[e] = Info{s, level.asset.path, 0.0};
        if (!s) {
            removeNodes(e);
            Diagnostic d;
            d.code = code;
            d.message = message;
            diags.push_back(d);
            m_reg.emplace_or_replace<ecs::InstantiateError>(e, ecs::InstantiateError{code, message});
            OHAO_LOG(Error, asset, "level {:#x}: {}: {}", levelId, code, message);
            return;
        }
        for (const std::string& kind : s->unsupported) {
            Diagnostic d;
            d.code = "W_LEVEL_UNSUPPORTED";
            d.message = "'" + level.asset.path + "': " + kind + " are not supported; ignored";
            diags.push_back(std::move(d));
        }

        // The level's existing node entities, by index (a re-expansion keeps them).
        const std::size_t n = s->nodes.size();
        std::vector<entt::entity> existing(n, entt::entity{entt::null});
        std::vector<entt::entity> stale;
        for (auto [node, ln] : m_reg.view<const ecs::LevelNode>().each()) {
            if (ln.level != e) continue;
            if (ln.index < n && existing[ln.index] == entt::null) existing[ln.index] = node;
            else stale.push_back(node);
        }
        // A level camera is active only when no other camera is in the world.
        bool cameraTaken = false;
        for (auto [c, cam] : m_reg.view<const ecs::Camera>().each()) {
            const auto* ln = m_reg.try_get<ecs::LevelNode>(c);
            if (!ln || ln->level != e) cameraTaken = true;
        }

        std::vector<entt::entity> made(n, entt::entity{entt::null});
        std::vector<std::pair<const TypeInfo*, Value>> want;
        for (uint32_t i = 0; i < n; ++i) {
            const GltfNode& node = s->nodes[i];
            const LevelHints hints = parseHints(*s, static_cast<int>(i), m_types, diags);
            const entt::entity parent = node.parent < 0 ? e : made[static_cast<std::size_t>(node.parent)];
            const ecs::Transform local{node.translation, node.rotation, node.scale};
            entt::entity ent = existing[i];
            if (ent != entt::null && m_reg.valid(ent)) {
                if (m_reg.get<ecs::Name>(ent).value != node.name) m_reg.replace<ecs::Name>(ent, ecs::Name{node.name});
                const ecs::Transform& t = m_reg.get<ecs::Transform>(ent);
                if (t.position != local.position || t.rotation != local.rotation || t.scale != local.scale)
                    m_world.setTransform(ent, local);
                const auto* p = m_reg.try_get<ecs::Parent>(ent);
                if (!p || p->entity != parent) m_world.setParent(ent, parent);
            } else {
                const uint64_t id = levelNodeId(levelId, i);
                ent = m_world.createWithId(id, node.name, local);
                if (ent == entt::null) {
                    Diagnostic d;
                    d.code = "E_LEVEL_NODE";
                    d.path = nodePath(*s, static_cast<int>(i));
                    d.message = "node '" + d.path + "': its id is already another entity's; the node is left out";
                    diags.push_back(std::move(d));
                    made[i] = parent;   // its children attach to its parent
                    continue;
                }
                m_world.attach(ent, parent);
                m_reg.emplace<ecs::LevelNode>(ent, ecs::LevelNode{e, i});
            }
            made[i] = ent;

            want.clear();
            if (node.mesh >= 0 && hints.visible)
                add(want, ecs::MeshSource{ecs::ModelAsset{level.asset, "#" + std::to_string(node.mesh)}});
            if (node.light >= 0 && static_cast<std::size_t>(node.light) < s->lights.size()) {
                const GltfLight& gl = s->lights[static_cast<std::size_t>(node.light)];
                ecs::Light l;
                l.type = gl.type == GltfLight::Type::Spot          ? ecs::LightType::Spot
                         : gl.type == GltfLight::Type::Directional ? ecs::LightType::Directional
                                                                   : ecs::LightType::Point;
                l.color = gl.color;
                l.intensity = gl.intensity * level.lightScale;
                if (gl.range > 0.0f) l.range = gl.range;   // else OHAO's default range
                if (l.type == ecs::LightType::Spot) {
                    l.innerConeDeg = glm::degrees(gl.innerCone);
                    l.outerConeDeg = glm::degrees(gl.outerCone);
                }
                add(want, l);
            }
            if (node.camera >= 0 && static_cast<std::size_t>(node.camera) < s->cameras.size()) {
                const GltfCamera& gc = s->cameras[static_cast<std::size_t>(node.camera)];
                add(want, ecs::Camera{glm::degrees(gc.yfovRad), gc.znear, gc.zfar > 0.0f ? gc.zfar : 200.0f, !cameraTaken});
                cameraTaken = true;
            }
            if (!hints.spawn.empty()) add(want, ecs::SpawnPoint{hints.spawn});
            if (!hints.socket.empty()) add(want, ecs::Socket{hints.socket});
            if (!hints.verb.empty()) add(want, ecs::ReverbZone{hints.verb});   // size 0: fitted to the node's mesh
            if (!hints.vfx.empty()) add(want, ecs::VfxEmitter{hints.vfx, 0.0f, 0u, true});   // rate 0: one spawn, where it stands
            // Collision: a collider on mesh nodes unless col says otherwise, a RigidBody only
            // when a body hint is given (no RigidBody is static). None for a render-only level.
            const ecs::ColliderShape col = hints.col.value_or(node.mesh >= 0 ? ecs::ColliderShape::Mesh : ecs::ColliderShape::None);
            if (level.collision && col != ecs::ColliderShape::None) {
                ecs::Collider c{col};
                c.trigger = hints.trigger;
                c.surface = hints.surface;
                add(want, c);
            }
            if (level.collision && (hints.body || hints.mass || hints.fric || hints.rest)) {
                ecs::RigidBody rb;
                rb.motion = hints.body.value_or(ecs::Motion::Static);
                rb.mass = hints.mass.value_or(rb.mass);
                rb.friction = hints.fric.value_or(rb.friction);
                rb.restitution = hints.rest.value_or(rb.restitution);
                add(want, rb);
            }
            for (const auto& [type, json] : hints.components) {
                if (!level.collision && (type == m_types.find<ecs::Collider>() || type == m_types.find<ecs::RigidBody>()))
                    continue;
                auto doc = ecs::parseJson(json);
                if (!doc) continue;
                Value v = makeValue(*type);
                reflect::DecodeContext ctx(doc.value());
                reflect::readStruct(v.get(), *type, doc.value().root(), ctx);
                if (ctx.errors.empty()) want.emplace_back(type, std::move(v));
            }
            apply(ent, want);
            // A collider on a node that is not drawn is fitted to the node's mesh all the same.
            const bool collides = level.collision && m_reg.all_of<ecs::Collider>(ent);
            if (collides && node.mesh >= 0 && !m_reg.all_of<ecs::MeshSource>(ent)) {
                const ecs::CollisionMesh cm{ecs::ModelAsset{level.asset, "#" + std::to_string(node.mesh)}};
                const auto* have = m_reg.try_get<ecs::CollisionMesh>(ent);
                if (!have || have->source.asset.path != cm.source.asset.path || have->source.asset.guid != cm.source.asset.guid ||
                    have->source.mesh != cm.source.mesh)
                    m_reg.emplace_or_replace<ecs::CollisionMesh>(ent, cm);
            } else if (m_reg.all_of<ecs::CollisionMesh>(ent)) {
                m_reg.remove<ecs::CollisionMesh>(ent);
            }
        }
        // UCX_<target>[_NN] hulls join the body of the node named <target> (the first by index).
        ankerl::unordered_dense::map<std::string_view, entt::entity> byName;
        for (uint32_t i = 0; i < n; ++i)
            if (made[i] != entt::null && made[i] != e) byName.try_emplace(s->nodes[i].name, made[i]);
        for (uint32_t i = 0; i < n; ++i) {
            const entt::entity ent = made[i];
            if (ent == entt::null || ent == e || !m_reg.valid(ent) || m_reg.get<ecs::LevelNode>(ent).index != i) continue;
            entt::entity target = entt::null;
            if (level.collision && m_reg.all_of<ecs::Collider>(ent)) target = ucxTarget(s->nodes[i].name, byName);
            if (target != entt::null && target != ent) {
                const auto* have = m_reg.try_get<ecs::ColliderPart>(ent);
                if (!have || have->body != target) m_reg.emplace_or_replace<ecs::ColliderPart>(ent, ecs::ColliderPart{target});
            } else if (m_reg.all_of<ecs::ColliderPart>(ent)) {
                m_reg.remove<ecs::ColliderPart>(ent);
            }
        }
        for (const entt::entity old : stale)   // nodes the file no longer has
            if (m_reg.valid(old)) m_world.destroy(old);

        std::size_t errors = 0;
        const Diagnostic* first = nullptr;
        for (const Diagnostic& d : diags) {
            const bool error = d.code.starts_with("E_");
            if (error && !first) first = &d;
            errors += error ? 1 : 0;
            if (error) OHAO_LOG(Error, asset, "level {:#x} ({}): {}: {}", levelId, level.asset.path, d.code, d.message);
            else OHAO_LOG(Warn, asset, "level {:#x} ({}): {}: {}", levelId, level.asset.path, d.code, d.message);
        }
        if (first) {
            const std::string what = std::to_string(errors) + (errors == 1 ? " error" : " errors") +
                                     " in the level's nodes, which load without the bad parts; the first: " + first->message;
            m_reg.emplace_or_replace<ecs::InstantiateError>(
                e, ecs::InstantiateError{first->code == "E_LEVEL_NODE" ? "E_LEVEL_NODE" : "E_LEVEL_EXTRAS", what});
        } else if (!m_reg.all_of<ecs::MeshSource>(e)) {
            m_reg.remove<ecs::InstantiateError>(e);
        }
        OHAO_LOG(Debug, asset, "level {:#x}: '{}' expanded, {} nodes", levelId, level.asset.path, n);
    }

    World& m_world;
    entt::registry& m_reg;
    ecs::AssetResolver& m_assets;
    const reflect::TypeRegistry& m_types;
    std::vector<const TypeInfo*> m_managed;
    std::map<std::string, std::shared_ptr<const GltfStructure>> m_structures;   // by absolute path
    ankerl::unordered_dense::map<entt::entity, std::vector<Diagnostic>> m_diags;
    std::vector<entt::entity> m_gone;
    std::vector<std::pair<uint64_t, entt::entity>> m_dirty;
    ankerl::unordered_dense::map<entt::entity, Info> m_info;   // per level: its last expansion
    std::vector<entt::entity> m_rebuild;                        // levels reloaded: touch their colliders
};

} // namespace

uint64_t levelNodeId(uint64_t levelId, uint32_t index) noexcept {
    const uint64_t id = splitmix64(splitmix64(levelId ^ 0x6c6576656c6e6f64ull) + index);   // "levelnod"
    return id != 0 ? id : 1;
}

void installLevels(World& world, ecs::AssetResolver& assets, const reflect::TypeRegistry& types) {
    auto& reg = world.registry();
    if (reg.ctx().contains<LevelInstalled>()) {
        OHAO_LOG(Warn, ecs, "installLevels called twice on one World; ignored");
        return;
    }
    auto state = std::make_shared<LevelState>(world, assets, types);
    reg.ctx().emplace<LevelInstalled>(LevelInstalled{state});
    world.addSync("level", [state](World&) { state->run(); });
}

std::vector<Diagnostic> levelDiagnostics(const World& world, entt::entity level) {
    const auto* installed = world.registry().ctx().find<LevelInstalled>();
    if (!installed) return {};
    const auto state = installed->state.lock();
    return state ? state->diagnostics(level) : std::vector<Diagnostic>{};
}

bool levelsInstalled(const World& world) {
    const auto* installed = world.registry().ctx().find<LevelInstalled>();
    return installed && !installed->state.expired();
}

std::optional<LevelSummary> levelSummary(const World& world, entt::entity level) {
    const auto* installed = world.registry().ctx().find<LevelInstalled>();
    const auto state = installed ? installed->state.lock() : nullptr;
    return state ? state->summary(level) : std::nullopt;
}

bool reloadLevel(World& world, entt::entity level) {
    const auto* installed = world.registry().ctx().find<LevelInstalled>();
    const auto state = installed ? installed->state.lock() : nullptr;
    if (!state || !world.registry().valid(level) || !world.registry().all_of<ecs::Level>(level)) return false;
    state->reload(level);
    return true;
}

} // namespace ohao::level
