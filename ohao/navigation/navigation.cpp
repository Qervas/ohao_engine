#include "navigation/navigation.hpp"

#include "navigation/components.hpp"
#include "navigation/private/navmesh.hpp"

#include "asset/content_hash.hpp"
#include "core/jobs/graph.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "physics/components.hpp"
#include "visual/components.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

namespace ohao::ecs {

void installNavigation(World& world, AssetResolver& assets);

namespace {

constexpr std::size_t kGrain = 8;           // agents a chunk of the step's parallelFor
constexpr uint32_t kMaxCorners = 32;        // a NavPath's corners
constexpr float kCornerReach = 0.4f;        // m: a corner this near (horizontally) is passed
constexpr float kNeighbourRange = 3.0f;     // m: the agents an agent steers around
constexpr std::size_t kMaxNeighbours = 16;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The entity's world matrix from the Transforms up its Parents (not WorldTransform, which a restore leaves stale until
// the next propagation).
template <class R>
glm::mat4 worldOf(const R& reg, entt::entity e) {
    glm::mat4 m(1.0f);
    for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
        if (const auto* t = reg.template try_get<Transform>(e)) m = t->matrix() * m;
        const auto* p = reg.template try_get<Parent>(e);
        e = p ? p->entity : entt::entity{entt::null};
    }
    return m;
}

// A box's twelve triangles, wound to face out, in world space.
void addBox(nav::Geometry& g, const glm::mat4& world, glm::vec3 centre, glm::vec3 half) {
    const auto base = static_cast<uint32_t>(g.verts.size());
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local = centre + half * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
        c[i] = glm::vec3(world * glm::vec4(local, 1.0f));
        g.verts.push_back(c[i]);
    }
    const glm::vec3 mid = glm::vec3(world * glm::vec4(centre, 1.0f));
    static constexpr int kQuads[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& q : kQuads)
        for (const auto& tri : {std::array<int, 3>{q[0], q[1], q[2]}, std::array<int, 3>{q[0], q[2], q[3]}}) {
            const glm::vec3 n = glm::cross(c[tri[1]] - c[tri[0]], c[tri[2]] - c[tri[0]]);
            const bool out = glm::dot(n, (c[tri[0]] + c[tri[1]] + c[tri[2]]) / 3.0f - mid) >= 0.0f;
            g.tris.push_back(base + static_cast<uint32_t>(tri[0]));
            g.tris.push_back(base + static_cast<uint32_t>(out ? tri[1] : tri[2]));
            g.tris.push_back(base + static_cast<uint32_t>(out ? tri[2] : tri[1]));
        }
}

std::shared_ptr<const cooked::Mesh> meshOf(AssetResolver& assets, const MeshSource* ms, const CollisionMesh* cm) {
    const ModelAsset* asset = cm ? &cm->source : (ms ? std::get_if<ModelAsset>(&ms->source) : nullptr);
    if (!asset) return nullptr;
    auto loaded = assets.loadMesh(asset->asset, asset->mesh);
    return loaded ? std::move(loaded.value()) : nullptr;
}

// The mesh's bounds in its own frame.
std::pair<glm::vec3, glm::vec3> boundsOf(const cooked::Mesh& mesh) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Vertex& v : mesh.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    return {lo, hi};
}

// The yaw (radians, about +Y) of a world matrix's rotation: where its -Z faces.
float yawOf(const glm::mat4& m) {
    const glm::vec3 fwd = -glm::vec3(m[2]);
    return std::atan2(-fwd.x, -fwd.z);
}

struct Agent {
    entt::entity e{entt::null};
    uint64_t id{0};
    glm::vec3 pos{0.0f}, vel{0.0f};
    float radius{0.3f}, walkSpeed{4.0f}, yawDeg{0.0f};
    NavAgent agent;
};

struct Steer {
    bool planned{false}, pathChanged{false};
    NavPath path;
    MoveIntent intent;
};

class NavSystem {
public:
    NavSystem(World& world, AssetResolver& assets) : m_world(world), m_assets(assets) {
        const SignalEffect dirty{.writesResources = {"navigation.geometry"}};
        world.onConstruct<Collider, &NavSystem::onGeometry>(this, dirty);
        world.onUpdate<Collider, &NavSystem::onGeometry>(this, dirty);
        world.onDestroy<Collider, &NavSystem::onGeometry>(this, dirty);
        world.onConstruct<NavLink, &NavSystem::onGeometry>(this, dirty);
        world.onUpdate<NavLink, &NavSystem::onGeometry>(this, dirty);
        world.onDestroy<NavLink, &NavSystem::onGeometry>(this, dirty);
        world.onConstruct<NavObstacle, &NavSystem::onGeometry>(this, dirty);
        world.onDestroy<NavObstacle, &NavSystem::onGeometry>(this, dirty);
        world.onConstruct<PhysicsMoved, &NavSystem::onMoved>(this, dirty);
    }
    ~NavSystem() {
        m_world.disconnect<Collider>(this);
        m_world.disconnect<NavLink>(this);
        m_world.disconnect<NavObstacle>(this);
        m_world.disconnect<PhysicsMoved>(this);
    }

    // The navmesh brought up to the state: built when its geometry changed, its obstacles applied.
    template <class R>
    void prepare(const R& reg) {
        if (m_dirty) build(reg);
        applyObstacles(reg);
    }

    template <class R>
    void build(const R& reg) {
        m_dirty = false;
        nav::Geometry g;
        // Static colliders in id order (a function of the state, not of creation or storage order).
        std::vector<std::pair<uint64_t, entt::entity>> colliders;
        for (auto [e, c] : reg.template view<const Collider>().each()) {
            if (c.trigger || reg.template all_of<NavObstacle>(e)) continue;
            entt::entity bodyOf = e;
            if (const auto* part = reg.template try_get<ColliderPart>(e)) bodyOf = part->body;
            if (const auto* rb = reg.template try_get<RigidBody>(bodyOf); rb && rb->motion != Motion::Static) continue;
            colliders.emplace_back(m_world.idOf(e), e);
        }
        std::sort(colliders.begin(), colliders.end());
        uint32_t skipped = 0;
        for (const auto& [id, e] : colliders) {
            const Collider& c = reg.template get<Collider>(e);
            const glm::mat4 world = worldOf(reg, e);
            const bool explicitSize = c.size != glm::vec3(0.0f) &&
                                      (c.shape == ColliderShape::Box || c.shape == ColliderShape::Sphere ||
                                       c.shape == ColliderShape::Capsule);
            if (explicitSize) {
                const glm::vec3 half = c.shape == ColliderShape::Box      ? c.size
                                       : c.shape == ColliderShape::Sphere ? glm::vec3(c.size.x)
                                                                          : glm::vec3(c.size.x, c.size.y + c.size.x, c.size.x);
                addBox(g, world, c.offset, half);
                continue;
            }
            const auto mesh = meshOf(m_assets, reg.template try_get<MeshSource>(e), reg.template try_get<CollisionMesh>(e));
            if (!mesh || mesh->vertices.empty()) {
                ++skipped;
                continue;
            }
            if (c.shape == ColliderShape::Mesh || c.shape == ColliderShape::Convex) {
                const auto base = static_cast<uint32_t>(g.verts.size());
                for (const Vertex& v : mesh->vertices) g.verts.push_back(glm::vec3(world * glm::vec4(v.position, 1.0f)));
                const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;
                for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
                    g.tris.push_back(base + mesh->indices[i]);
                    g.tris.push_back(base + mesh->indices[i + (mirrored ? 2 : 1)]);
                    g.tris.push_back(base + mesh->indices[i + (mirrored ? 1 : 2)]);
                }
            } else {
                const auto [lo, hi] = boundsOf(*mesh);
                addBox(g, world, (lo + hi) * 0.5f, (hi - lo) * 0.5f);
            }
        }
        std::vector<std::pair<uint64_t, entt::entity>> links;
        for (auto [e, l] : reg.template view<const NavLink>().each()) links.emplace_back(m_world.idOf(e), e);
        std::sort(links.begin(), links.end());
        for (const auto& [id, e] : links) {
            const NavLink& l = reg.template get<NavLink>(e);
            const glm::mat4 world = worldOf(reg, e);
            nav::Link out;
            out.a = glm::vec3(world[3]);
            out.b = glm::vec3(world * glm::vec4(l.end, 1.0f));
            out.radius = l.radius;
            out.bidirectional = l.bidirectional;
            out.id = static_cast<uint32_t>(g.links.size());
            g.links.push_back(out);
        }
        m_triangles = static_cast<uint32_t>(g.tris.size() / 3);
        m_links = static_cast<uint32_t>(g.links.size());
        // The geometry's hash: every triangle and link, with the settings.
        ContentHasher h;
        h.text("ohao.navmesh");
        h.value(m_settings.cellSize); h.value(m_settings.cellHeight); h.value(m_settings.tileCells);
        h.value(m_settings.agentRadius); h.value(m_settings.agentHeight); h.value(m_settings.agentClimb);
        h.value(m_settings.slopeDeg);
        for (const uint32_t t : g.tris) h.value(g.verts[t]);
        for (const nav::Link& l : g.links) {
            h.value(l.a); h.value(l.b); h.value(l.radius); h.value(l.bidirectional);
        }
        m_geometryHash = h.finish().lo;
        std::filesystem::path cache;
        if (const auto shape = m_assets.shapeFile(ContentHash{}); !shape.empty()) cache = shape.parent_path();
        m_mesh.build(g, m_settings, cache, m_report);
        if (skipped) m_report.warnings.push_back(std::to_string(skipped) + " colliders had no mesh to bake; left out");
        m_applied.clear();
        m_appliedHash = 0;
        m_built = true;
        m_queries.clear();
        m_methodQuery.reset();
        OHAO_LOG(Info, ecs, "navigation: {} tiles ({} cooked, {} cached), {} polygons, {} links in {:.1f} ms",
                 m_report.tiles, m_report.cooked, m_report.cached, m_report.polys, g.links.size(), m_report.loadMs);
    }

    // The active NavObstacles, in id order, applied to the tile cache when they differ from what it holds.
    template <class R>
    void applyObstacles(const R& reg) {
        std::vector<nav::Box> boxes;
        for (auto [e, ob] : reg.template view<const NavObstacle>().each()) {
            if (!ob.active) continue;
            const glm::mat4 world = worldOf(reg, e);
            glm::vec3 centre(0.0f), half = ob.halfExtents;
            if (half == glm::vec3(0.0f)) {
                const auto mesh = meshOf(m_assets, reg.template try_get<MeshSource>(e), reg.template try_get<CollisionMesh>(e));
                if (!mesh || mesh->vertices.empty()) continue;
                const auto [lo, hi] = boundsOf(*mesh);
                centre = (lo + hi) * 0.5f;
                half = (hi - lo) * 0.5f;
            }
            const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                  glm::length(glm::vec3(world[2])));
            nav::Box b;
            b.id = m_world.idOf(e);
            b.centre = glm::vec3(world * glm::vec4(centre, 1.0f));
            b.half = half * scale;
            b.yaw = yawOf(world);
            boxes.push_back(b);
        }
        std::sort(boxes.begin(), boxes.end(), [](const nav::Box& a, const nav::Box& b) { return a.id < b.id; });
        ContentHasher h;
        h.text("ohao.navobstacles");
        for (const nav::Box& b : boxes) {
            h.value(b.id); h.value(b.centre); h.value(b.half); h.value(b.yaw);
        }
        const uint64_t setHash = boxes.empty() ? 0 : h.finish().lo;
        m_obstaclesMs = 0.0;
        if (setHash == m_appliedHash) return;
        const auto t0 = std::chrono::steady_clock::now();
        m_mesh.setObstacles(boxes);
        m_appliedHash = setHash;
        m_applied = std::move(boxes);
        m_obstaclesMs = msSince(t0);
        m_lastObstaclesMs = m_obstaclesMs;
    }

    [[nodiscard]] uint64_t version() const noexcept {
        if (!m_built || m_mesh.empty()) return 0;
        const uint64_t v = m_geometryHash ^ (m_appliedHash * 0x9e3779b97f4a7c15ull);
        return v ? v : 1;
    }

    // The `navigation` simulate system.
    void step(SystemContext& ctx, float dt) {
        const auto t0 = std::chrono::steady_clock::now();
        m_timing = NavStepTiming{};
        std::vector<Agent> agents;
        for (auto [e, a, pc] : ctx.view<const NavAgent, const PlayerController>().each()) {
            if (!a.active || pc.input) continue;
            Agent x;
            x.e = e;
            x.id = ctx.idOf(e);
            x.agent = a;
            x.radius = pc.radius;
            x.walkSpeed = pc.walkSpeed;
            x.pos = glm::vec3(worldOf(ctx, e)[3]);
            if (const auto* bm = ctx.try_get<BodyMotion>(e)) x.vel = bm->linear;
            if (const auto* mi = ctx.try_get<MoveIntent>(e)) x.yawDeg = mi->yawDeg;
            agents.push_back(x);
        }
        if (agents.empty()) return;
        std::sort(agents.begin(), agents.end(), [](const Agent& a, const Agent& b) { return a.id < b.id; });
        prepare(ctx);
        m_timing.obstaclesMs = m_obstaclesMs;
        const uint64_t version = this->version();
        const uint64_t step = ctx.world().steps();
        const auto t1 = std::chrono::steady_clock::now();

        // One query per chunk: Detour's node pool is per query, so a chunk's results depend on its agents alone.
        const std::size_t chunks = (agents.size() + kGrain - 1) / kGrain;
        while (m_queries.size() < chunks) m_queries.push_back(std::make_unique<nav::Query>(m_mesh));
        std::vector<NavPath> paths(agents.size());
        for (std::size_t i = 0; i < agents.size(); ++i)
            if (const auto* p = ctx.try_get<NavPath>(agents[i].e)) paths[i] = *p;
        std::vector<Steer> out(agents.size());
        jobs::parallelFor(0, agents.size(), kGrain, [&](std::size_t lo, std::size_t hi) {
            nav::Query& q = *m_queries[lo / kGrain];
            for (std::size_t i = lo; i < hi; ++i) out[i] = steer(q, agents, i, paths[i], version, step, dt);
        }, "navigation.agents");

        for (std::size_t i = 0; i < agents.size(); ++i) {
            const Steer& s = out[i];
            if (s.pathChanged) ctx.emplace_or_replace<NavPath>(agents[i].e, s.path);
            ctx.emplace_or_replace<MoveIntent>(agents[i].e, s.intent);
            m_timing.planned += s.planned ? 1u : 0u;
        }
        m_timing.agents = static_cast<uint32_t>(agents.size());
        m_timing.agentsMs = msSince(t1);
        m_timing.totalMs = msSince(t0);
    }

    // One agent's plan, follow and steer: a function of the agents as the step began, its path and the navmesh.
    Steer steer(nav::Query& q, const std::vector<Agent>& agents, std::size_t i, const NavPath& have, uint64_t version,
                uint64_t step, float dt) const {
        const Agent& a = agents[i];
        Steer s;
        s.path = have;
        s.intent.yawDeg = a.yawDeg;
        const auto every = static_cast<uint64_t>(std::max(1.0f, std::round(a.agent.replanSeconds / std::max(dt, 1e-6f))));
        const bool due = (step + a.id) % every == 0;
        if (have.status == NavPathStatus::None || have.version != version || due) {
            const nav::PathResult r = q.path(a.pos, a.agent.goal, kMaxCorners);
            s.planned = true;
            s.pathChanged = true;
            s.path.corners = r.corners;
            s.path.next = r.corners.size() > 1 ? 1u : 0u;
            s.path.status = r.status == nav::PathResult::Status::Found     ? NavPathStatus::Found
                            : r.status == nav::PathResult::Status::Partial ? NavPathStatus::Partial
                                                                           : NavPathStatus::NoPath;
            s.path.plannedStep = step;
            s.path.version = version;
            s.path.goal = a.agent.goal;
        }
        NavPath& p = s.path;
        if (p.status == NavPathStatus::NoPath || p.corners.empty()) return s;
        const auto flat = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
        const auto last = static_cast<uint32_t>(p.corners.size() - 1);
        while (p.next < last && glm::length(flat(p.corners[p.next]) - flat(a.pos)) < kCornerReach) {
            ++p.next;
            s.pathChanged = true;
        }
        const glm::vec2 toEnd = flat(p.corners[last]) - flat(a.pos);
        if (p.next >= last && glm::length(toEnd) < a.agent.arrive) {
            if (p.status == NavPathStatus::Found) {
                s.pathChanged = s.pathChanged || p.status != NavPathStatus::Arrived;
                p.status = NavPathStatus::Arrived;
            }
            return s;   // it stands
        }
        if (p.status == NavPathStatus::Arrived) return s;
        const glm::vec2 d = flat(p.corners[std::min(p.next, last)]) - flat(a.pos);
        const float len = glm::length(d);
        if (len < 1e-4f) return s;
        const float maxSpeed = a.walkSpeed * std::clamp(a.agent.speed, 0.0f, 1.0f);
        glm::vec3 desired(d.x / len * maxSpeed, 0.0f, d.y / len * maxSpeed);
        glm::vec3 vel = desired;
        if (a.agent.avoid) {
            std::vector<std::pair<float, std::size_t>> near;
            for (std::size_t j = 0; j < agents.size(); ++j) {
                if (j == i) continue;
                const float dist = glm::length(flat(agents[j].pos) - flat(a.pos));
                if (dist < kNeighbourRange && std::abs(agents[j].pos.y - a.pos.y) < 2.0f) near.emplace_back(dist, j);
            }
            std::sort(near.begin(), near.end());
            std::vector<nav::Neighbour> ns;
            for (std::size_t k = 0; k < near.size() && k < kMaxNeighbours; ++k) {
                const Agent& b = agents[near[k].second];
                ns.push_back({glm::vec3(b.pos.x, 0.0f, b.pos.z), glm::vec3(b.vel.x, 0.0f, b.vel.z), b.radius});
            }
            vel = q.avoid(glm::vec3(a.pos.x, 0.0f, a.pos.z), a.radius, maxSpeed, glm::vec3(a.vel.x, 0.0f, a.vel.z), desired, ns);
        }
        const float speed = glm::length(glm::vec2(vel.x, vel.z));
        if (speed < 0.05f) return s;
        s.intent.yawDeg = glm::degrees(std::atan2(-vel.x, -vel.z));
        s.intent.move = glm::vec2(0.0f, std::min(1.0f, speed / std::max(a.walkSpeed, 1e-3f)));
        return s;
    }

    // Outside a step: the methods' navmesh and query.
    nav::Query* methodQuery() {
        prepare(std::as_const(m_world.registry()));
        if (!m_methodQuery) m_methodQuery = std::make_unique<nav::Query>(m_mesh);
        return m_methodQuery.get();
    }

    NavInfo info() {
        prepare(std::as_const(m_world.registry()));
        NavInfo n;
        n.built = m_built && !m_mesh.empty();
        n.version = version();
        n.tiles = m_report.tiles;
        n.cooked = m_report.cooked;
        n.cached = m_report.cached;
        n.layers = m_report.layers;
        n.polys = m_report.polys;
        n.obstacles = m_mesh.obstacleCount();
        n.triangles = m_triangles;
        n.links = m_links;
        n.cookMs = m_report.cookMs;
        n.buildMs = m_report.loadMs;
        n.obstaclesMs = m_lastObstaclesMs;
        n.cellSize = m_settings.cellSize;
        n.cellHeight = m_settings.cellHeight;
        n.tileSize = m_settings.tileSize();
        n.agentRadius = m_settings.agentRadius;
        n.agentHeight = m_settings.agentHeight;
        n.agentClimb = m_settings.agentClimb;
        n.slopeDeg = m_settings.slopeDeg;
        n.warnings = m_report.warnings;
        const auto& reg = std::as_const(m_world.registry());
        // Every spawn point a path from the player's spawn does not reach: an island, or a spawn off the navmesh.
        std::vector<std::pair<uint64_t, entt::entity>> spawns;
        for (auto [e, sp] : reg.view<const SpawnPoint>().each()) spawns.emplace_back(m_world.idOf(e), e);
        std::sort(spawns.begin(), spawns.end());
        const auto player = std::find_if(spawns.begin(), spawns.end(), [&](const auto& x) {
            return reg.get<SpawnPoint>(x.second).tag == "player";
        });
        if (n.built && player != spawns.end()) {
            const glm::vec3 from = glm::vec3(worldOf(reg, player->second)[3]);
            nav::Query* q = methodQuery();
            uint32_t unreached = 0;
            for (const auto& [id, e] : spawns) {
                if (e == player->second) continue;
                if (q->path(from, glm::vec3(worldOf(reg, e)[3]), 2).status == nav::PathResult::Status::Found) continue;
                if (++unreached <= 4) {
                    const auto* name = reg.try_get<Name>(e);
                    n.warnings.push_back("spawn '" + (name ? name->value : std::to_string(id)) + "' (" +
                                         reg.get<SpawnPoint>(e).tag + ") is not reachable from the player's spawn");
                }
            }
            if (unreached > 4) n.warnings.push_back(std::to_string(unreached - 4) + " more spawns are not reachable");
        }
        for (const entt::entity e : reg.view<const NavAgent>()) {
            (void)e;
            ++n.agents;
        }
        return n;
    }

    nav::Mesh& mesh() noexcept { return m_mesh; }
    [[nodiscard]] const std::vector<nav::Box>& obstacles() const noexcept { return m_applied; }
    [[nodiscard]] const NavStepTiming& timing() const noexcept { return m_timing; }

private:
    void onGeometry(entt::registry&, entt::entity) { m_dirty = true; }
    // A static collider moved by an edit (the transform system tags an entity with a body it moved).
    void onMoved(entt::registry& r, entt::entity e) {
        if (!r.all_of<Collider>(e)) return;
        if (const auto* rb = r.try_get<RigidBody>(e); rb && rb->motion != Motion::Static) return;
        m_dirty = true;
    }

    World& m_world;
    AssetResolver& m_assets;
    nav::Settings m_settings;
    nav::Mesh m_mesh;
    nav::BuildReport m_report;
    bool m_dirty{true}, m_built{false};
    uint32_t m_triangles{0}, m_links{0};
    uint64_t m_geometryHash{0}, m_appliedHash{0};
    std::vector<nav::Box> m_applied;
    double m_obstaclesMs{0.0}, m_lastObstaclesMs{0.0};   // this step's change (0: none); the last change
    std::vector<std::unique_ptr<nav::Query>> m_queries;   // one per chunk of the step's loop
    std::unique_ptr<nav::Query> m_methodQuery;
    NavStepTiming m_timing;
};

// The World's context names the system; its `navigation` simulate system owns it, so it goes with the World's
// systems, before the registry it disconnects from.
struct NavInstalled {
    std::weak_ptr<NavSystem> system;
};

std::shared_ptr<NavSystem> systemOf(const World& world) {
    const auto* installed = world.context().find<NavInstalled>();
    return installed ? installed->system.lock() : nullptr;
}

} // namespace

void installNavigation(World& world, AssetResolver& assets) {
    auto& ctx = world.context();
    if (ctx.contains<NavInstalled>()) return;
    auto system = std::make_shared<NavSystem>(world, assets);
    ctx.emplace<NavInstalled>(NavInstalled{system});
    // Before physics: it writes the MoveIntents the physics step moves the characters by.
    world.addSimulate("navigation", [system](SystemContext& c, float dt) { system->step(c, dt); },
                      {.before = {"physics"},
                       .reads = types<NavAgent, PlayerController, Transform, Parent, BodyMotion, NavObstacle, NavLink,
                                      Collider, ColliderPart, RigidBody, MeshSource, CollisionMesh>(),
                       .writes = types<NavPath, MoveIntent>(),
                       .readsResources = {"navigation.geometry"}});
}

std::optional<NavInfo> navInfo(World& world) {
    const auto s = systemOf(world);
    if (!s) return std::nullopt;
    return s->info();
}

std::optional<NavRoute> navPath(World& world, glm::vec3 from, glm::vec3 to, uint32_t maxCorners) {
    const auto s = systemOf(world);
    if (!s) return std::nullopt;
    const nav::PathResult r = s->methodQuery()->path(from, to, maxCorners);
    NavRoute out;
    out.status = r.status == nav::PathResult::Status::Found     ? NavRoute::Status::Found
                 : r.status == nav::PathResult::Status::Partial ? NavRoute::Status::Partial
                                                                : NavRoute::Status::NoPath;
    out.corners = r.corners;
    out.length = r.length;
    out.polys = r.polys;
    out.why = r.why;
    return out;
}

std::optional<glm::vec3> navNearest(World& world, glm::vec3 p, glm::vec3 extents) {
    const auto s = systemOf(world);
    if (!s) return std::nullopt;
    return s->methodQuery()->nearest(p, extents);
}

NavDrawing navDrawing(World& world) {
    NavDrawing d;
    if (const auto s = systemOf(world)) {
        (void)s->methodQuery();
        s->mesh().outlines(d.edges, d.links);
        for (const nav::Box& b : s->obstacles()) {
            const glm::mat4 m = glm::translate(glm::mat4(1.0f), b.centre) *
                                glm::rotate(glm::mat4(1.0f), b.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 c[8];
            for (int i = 0; i < 8; ++i)
                c[i] = glm::vec3(m * glm::vec4(b.half * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                                                                (i & 4) ? 1.0f : -1.0f), 1.0f));
            static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                                  {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : kEdges) d.obstacles.emplace_back(c[e[0]], c[e[1]]);
        }
    }
    return d;
}

NavStepTiming navStepTiming(const World& world) {
    const auto s = systemOf(world);
    return s ? s->timing() : NavStepTiming{};
}

} // namespace ohao::ecs
