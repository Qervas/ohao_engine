#include "ecs/physics_scene.hpp"

#include "core/log.hpp"
#include "ecs/instantiate.hpp"
#include "physics/world/physics_world.hpp"
#include "scene/asset/model.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

namespace ohao::ecs {

namespace pb = physics::backend;

namespace {

struct PhysicsInstalled {
    PhysicsScene* scene{nullptr};
};

// A world matrix as translation, rotation and (signed) scale: a mirror shows as a negative x.
struct Placement {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1.0f};
};

Placement decompose(const glm::mat4& m) {
    Placement p;
    p.position = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    p.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    if (glm::determinant(glm::mat3(m)) < 0.0f) p.scale.x = -p.scale.x;
    const auto unit = [](glm::vec3 c, float s) { return std::fabs(s) > 1e-12f ? c / s : c; };
    p.rotation = glm::normalize(glm::quat_cast(glm::mat3(unit(c0, p.scale.x), unit(c1, p.scale.y), unit(c2, p.scale.z))));
    return p;
}

bool differs(glm::vec3 a, glm::vec3 b, float eps) {
    return std::fabs(a.x - b.x) > eps || std::fabs(a.y - b.y) > eps || std::fabs(a.z - b.z) > eps;
}

float maxAbs(glm::vec3 v) { return std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}); }

// The scale's axes differ by more than 1%.
bool nonUniform(glm::vec3 s) {
    const glm::vec3 a = glm::abs(s);
    return maxAbs(s) - std::min({a.x, a.y, a.z}) > 0.01f * maxAbs(s);
}

pb::MotionType toBackend(Motion m) {
    switch (m) {
        case Motion::Dynamic: return pb::MotionType::DYNAMIC;
        case Motion::Kinematic: return pb::MotionType::KINEMATIC;
        case Motion::Static: break;
    }
    return pb::MotionType::STATIC;
}

} // namespace

FittedCollider fitCollider(const MeshData& mesh, ColliderShape shape, glm::vec3 scale) {
    FittedCollider f;
    f.shape = shape;
    // The mesh's own AABB: spheres and capsules are fitted to it, then scaled by the largest axis.
    glm::vec3 lo(0.0f), hi(0.0f);
    if (!mesh.positions.empty()) {
        lo = hi = mesh.positions.front();
        for (const glm::vec3& p : mesh.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    const glm::vec3 centre = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    const float largest = maxAbs(scale);
    f.vertices.reserve(mesh.positions.size());
    for (const glm::vec3& p : mesh.positions) f.vertices.push_back(p * scale);
    switch (shape) {
        case ColliderShape::Box:
            f.halfExtents = half * glm::abs(scale);
            f.offset = centre * scale;
            break;
        case ColliderShape::Sphere: {
            float r2 = 0.0f;
            for (const glm::vec3& p : mesh.positions) r2 = std::max(r2, glm::dot(p - centre, p - centre));
            f.radius = std::sqrt(r2) * largest;
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Capsule: {
            const int axis = half.x >= half.y && half.x >= half.z ? 0 : half.y >= half.z ? 1 : 2;
            const float a = half[(axis + 1) % 3], b = half[(axis + 2) % 3];
            f.radius = std::max(a, b) * largest;
            f.halfHeight = std::max(half[axis] * largest - f.radius, 0.0f);
            // The backend's capsule runs along +Y.
            if (axis == 0) f.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            if (axis == 2) f.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Mesh:
            f.indices = mesh.indices;
            // A mirror turns the triangles inside out; swapping two corners turns them back.
            if (scale.x * scale.y * scale.z < 0.0f)
                for (std::size_t i = 0; i + 2 < f.indices.size(); i += 3) std::swap(f.indices[i + 1], f.indices[i + 2]);
            break;
        case ColliderShape::Convex:
        case ColliderShape::None:
            break;
    }
    if (shape != ColliderShape::Convex && shape != ColliderShape::Mesh) f.vertices.clear();
    return f;
}

struct PhysicsScene::Impl {
    struct BodyRecord {
        pb::BodyHandle handle{pb::INVALID_BODY};
        Motion motion{Motion::Static};
        enum class Kind : uint8_t { Box, Sphere, Capsule, Convex, Mesh, Compound } kind{Kind::Box};
        glm::vec3 scale{1.0f};   // the world scale the shapes were baked with
        bool hasParts{false};
    };

    entt::registry& reg;
    AssetResolver& assets;
    std::unique_ptr<pb::IPhysicsBackend> backend;
    ankerl::unordered_dense::map<entt::entity, BodyRecord> bodies;
    ankerl::unordered_dense::map<pb::BodyHandle, entt::entity> entityOf;
    ankerl::unordered_dense::map<entt::entity, entt::entity> partOf;   // part -> the body it was built into
    std::vector<entt::entity> touched;
    std::vector<PhysicsWarning> warnings;
    std::set<std::pair<entt::entity, std::string>> warned;
    double buildMs{0.0};
    std::size_t builds{0};
    bool removing{false};

    Impl(entt::registry& r, AssetResolver& a) : reg(r), assets(a) {
        connect<Collider>();
        connect<RigidBody>();
        connect<ColliderPart>();
        connect<CollisionMesh>();
        connect<MeshSource>();
        reg.on_destroy<PhysicsBody>().connect<&Impl::onBodyGone>(this);
        for (const entt::entity e : reg.view<Collider>()) touched.push_back(e);
        for (const entt::entity e : reg.view<ColliderPart>()) touched.push_back(e);
    }

    ~Impl() {
        disconnect<Collider>();
        disconnect<RigidBody>();
        disconnect<ColliderPart>();
        disconnect<CollisionMesh>();
        disconnect<MeshSource>();
        reg.on_destroy<PhysicsBody>().disconnect(this);
    }

    template <class T>
    void connect() {
        reg.on_construct<T>().template connect<&Impl::touch>(this);
        reg.on_update<T>().template connect<&Impl::touch>(this);
        reg.on_destroy<T>().template connect<&Impl::touch>(this);
    }
    template <class T>
    void disconnect() {
        reg.on_construct<T>().disconnect(this);
        reg.on_update<T>().disconnect(this);
        reg.on_destroy<T>().disconnect(this);
    }
    // Only queued: the entity may be in the middle of being destroyed.
    void touch(entt::registry&, entt::entity e) { touched.push_back(e); }
    // A body entity destroyed (or its PhysicsBody removed by hand); not the scene's own removal.
    void onBodyGone(entt::registry&, entt::entity e) {
        if (!removing) touched.push_back(e);
    }

    pb::IPhysicsBackend& ensureBackend() {
        if (!backend) {
            // Single-threaded: a level's bodies build and step the same way on every machine,
            // and no worker threads start for a world that only ray-casts.
            physics::PhysicsWorldConfig config;
            config.enableMultithreading = false;
            config.maxBodies = 65536;
            backend = pb::createPhysicsBackend("jolt");
            if (!backend->initialize(config)) {
                OHAO_LOG(Error, physics, "physics: the Jolt backend did not start; levels will not collide");
                backend = std::make_unique<pb::NullPhysicsBackend>();
                (void)backend->initialize(config);
            }
        }
        return *backend;
    }

    void warn(entt::entity e, std::string code, std::string message) {
        if (!warned.emplace(e, code).second) return;
        const auto* name = reg.try_get<Name>(e);
        OHAO_LOG(Warn, physics, "physics: '{}': {}: {}", name ? name->value : std::string("?"), code, message);
        warnings.push_back({std::move(code), e, std::move(message)});
    }

    [[nodiscard]] uint64_t seqOf(entt::entity e) const {
        const auto* co = reg.valid(e) ? reg.try_get<CreationOrder>(e) : nullptr;
        return co ? co->seq : UINT64_MAX;
    }

    [[nodiscard]] bool ownCollider(entt::entity e) const {
        const auto* c = reg.try_get<Collider>(e);
        return c && c->shape != ColliderShape::None && !reg.all_of<ColliderPart>(e);
    }

    // The mesh a collider is fitted to: a CollisionMesh, else the entity's MeshSource.
    [[nodiscard]] std::shared_ptr<const Model> meshOf(entt::entity e) {
        const MeshSource* source = nullptr;
        MeshSource fromCollision;
        if (const auto* cm = reg.try_get<CollisionMesh>(e)) {
            fromCollision.source = cm->source;
            source = &fromCollision;
        } else {
            source = reg.try_get<MeshSource>(e);
        }
        if (!source) return nullptr;
        if (const auto* asset = std::get_if<ModelAsset>(&source->source)) {
            auto loaded = assets.loadModel(asset->asset, asset->mesh);
            if (!loaded) {
                warn(e, "W_COLLIDER_NO_MESH", "its mesh did not load (" + loaded.error().message + "); no collider");
                return nullptr;
            }
            return std::move(loaded.value());
        }
        return primitiveModel(*source);
    }

    // One collider as a backend shape in the body's frame (scale baked), or nullopt.
    std::optional<pb::ShapeInfo> shapeOf(entt::entity e, const Collider& c, glm::vec3 scale, Motion motion,
                                         std::vector<std::shared_ptr<FittedCollider>>& keep, BodyRecord::Kind& kind) {
        ColliderShape shape = c.shape;
        if (shape == ColliderShape::Mesh && motion == Motion::Dynamic) {
            warn(e, "W_LEVEL_DYNAMIC_MESH", "a dynamic body cannot have a triangle-mesh collider; it is built as the mesh's convex hull");
            shape = ColliderShape::Convex;
        }
        auto fitted = std::make_shared<FittedCollider>();
        const bool explicitSize = c.size != glm::vec3(0.0f) &&
                                  (shape == ColliderShape::Box || shape == ColliderShape::Sphere || shape == ColliderShape::Capsule);
        if (explicitSize) {
            fitted->shape = shape;
            fitted->offset = c.offset * scale;
            fitted->halfExtents = c.size * glm::abs(scale);
            fitted->radius = c.size.x * maxAbs(scale);
            fitted->halfHeight = c.size.y * maxAbs(scale);
            fitted->roundedScale = shape != ColliderShape::Box && nonUniform(scale);
        } else {
            const auto model = meshOf(e);
            if (!model || model->vertices.empty()) {
                warn(e, "W_COLLIDER_NO_MESH", "its collider has no size and the entity no mesh to fit it to; no collider");
                return std::nullopt;
            }
            MeshData data;
            data.positions.reserve(model->vertices.size());
            for (const auto& v : model->vertices) data.positions.push_back(v.position);
            if (shape == ColliderShape::Mesh) data.indices = model->indices;
            *fitted = fitCollider(data, shape, scale);
        }
        if (fitted->roundedScale)
            warn(e, "W_COLLIDER_SCALE", "a sphere or capsule cannot stretch unevenly; it takes the largest scale axis");

        pb::ShapeInfo s;
        bool placed = false;   // the shape sits away from the body's origin (or turned)
        switch (shape) {
            case ColliderShape::Box:
                s.type = pb::ShapeInfo::Type::BOX;
                s.halfExtents = glm::max(fitted->halfExtents, glm::vec3(0.001f));
                kind = BodyRecord::Kind::Box;
                placed = fitted->offset != glm::vec3(0.0f);
                break;
            case ColliderShape::Sphere:
                s.type = pb::ShapeInfo::Type::SPHERE;
                s.radius = std::max(fitted->radius, 0.001f);
                kind = BodyRecord::Kind::Sphere;
                placed = fitted->offset != glm::vec3(0.0f);
                break;
            case ColliderShape::Capsule:
                s.type = pb::ShapeInfo::Type::CAPSULE;
                s.radius = std::max(fitted->radius, 0.001f);
                s.height = 2.0f * (fitted->halfHeight + s.radius);   // the backend's height is end to end
                kind = BodyRecord::Kind::Capsule;
                placed = fitted->offset != glm::vec3(0.0f) || fitted->rotation != glm::quat(1, 0, 0, 0);
                break;
            case ColliderShape::Convex:
                s.type = pb::ShapeInfo::Type::CONVEX_HULL;
                s.meshVertices = fitted->vertices.data();
                s.meshVertexCount = static_cast<uint32_t>(fitted->vertices.size());
                kind = BodyRecord::Kind::Convex;
                break;
            case ColliderShape::Mesh:
                s.setMesh(fitted->vertices, fitted->indices);
                kind = BodyRecord::Kind::Mesh;
                break;
            case ColliderShape::None:
                return std::nullopt;
        }
        keep.push_back(fitted);
        if (!placed) return s;
        pb::ShapeInfo wrap;
        wrap.type = pb::ShapeInfo::Type::COMPOUND;
        wrap.children.push_back({std::move(s), fitted->offset, fitted->rotation});
        return wrap;
    }

    void destroyBody(entt::entity e) {
        const auto it = bodies.find(e);
        if (it == bodies.end()) return;
        const pb::BodyHandle h = it->second.handle;
        bodies.erase(it);
        if (backend && h != pb::INVALID_BODY) {
            backend->destroyBody(h);
            entityOf.erase(h);
        }
        if (reg.valid(e) && reg.all_of<PhysicsBody>(e)) {
            removing = true;
            reg.remove<PhysicsBody>(e);
            removing = false;
        }
    }

    void build(entt::entity e, const std::vector<entt::entity>& parts) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto* wt = reg.try_get<WorldTransform>(e);
        const Placement body = decompose(wt ? wt->world : glm::mat4(1.0f));
        const RigidBody rb = reg.all_of<RigidBody>(e) ? reg.get<RigidBody>(e) : RigidBody{};

        std::vector<std::shared_ptr<FittedCollider>> keep;
        std::vector<pb::CompoundChild> children;
        BodyRecord record;
        record.motion = rb.motion;
        record.scale = body.scale;
        record.hasParts = !parts.empty();
        if (ownCollider(e))
            if (auto s = shapeOf(e, reg.get<Collider>(e), body.scale, rb.motion, keep, record.kind))
                children.push_back({std::move(*s), glm::vec3(0.0f), glm::quat(1, 0, 0, 0)});
        const glm::quat inverse = glm::inverse(body.rotation);
        for (const entt::entity p : parts) {
            const auto* c = reg.try_get<Collider>(p);
            const auto* pw = reg.try_get<WorldTransform>(p);
            if (!c || c->shape == ColliderShape::None || !pw) continue;
            const Placement part = decompose(pw->world);
            BodyRecord::Kind ignored{};
            if (auto s = shapeOf(p, *c, part.scale, rb.motion, keep, ignored))
                children.push_back({std::move(*s), inverse * (part.position - body.position), inverse * part.rotation});
            partOf[p] = e;
        }
        if (children.empty()) return;

        pb::BodyCreationInfo info;
        info.position = body.position;
        info.rotation = body.rotation;
        info.motionType = toBackend(rb.motion);
        info.mass = rb.mass;
        info.friction = rb.friction;
        info.restitution = rb.restitution;
        if (parts.empty() && children.size() == 1 && children[0].position == glm::vec3(0.0f) &&
            children[0].rotation == glm::quat(1, 0, 0, 0)) {
            info.shape = std::move(children[0].shape);
        } else {
            info.shape.type = pb::ShapeInfo::Type::COMPOUND;
            info.shape.children = std::move(children);
        }
        if (!parts.empty()) record.kind = BodyRecord::Kind::Compound;

        pb::IPhysicsBackend& be = ensureBackend();
        record.handle = be.createBody(info);
        ++builds;
        if (record.handle != pb::INVALID_BODY) {
            entityOf[record.handle] = e;
            bodies[e] = record;
            reg.emplace_or_replace<PhysicsBody>(e, PhysicsBody{record.handle});
        }
        buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void sync() {
        // 1. Bodies to rebuild: the touched entities' own, and the bodies their parts were or
        // are in; moved parts and moved bodies whose shapes depend on where things are.
        std::vector<entt::entity> rebuild;
        const auto addBodyOf = [&](entt::entity e) {
            rebuild.push_back(e);
            if (const auto it = partOf.find(e); it != partOf.end()) rebuild.push_back(it->second);
            if (reg.valid(e))
                if (const auto* p = reg.try_get<ColliderPart>(e)) rebuild.push_back(p->body);
        };
        for (const entt::entity e : touched) addBodyOf(e);
        touched.clear();
        std::vector<entt::entity> moved;
        for (const entt::entity e : reg.view<PhysicsMoved>()) {
            if (reg.all_of<ColliderPart>(e)) {
                addBodyOf(e);
                continue;
            }
            const auto it = bodies.find(e);
            if (it == bodies.end()) continue;
            const auto& wt = reg.get<WorldTransform>(e);
            if (it->second.hasParts || differs(decompose(wt.world).scale, it->second.scale, 1e-6f)) rebuild.push_back(e);
            else moved.push_back(e);
        }
        reg.clear<PhysicsMoved>();
        if (rebuild.empty() && moved.empty()) return;

        // 2. Rebuild, in creation order (bodies build in the same order on every run).
        std::sort(rebuild.begin(), rebuild.end(), [&](entt::entity a, entt::entity b) {
            const uint64_t sa = seqOf(a), sb = seqOf(b);
            return sa != sb ? sa < sb : entt::to_integral(a) < entt::to_integral(b);
        });
        rebuild.erase(std::unique(rebuild.begin(), rebuild.end()), rebuild.end());
        ankerl::unordered_dense::map<entt::entity, std::vector<entt::entity>> partsOf;
        if (!rebuild.empty()) {
            std::vector<std::pair<uint64_t, entt::entity>> ordered;
            for (auto [p, part] : reg.view<const ColliderPart>().each())
                if (reg.valid(part.body)) ordered.emplace_back(seqOf(p), p);
            std::sort(ordered.begin(), ordered.end());
            for (const auto& [seq, p] : ordered) partsOf[reg.get<ColliderPart>(p).body].push_back(p);
        }
        static const std::vector<entt::entity> kNone;
        for (const entt::entity e : rebuild) {
            if (e == entt::null) continue;
            destroyBody(e);
            for (auto it = partOf.begin(); it != partOf.end();)
                it = it->second == e ? partOf.erase(it) : std::next(it);
            if (!reg.valid(e) || reg.all_of<ColliderPart>(e)) continue;
            const auto parts = partsOf.find(e);
            const auto& list = parts == partsOf.end() ? kNone : parts->second;
            if (ownCollider(e) || !list.empty()) build(e, list);
        }

        // 3. Moved bodies follow their entities.
        for (const entt::entity e : moved) {
            const auto it = bodies.find(e);
            if (it == bodies.end() || !backend) continue;
            const Placement p = decompose(reg.get<WorldTransform>(e).world);
            backend->setPosition(it->second.handle, p.position);
            backend->setRotation(it->second.handle, p.rotation);
        }
    }
};

PhysicsScene::PhysicsScene(entt::registry& reg, AssetResolver& assets) : m(std::make_unique<Impl>(reg, assets)) {}
PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::sync(entt::registry&) { m->sync(); }

std::optional<RayHit> PhysicsScene::raycast(glm::vec3 from, glm::vec3 dir, float max) const {
    if (!m->backend || glm::dot(dir, dir) == 0.0f || !(max > 0.0f)) return std::nullopt;
    const glm::vec3 d = glm::normalize(dir);
    pb::RaycastHit hit;
    if (!m->backend->castRay(from, d, max, hit)) return std::nullopt;
    const auto it = m->entityOf.find(hit.bodyHandle);
    RayHit out;
    out.entity = it == m->entityOf.end() ? entt::entity{entt::null} : it->second;
    out.point = hit.position;
    out.normal = hit.normal;
    out.dist = hit.fraction * max;
    return out;
}

PhysicsInfo PhysicsScene::info() const {
    PhysicsInfo out;
    using Kind = Impl::BodyRecord::Kind;
    for (const auto& [e, r] : m->bodies) {
        switch (r.motion) {
            case Motion::Static: ++out.bodies.statics; break;
            case Motion::Dynamic: ++out.bodies.dynamics; break;
            case Motion::Kinematic: ++out.bodies.kinematics; break;
        }
        switch (r.kind) {
            case Kind::Box: ++out.shapes.box; break;
            case Kind::Sphere: ++out.shapes.sphere; break;
            case Kind::Capsule: ++out.shapes.capsule; break;
            case Kind::Convex: ++out.shapes.convex; break;
            case Kind::Mesh: ++out.shapes.mesh; break;
            case Kind::Compound: ++out.shapes.compound; break;
        }
    }
    out.buildMs = m->buildMs;
    out.unstepped = out.bodies.dynamics;
    return out;
}

physics::backend::IPhysicsBackend& PhysicsScene::backend() { return m->ensureBackend(); }

physics::backend::BodyHandle PhysicsScene::bodyOf(entt::entity e) const {
    const auto it = m->bodies.find(e);
    return it == m->bodies.end() ? pb::INVALID_BODY : it->second.handle;
}

const std::vector<PhysicsWarning>& PhysicsScene::warnings() const noexcept { return m->warnings; }

std::size_t PhysicsScene::bodyBuilds() const noexcept { return m->builds; }

PhysicsScene& installPhysics(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<PhysicsInstalled>()) return *installed->scene;
    // The system owns the scene: it goes with the World's systems, before the registry.
    auto scene = std::make_shared<PhysicsScene>(reg, assets);
    reg.ctx().emplace<PhysicsInstalled>(PhysicsInstalled{scene.get()});
    world.addSyncAfter("transform", "physics", [scene](World& w) { scene->sync(w.registry()); });
    return *scene;
}

PhysicsScene* physicsScene(World& world) {
    const auto* installed = world.registry().ctx().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

const PhysicsScene* physicsScene(const World& world) {
    const auto* installed = world.registry().ctx().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

} // namespace ohao::ecs
