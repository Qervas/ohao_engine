#include "ecs/physics_scene.hpp"

#include "core/log.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/transform_system.hpp"
#include "physics/world/physics_world.hpp"
#include "scene/asset/model.hpp"

#include <ankerl/unordered_dense.h>
#include <glm/gtc/matrix_transform.hpp>

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

// The same rotation within eps per component (q and -q are the same rotation).
bool sameRotation(glm::quat a, glm::quat b, float eps) {
    if (glm::dot(a, b) < 0.0f) b = -b;
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps &&
           std::fabs(a.w - b.w) <= eps;
}

uint64_t pairKey(pb::BodyHandle x, pb::BodyHandle y) {
    const pb::BodyHandle lo = std::min(x, y), hi = std::max(x, y);
    return (uint64_t(lo) << 32) | hi;
}

// Begins and enters sort before ends and exits within a step; a player's footsteps and landings
// after both.
int kindRank(PhysicsEvent::Kind k) {
    switch (k) {
        case PhysicsEvent::Kind::ContactBegin:
        case PhysicsEvent::Kind::TriggerEnter: return 0;
        case PhysicsEvent::Kind::ContactEnd:
        case PhysicsEvent::Kind::TriggerExit: return 1;
        case PhysicsEvent::Kind::Footstep:
        case PhysicsEvent::Kind::Land: return 2;
    }
    return 3;
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
        bool trigger{false};     // a sensor
        // Dynamic bodies: the world pose last written to the entity (or the body was placed at),
        // so physics' own write-back is told apart from a teleport; awake after the last step.
        glm::vec3 wpos{0.0f};
        glm::quat wrot{1, 0, 0, 0};
        bool awake{true};
    };
    // A body pair in contact (or a body inside a trigger): its sub-shape contacts, who is a and b.
    struct Pair {
        uint32_t count{0};
        bool trigger{false};
        // Jolt removes a sleeping body's contacts (and adds them again when it wakes): such a
        // pair stays, dormant, until its bodies wake apart.
        bool dormant{false};
        entt::entity a{entt::null}, b{entt::null};
        uint64_t aId{0}, bId{0};
        glm::vec3 point{0.0f}, normal{0.0f};
        uint64_t since{0};   // the step it began in
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
    // Events (spec "Events").
    ankerl::unordered_dense::map<uint64_t, Pair> pairs;
    std::vector<PhysicsEvent> pending;      // ends from bodies removed since the last step
    std::vector<PhysicsEvent> stepEvents;   // the latest step's, published
    std::vector<PhysicsEvent> log;          // a ring of kEventLog, by seq
    std::size_t dormant{0};                 // pairs kept through their bodies' sleep
    uint64_t nextSeq{1};
    uint64_t steps{0};
    int lastSubsteps{0};
    bool warnedLongStep{false};
    PhysicsScene::StepTiming timing;
    float stepDt{0.0f};                            // the step's dt while it runs; 0 outside a step
    std::vector<pb::BodyHandle> kinematicMoved;    // moved by velocity this step; stopped after it
    // BodyMotion written by anything but this scene (a snapshot's restore): bodies take theirs in
    // the next sync, players in their next step (after its teleport).
    std::vector<entt::entity> motionTouched;
    ankerl::unordered_dense::set<entt::entity> playerMotion;
    bool writingMotion{false};
    // Transforms written by anything but this scene since the last step (an edit, a restore):
    // such a body or player is placed there however close it is to where the scene last put it.
    // Moved through a parent, a pose further than 1e-4 from the scene's own counts too.
    ankerl::unordered_dense::set<entt::entity> posedExternally;
    bool writingPose{false};

    // The simulation a snapshot carries (SnapshotHook): the backend's state (bodies, contact
    // cache, characters), which body is whose, the players and the contact bookkeeping. A restore
    // hands it back; the next sync puts it in place if the bodies are the same ones, else the
    // components' path (teleports and BodyMotion) does what it can.
    struct SavedSimulation;
    std::shared_ptr<const SavedSimulation> savedPending;
    // A restore since the players last spawned: a PlayerController it put back (a dead enemy's,
    // combat's round restart) gets its character where the restored entity stands, not at a spawn.
    bool restoredSinceSpawn{false};

    // A player (spec "The player"): its character, and what the step last made of it.
    struct PlayerRecord {
        pb::CharacterHandle character{pb::INVALID_CHARACTER};
        pb::BodyHandle inner{pb::INVALID_BODY};
        glm::vec3 written{0.0f};                // the Transform position last written
        glm::quat writtenRot{1, 0, 0, 0};
        glm::vec3 horizontal{0.0f};             // m/s
        float vy{0.0f};                         // m/s, the vertical velocity at the step's end
        float yawDeg{0.0f}, pitchDeg{0.0f};
        // The head (spec "The camera"): the child holding the Camera, and its local pose last written.
        entt::entity head{entt::null};
        glm::vec3 headPos{0.0f};
        glm::quat headRot{1, 0, 0, 0};
        // Footsteps (spec "Footsteps"): ground walked since the last footstep, the highest point
        // since it last stood on the ground, and whether it did at the last step's end.
        float stride{0.0f};
        float airTop{0.0f};
        bool grounded{false};
        entt::entity ground{entt::null};   // the body it stood on at the last step's end
        uint64_t footsteps{0};
        // Made or teleported, not stepped since: its ground is found again when bodies are built
        // (a play scene spawns its player before the level's bodies exist).
        bool placed{true};
    };
    static constexpr float kWalkStride = 0.75f;     // m between footsteps walking
    static constexpr float kSprintStride = 0.95f;   // and sprinting
    static constexpr float kLandDrop = 0.3f;        // m fallen before touching the ground is a land
    ankerl::unordered_dense::map<entt::entity, PlayerRecord> players;
    std::vector<entt::entity> playersTouched;  // PlayerController added or changed
    // Characters whose PlayerController went (a dead enemy's), out of the world but kept with the
    // controller they were made for: a restore that puts the controller back gets the same
    // character, so the snapshot's simulation restores exactly. Destroyed with their entity.
    struct Parked {
        PlayerRecord record;
        PlayerController made;
    };
    ankerl::unordered_dense::map<entt::entity, Parked> parked;

    // Render interpolation (spec "Interpolation"): what the last step moved, from where to where
    // (world poses, without scale). An entry the next step does not move is dropped after it.
    struct Mover {
        glm::vec3 fromPos{0.0f}, toPos{0.0f};
        glm::quat fromRot{1, 0, 0, 0}, toRot{1, 0, 0, 0};
        bool moved{false};
    };
    ankerl::unordered_dense::map<entt::entity, Mover> movers;
    ankerl::unordered_dense::set<entt::entity> drawn;   // given a RenderPose by the last interpolate

    Impl(entt::registry& r, AssetResolver& a) : reg(r), assets(a) {
        connect<Collider>();
        connect<RigidBody>();
        connect<ColliderPart>();
        connect<CollisionMesh>();
        connect<MeshSource>();
        reg.on_destroy<PhysicsBody>().connect<&Impl::onBodyGone>(this);
        reg.on_construct<PlayerController>().connect<&Impl::touchPlayer>(this);
        reg.on_update<PlayerController>().connect<&Impl::touchPlayer>(this);
        reg.on_destroy<PlayerController>().connect<&Impl::onPlayerGone>(this);
        reg.on_construct<BodyMotion>().connect<&Impl::touchMotion>(this);
        reg.on_update<BodyMotion>().connect<&Impl::touchMotion>(this);
        reg.on_update<Transform>().connect<&Impl::touchPose>(this);
        for (const entt::entity e : reg.view<Collider>()) touched.push_back(e);
        for (const entt::entity e : reg.view<ColliderPart>()) touched.push_back(e);
        for (const entt::entity e : reg.view<PlayerController>()) playersTouched.push_back(e);
    }

    ~Impl() {
        disconnect<Collider>();
        disconnect<RigidBody>();
        disconnect<ColliderPart>();
        disconnect<CollisionMesh>();
        disconnect<MeshSource>();
        reg.on_destroy<PhysicsBody>().disconnect(this);
        reg.on_construct<PlayerController>().disconnect(this);
        reg.on_update<PlayerController>().disconnect(this);
        reg.on_destroy<PlayerController>().disconnect(this);
        reg.on_construct<BodyMotion>().disconnect(this);
        reg.on_update<BodyMotion>().disconnect(this);
        reg.on_update<Transform>().disconnect(this);
        if (backend) {
            for (auto& [e, p] : players) backend->destroyCharacter(p.character);
            for (auto& [e, k] : parked) backend->destroyCharacter(k.record.character);
        }
    }

    void touchPlayer(entt::registry&, entt::entity e) { playersTouched.push_back(e); }
    void touchMotion(entt::registry&, entt::entity e) {
        if (!writingMotion) motionTouched.push_back(e);
    }
    void touchPose(entt::registry&, entt::entity e) {
        if (!writingPose) posedExternally.insert(e);
    }
    // This scene's own Transform write (the write-back, a player's step).
    template <class F>
    void writePose(entt::entity e, F&& f) {
        writingPose = true;
        reg.patch<Transform>(e, std::forward<F>(f));
        writingPose = false;
    }
    // This scene's own BodyMotion write (not a restore's), only when it changes.
    void writeMotion(entt::entity e, const BodyMotion& m) {
        if (const auto* cur = reg.try_get<BodyMotion>(e);
            cur && cur->linear == m.linear && cur->angular == m.angular && cur->asleep == m.asleep)
            return;
        writingMotion = true;
        reg.emplace_or_replace<BodyMotion>(e, m);
        writingMotion = false;
    }
    // A restored body's motion: its velocities, awake, or asleep at rest (after the restore's
    // teleport, which left it at rest).
    void applyMotion(entt::entity e) {
        const auto it = bodies.find(e);
        if (it == bodies.end() || it->second.motion != Motion::Dynamic || !backend || !reg.valid(e)) return;
        const auto* m = reg.try_get<BodyMotion>(e);
        if (!m) return;
        BodyRecord& r = it->second;
        if (m->asleep) {
            backend->setLinearVelocity(r.handle, glm::vec3(0.0f));
            backend->setAngularVelocity(r.handle, glm::vec3(0.0f));
            backend->setAwake(r.handle, false);
        } else {
            backend->setAwake(r.handle, true);
            backend->setLinearVelocity(r.handle, m->linear);
            backend->setAngularVelocity(r.handle, m->angular);
        }
        r.awake = !m->asleep;
    }
    // A player's character leaves the world with its PlayerController (parked; destroyed with its
    // entity); its trigger pairs end.
    void onPlayerGone(entt::registry&, entt::entity e) { removePlayer(e, true); }
    void removePlayer(entt::entity e, bool park) {
        const auto it = players.find(e);
        if (it == players.end()) return;
        if (it->second.inner != pb::INVALID_BODY) {
            if (!pairs.empty()) endPairsOf(it->second.inner);
            entityOf.erase(it->second.inner);
        }
        if (backend) {
            if (park && reg.all_of<PlayerController>(e)) {
                backend->setCharacterInWorld(it->second.character, false);
                dropParked(e);
                parked[e] = Parked{it->second, reg.get<PlayerController>(e)};
            } else {
                backend->destroyCharacter(it->second.character);
            }
        }
        players.erase(it);
    }
    void dropParked(entt::entity e) {
        const auto it = parked.find(e);
        if (it == parked.end()) return;
        if (backend) backend->destroyCharacter(it->second.record.character);
        parked.erase(it);
    }
    // A parked character for a controller like the one it was made for (a restore's), back in the
    // world; else the parked one goes and false.
    bool unpark(entt::entity e, const PlayerController& pc, PlayerRecord& out) {
        const auto it = parked.find(e);
        if (it == parked.end()) return false;
        const PlayerController& m = it->second.made;
        const bool same = m.radius == pc.radius && m.height == pc.height && m.maxSlopeDeg == pc.maxSlopeDeg &&
                          m.mass == pc.mass && m.pushForce == pc.pushForce;
        if (!same || !backend) {
            dropParked(e);
            return false;
        }
        out = it->second.record;
        parked.erase(it);
        backend->setCharacterInWorld(out.character, true);
        return true;
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

    // A removed body's pairs end now (Jolt's later OnContactRemoved for it no longer maps).
    void endPairsOf(pb::BodyHandle h) {
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (pb::BodyHandle(it->first >> 32) != h && pb::BodyHandle(it->first & 0xffffffffu) != h) {
                ++it;
                continue;
            }
            if (it->second.dormant) --dormant;
            pending.push_back(endEvent(it->second));
            it = pairs.erase(it);
        }
    }

    [[nodiscard]] PhysicsEvent endEvent(const Pair& p) const {
        PhysicsEvent ev;
        ev.kind = p.trigger ? PhysicsEvent::Kind::TriggerExit : PhysicsEvent::Kind::ContactEnd;
        ev.a = p.a != entt::null && reg.valid(p.a) ? p.a : entt::entity{entt::null};
        ev.b = p.b != entt::null && reg.valid(p.b) ? p.b : entt::entity{entt::null};
        ev.aId = p.aId;
        ev.bId = p.bId;
        ev.point = p.point;
        ev.normal = p.normal;
        return ev;
    }

    void destroyBody(entt::entity e) {
        const auto it = bodies.find(e);
        if (it == bodies.end()) return;
        const pb::BodyHandle h = it->second.handle;
        bodies.erase(it);
        if (h != pb::INVALID_BODY && !pairs.empty()) endPairsOf(h);
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
        record.trigger = ownCollider(e) && reg.get<Collider>(e).trigger;
        record.wpos = body.position;
        record.wrot = body.rotation;
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
        info.isSensor = record.trigger;
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

    void syncBodies() {
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
        if (rebuild.empty() && moved.empty() && !savedPending) return;

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

        // A snapshot's simulation, restored: every body as it was, so a body whose entity the
        // restore put back where its body now is stays (no teleport, which would drop the contact
        // cache); one the snapshot had already moved without a step goes there below.
        const bool restored = savedPending && restoreSaved();

        // 3. Moved bodies follow their entities. A dynamic body's entity moved by the write-back
        // (its world pose is the one written) stays; moved by anything else, the body jumps
        // there, at rest.
        for (const entt::entity e : moved) {
            const auto it = bodies.find(e);
            if (it == bodies.end() || !backend) continue;
            BodyRecord& r = it->second;
            const Placement p = decompose(reg.get<WorldTransform>(e).world);
            if (restored && !differs(p.position, r.wpos, 1e-4f) && sameRotation(p.rotation, r.wrot, 1e-4f)) {
                posedExternally.erase(e);
                continue;
            }
            if (r.motion == Motion::Dynamic) {
                if (!posedExternally.erase(e) && !differs(p.position, r.wpos, 1e-4f) && sameRotation(p.rotation, r.wrot, 1e-4f))
                    continue;
                backend->setPose(r.handle, p.position, p.rotation);
                backend->setLinearVelocity(r.handle, glm::vec3(0.0f));
                backend->setAngularVelocity(r.handle, glm::vec3(0.0f));
                backend->setAwake(r.handle, true);
                r.wpos = p.position;
                r.wrot = p.rotation;
                r.awake = true;
                continue;
            }
            if (r.motion == Motion::Kinematic && stepDt > 0.0f) {
                // Inside a step: by velocity, so it pushes what it meets; stopped after the step.
                backend->moveKinematic(r.handle, p.position, p.rotation, stepDt);
                kinematicMoved.push_back(r.handle);
                continue;
            }
            backend->setPose(r.handle, p.position, p.rotation);
        }
    }

    // ---- snapshots (SnapshotHook) ----------------------------------------------------------------

    struct SavedPlayer {
        uint64_t id{0}, headId{0}, groundId{0};
        PlayerRecord record;
    };

    // Every body (players' inner bodies too) and its entity's id, by handle.
    [[nodiscard]] std::vector<std::pair<pb::BodyHandle, uint64_t>> bodyIdentity() const {
        std::vector<std::pair<pb::BodyHandle, uint64_t>> out;
        out.reserve(entityOf.size());
        for (const auto& [h, e] : entityOf) out.emplace_back(h, idOf(e));
        std::sort(out.begin(), out.end());
        return out;
    }

    [[nodiscard]] std::shared_ptr<const void> saveSimulation() const;
    void restoreSimulation(const std::shared_ptr<const void>& state) {
        savedPending = std::static_pointer_cast<const SavedSimulation>(state);
        restoredSinceSpawn = true;
    }
    bool restoreSaved();

    // Bodies up to date, then the motion a restore wrote: into the bodies (after their
    // teleports); players take theirs in their step, after its own teleport.
    void sync() {
        const std::size_t built = builds;
        syncBodies();
        if (builds != built && backend)
            for (auto& [e, p] : players) {
                if (!p.placed || !reg.valid(e)) continue;
                backend->setCharacterPosition(p.character, p.written);   // its contacts found again
                standWhereItIs(p);
                if (auto* st = reg.try_get<PlayerState>(e)) st->onGround = p.grounded;
            }
        if (motionTouched.empty()) return;
        std::vector<entt::entity> list;
        list.swap(motionTouched);
        for (const entt::entity e : list) {
            if (players.contains(e)) playerMotion.insert(e);
            else applyMotion(e);
        }
    }

    // ---- the step (sub-project 6) ---------------------------------------------------------------

    // The step's contact and trigger events, from the backend's queue (filled by Jolt's callbacks
    // during the substeps, on whatever thread; drained here on the simulating one).
    void drainContacts(std::vector<PhysicsEvent>& out) {
        for (const pb::ContactEvent& ce : backend->getContactEvents()) {
            if (ce.type == pb::ContactEvent::Type::PERSIST) continue;
            if (ce.body1 == pb::INVALID_BODY || ce.body2 == pb::INVALID_BODY) continue;   // a removed body's
            const uint64_t key = pairKey(ce.body1, ce.body2);
            if (ce.type == pb::ContactEvent::Type::END) {
                const auto it = pairs.find(key);
                if (it == pairs.end() || it->second.count == 0) continue;   // ended when its body was removed
                if (--it->second.count == 0) {
                    if (asleep(key)) {
                        it->second.dormant = true;   // removed because its bodies went to sleep
                        ++dormant;
                    } else {
                        out.push_back(endEvent(it->second));
                        pairs.erase(it);
                    }
                }
                continue;
            }
            const auto e1 = entityOf.find(ce.body1), e2 = entityOf.find(ce.body2);
            if (e1 == entityOf.end() || e2 == entityOf.end()) continue;
            Pair& p = pairs[key];
            if (p.count++ > 0) continue;
            if (p.dormant) {   // woken still touching: the same contact
                p.dormant = false;
                --dormant;
                continue;
            }
            const auto r1 = bodies.find(e1->second), r2 = bodies.find(e2->second);
            const bool t1 = r1 != bodies.end() && r1->second.trigger, t2 = r2 != bodies.end() && r2->second.trigger;
            p.trigger = t1 || t2;
            const uint64_t id1 = idOf(e1->second), id2 = idOf(e2->second);
            // Contacts: a has the lower id; triggers: a is the trigger. Jolt's normal points from
            // body 1 toward body 2; ours from a toward b.
            const bool swap = p.trigger ? (!t1 && t2) : id2 < id1;
            p.a = swap ? e2->second : e1->second;
            p.b = swap ? e1->second : e2->second;
            p.aId = swap ? id2 : id1;
            p.bId = swap ? id1 : id2;
            p.point = ce.contactPoint;
            p.since = steps;
            const float n = glm::length(ce.contactNormal);
            p.normal = n > 0.0f ? (swap ? -ce.contactNormal : ce.contactNormal) / n : glm::vec3(0.0f);
            PhysicsEvent ev;
            ev.kind = p.trigger ? PhysicsEvent::Kind::TriggerEnter : PhysicsEvent::Kind::ContactBegin;
            ev.a = p.a;
            ev.b = p.b;
            ev.aId = p.aId;
            ev.bId = p.bId;
            ev.point = p.point;
            ev.normal = p.normal;
            ev.speed = std::fabs(glm::dot(ce.relativeVelocity, p.normal));
            out.push_back(ev);
        }
        // Dormant pairs whose bodies woke and were not added again have come apart.
        if (dormant == 0) return;
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (!it->second.dormant || asleep(it->first)) {
                ++it;
                continue;
            }
            --dormant;
            out.push_back(endEvent(it->second));
            it = pairs.erase(it);
        }
    }

    // Every moving body of the pair asleep (a static one never wakes).
    [[nodiscard]] bool asleep(uint64_t key) const {
        for (const pb::BodyHandle h : {pb::BodyHandle(key >> 32), pb::BodyHandle(key & 0xffffffffu)}) {
            const auto e = entityOf.find(h);
            if (e == entityOf.end()) continue;
            if (players.contains(e->second)) return false;   // a character's inner body never sleeps
            const auto r = bodies.find(e->second);
            if (r == bodies.end() || r->second.motion == Motion::Static) continue;
            if (backend->isAwake(h)) return false;
        }
        return true;
    }

    [[nodiscard]] uint64_t idOf(entt::entity e) const {
        const auto* id = reg.valid(e) ? reg.try_get<EntityId>(e) : nullptr;
        return id ? id->value : 0;
    }

    // Every dynamic body awake now or at the end of the last step: its pose into its entity's
    // Transform, through its parent's world transform (the local scale is kept), and its motion
    // into BodyMotion (asleep at rest, once, as it falls asleep).
    void writeBack() {
        for (auto& [e, r] : bodies) {
            if (r.motion != Motion::Dynamic) continue;
            const bool awake = backend->isAwake(r.handle);
            if (!awake && !r.awake) continue;
            r.awake = awake;
            writeMotion(e, awake ? BodyMotion{backend->getLinearVelocity(r.handle), backend->getAngularVelocity(r.handle), false}
                                 : BodyMotion{glm::vec3(0.0f), glm::vec3(0.0f), true});
            const glm::vec3 pos = backend->getPosition(r.handle);
            const glm::quat rot = backend->getRotation(r.handle);
            recordMove(e, r.wpos, r.wrot, pos, rot);
            r.wpos = pos;
            r.wrot = rot;
            glm::vec3 localPos = pos;
            glm::quat localRot = rot;
            if (const auto* parent = reg.try_get<Parent>(e); parent && reg.valid(parent->entity))
                if (const auto* pw = reg.try_get<WorldTransform>(parent->entity)) {
                    const Placement pp = decompose(pw->world);
                    if (nonUniform(pp.scale))
                        warn(e, "W_PHYSICS_PARENT_SCALE",
                             "a dynamic body under a parent scaled unevenly: its pose is written approximately");
                    localPos = glm::vec3(glm::inverse(pw->world) * glm::vec4(pos, 1.0f));
                    localRot = glm::normalize(glm::inverse(pp.rotation) * rot);
                }
            writePose(e, [&](Transform& t) {
                t.position = localPos;
                t.rotation = localRot;
            });
            ++timing.writes;
        }
    }

    void publish(std::vector<PhysicsEvent>& fresh) {
        std::stable_sort(fresh.begin(), fresh.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
            const int kx = kindRank(x.kind), ky = kindRank(y.kind);
            if (kx != ky) return kx < ky;
            return x.aId != y.aId ? x.aId < y.aId : x.bId < y.bId;
        });
        stepEvents.clear();
        stepEvents.swap(pending);   // removals first: they happened before the step
        stepEvents.insert(stepEvents.end(), fresh.begin(), fresh.end());
        if (log.size() < PhysicsScene::kEventLog) log.resize(PhysicsScene::kEventLog);
        for (PhysicsEvent& ev : stepEvents) {
            ev.step = steps;
            ev.seq = nextSeq++;
            log[(ev.seq - 1) % PhysicsScene::kEventLog] = ev;
        }
    }

    // ---- the player (spec "The player") -----------------------------------------------------------

    static float yawOf(const glm::quat& q) {
        const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, -1.0f);
        if (std::fabs(f.x) < 1e-6f && std::fabs(f.z) < 1e-6f) return 0.0f;
        return glm::degrees(std::atan2(-f.x, -f.z));
    }
    static glm::quat yawQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }
    static glm::quat pitchQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(1.0f, 0.0f, 0.0f)); }
    // Up positive, from the rotation's -Z.
    static float pitchOf(const glm::quat& q) {
        const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, -1.0f);
        return glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
    }

    // An entity's world matrix from the Transforms up its chain: right after a level expands,
    // before the transform system has given its nodes a WorldTransform.
    [[nodiscard]] glm::mat4 worldOf(entt::entity e) const {
        const auto* t = reg.try_get<Transform>(e);
        glm::mat4 m = t ? t->matrix() : glm::mat4(1.0f);
        for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity); p = reg.try_get<Parent>(p->entity))
            if (const auto* pt = reg.try_get<Transform>(p->entity)) m = pt->matrix() * m;
        return m;
    }

    // The player's head: its first child (creation order) holding a Camera, else one named Head.
    [[nodiscard]] entt::entity headOf(entt::entity e) const {
        const auto* ch = reg.try_get<Children>(e);
        if (!ch) return entt::null;
        entt::entity camera = entt::null, named = entt::null;
        uint64_t cameraSeq = UINT64_MAX, namedSeq = UINT64_MAX;
        for (const entt::entity c : ch->entities) {
            if (!reg.valid(c) || !reg.all_of<Transform>(c)) continue;
            const uint64_t s = seqOf(c);
            if (reg.all_of<Camera>(c)) {
                if (s < cameraSeq) cameraSeq = s, camera = c;
            } else if (const auto* n = reg.try_get<Name>(c); n && n->value == "Head" && s < namedSeq) {
                namedSeq = s, named = c;
            }
        }
        return camera != entt::null ? camera : named;
    }

    void recordMove(entt::entity e, glm::vec3 fromPos, glm::quat fromRot, glm::vec3 toPos, glm::quat toRot) {
        Mover& m = movers[e];
        m.fromPos = fromPos;
        m.fromRot = fromRot;
        m.toPos = toPos;
        m.toRot = toRot;
        m.moved = true;
    }

    // The spawn point of a character: the k-th character with its PlayerController.spawn tag (in
    // creation order) takes the k-th SpawnPoint with that tag (in creation order); null if it has
    // no tag or no point is left for it.
    [[nodiscard]] entt::entity spawnFor(entt::entity e) const {
        const std::string& tag = reg.get<PlayerController>(e).spawn;
        if (tag.empty()) return entt::null;
        std::vector<std::pair<uint64_t, entt::entity>> points, characters;
        for (auto [p, sp] : reg.view<const SpawnPoint>().each())
            if (sp.tag == tag && reg.all_of<Transform>(p)) points.emplace_back(seqOf(p), p);
        for (auto [c, pc] : reg.view<const PlayerController>().each())
            if (pc.spawn == tag) characters.emplace_back(seqOf(c), c);
        std::sort(points.begin(), points.end());
        std::sort(characters.begin(), characters.end());
        for (std::size_t k = 0; k < characters.size(); ++k)
            if (characters[k].second == e) return k < points.size() ? points[k].second : entt::entity{entt::null};
        return entt::null;
    }

    // A character for a new PlayerController, at the player spawn (or where the entity is, with
    // W_PLAYER_NO_SPAWN); for a changed one, where the player stands, facing as it faced; for one
    // a restore put back (`restored`), as the restored entity and its head stand.
    void createPlayer(entt::entity e, bool restored) {
        const auto old = players.find(e);
        const bool existed = old != players.end();
        float oldPitch = existed ? old->second.pitchDeg : 0.0f;
        // Its yaw, unless its rotation was edited too.
        const glm::quat rotNow = reg.get<Transform>(e).rotation;   // a copy: writePlayer rewrites it
        const float oldYaw = !existed ? 0.0f
                             : sameRotation(rotNow, old->second.writtenRot, 1e-4f) ? old->second.yawDeg
                                                                                   : yawOf(rotNow);
        removePlayer(e, false);
        const PlayerController& pc = reg.get<PlayerController>(e);
        PlayerRecord back;
        const bool same = !existed && restored && unpark(e, pc, back);
        if (!same) dropParked(e);
        glm::vec3 pos(0.0f);
        float yaw = 0.0f;
        const entt::entity spawn = existed || restored ? entt::null : spawnFor(e);
        if (existed) {
            pos = reg.get<Transform>(e).position;
            yaw = oldYaw;
        } else if (restored) {
            pos = reg.get<Transform>(e).position;   // a character is a root entity: its world pose
            yaw = yawOf(rotNow);
            if (const entt::entity head = headOf(e); head != entt::null)
                oldPitch = std::clamp(pitchOf(reg.get<Transform>(head).rotation), -89.0f, 89.0f);
        } else if (spawn != entt::null) {
            const glm::mat4 w = worldOf(spawn);
            pos = glm::vec3(w[3]);
            yaw = yawOf(decompose(w).rotation);
        } else {
            const Placement p = decompose(worldOf(e));
            pos = p.position;
            yaw = yawOf(p.rotation);
            if (!pc.spawn.empty())
                warn(e, "W_PLAYER_NO_SPAWN",
                     "no SpawnPoint tagged \"" + pc.spawn + "\" is left for it; the character starts where it is");
        }
        pb::CharacterCreationInfo ci;
        ci.position = pos;
        ci.rotation = yawQuat(yaw);
        ci.capsuleRadius = pc.radius;
        ci.capsuleHeight = std::max(pc.height, 2.0f * pc.radius + 0.01f);
        ci.maxSlopeAngleDeg = pc.maxSlopeDeg;
        ci.mass = pc.mass;
        ci.maxStrength = pc.pushForce;
        ci.layer = pb::CollisionLayer::CHARACTER;
        ci.positionAtFeet = true;
        ci.innerBody = true;
        pb::IPhysicsBackend& be = ensureBackend();
        PlayerRecord p;
        if (same) {
            // The character it had (a restore puts the snapshot's simulation back over it), placed
            // where the restored entity stands.
            p.character = back.character;
            p.inner = back.inner;
            be.setCharacterRotation(p.character, ci.rotation);
            be.setCharacterPosition(p.character, pos);
            be.setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
        } else {
            p.character = be.createCharacter(ci);
            if (p.character == pb::INVALID_CHARACTER) {
                warn(e, "W_PLAYER_NO_CHARACTER", "the physics backend could not make the player's character");
                return;
            }
            p.inner = be.getCharacterInnerBody(p.character);
        }
        if (p.inner != pb::INVALID_BODY) entityOf[p.inner] = e;
        p.yawDeg = yaw;
        p.pitchDeg = oldPitch;
        p.written = pos;
        p.writtenRot = ci.rotation;
        p.airTop = pos.y;
        standWhereItIs(p);   // the backend found its ground at creation
        movers.erase(e);     // placed, not moved: nothing to draw between
        const entt::entity headNow = restored ? headOf(e) : entt::entity{entt::null};
        const glm::quat headRotNow = headNow != entt::null ? reg.get<Transform>(headNow).rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        writePlayer(e, p, pos, glm::vec3(0.0f), p.grounded, false);
        if (restored) {
            // A restore's entity keeps the rotation it was restored with, bit for bit: the yaw and
            // pitch read back from it give the same rotation but not always the same quaternion (a
            // yaw of -180 reads back as +180), and world.hash after a restart is the round start's.
            if (sameRotation(rotNow, p.writtenRot, 1e-4f) && rotNow != p.writtenRot) {
                const glm::quat keep = rotNow;
                p.writtenRot = keep;
                writePose(e, [&](Transform& t) { t.rotation = keep; });
            }
            if (headNow != entt::null && headNow == p.head && sameRotation(headRotNow, p.headRot, 1e-4f) &&
                headRotNow != p.headRot) {
                p.headRot = headRotNow;
                writePose(headNow, [&](Transform& t) { t.rotation = headRotNow; });
            }
        }
        players[e] = p;
    }

    // A placed player (made or teleported) on the ground the backend found where it is.
    void standWhereItIs(PlayerRecord& p) {
        const pb::CharacterState st = backend->getCharacterState(p.character);
        p.grounded = st.groundState == pb::GroundState::ON_GROUND;
        p.ground = groundEntity(st);
    }
    [[nodiscard]] entt::entity groundEntity(const pb::CharacterState& st) const {
        if (st.groundState != pb::GroundState::ON_GROUND || st.groundBody == pb::INVALID_BODY) return entt::null;
        const auto it = entityOf.find(st.groundBody);
        return it == entityOf.end() ? entt::entity{entt::null} : it->second;
    }

    bool teleport(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg) {
        const auto it = players.find(e);
        if (it == players.end() || !backend) return false;
        PlayerRecord& p = it->second;
        if (yawDeg) p.yawDeg = std::remainder(*yawDeg, 360.0f);
        backend->setCharacterRotation(p.character, yawQuat(p.yawDeg));
        backend->setCharacterPosition(p.character, pos);   // its contacts found there
        backend->setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
        p.horizontal = glm::vec3(0.0f);
        p.vy = 0.0f;
        p.stride = 0.0f;
        p.airTop = pos.y;
        p.placed = true;
        standWhereItIs(p);
        movers.erase(e);
        if (p.head != entt::null) movers.erase(p.head);
        writePlayer(e, p, pos, glm::vec3(0.0f), p.grounded, false);
        return true;
    }

    // The player's Transform (feet, yaw), its head and PlayerState. `track`: a move of the step,
    // drawn between where it was and where it is (else a placement).
    void writePlayer(entt::entity e, PlayerRecord& p, glm::vec3 pos, glm::vec3 velocity, bool onGround, bool track) {
        const glm::quat rot = yawQuat(p.yawDeg);
        const glm::vec3 fromPos = p.written;
        const glm::quat fromRot = p.writtenRot;
        if (track) recordMove(e, fromPos, fromRot, pos, rot);
        p.written = pos;
        p.writtenRot = rot;
        writePose(e, [&](Transform& t) {
            t.position = pos;
            t.rotation = rot;
        });
        writeHead(e, p, fromPos, fromRot, track);
        reg.emplace_or_replace<PlayerState>(e, PlayerState{velocity, onGround, p.yawDeg, p.pitchDeg, p.character});
        writeMotion(e, BodyMotion{velocity, glm::vec3(0.0f), false});
    }

    // Recoil: the view turned after the step moved the player. A turn inside a step keeps the step's
    // move (drawn from where the step started to the turned pose); outside one it is a placement.
    bool turnPlayer(entt::entity e, float dYaw, float dPitch) {
        const auto it = players.find(e);
        if (it == players.end() || !reg.valid(e)) return false;
        PlayerRecord& p = it->second;
        p.yawDeg = std::remainder(p.yawDeg + dYaw, 360.0f);
        p.pitchDeg = std::clamp(p.pitchDeg + dPitch, -89.0f, 89.0f);
        if (backend) backend->setCharacterRotation(p.character, yawQuat(p.yawDeg));
        const auto mv = movers.find(e);
        const bool moving = mv != movers.end() && mv->second.moved;
        std::optional<Mover> body, head;
        if (moving) {
            body = mv->second;
            if (const auto h = movers.find(p.head); p.head != entt::null && h != movers.end()) head = h->second;
        }
        const PlayerState* st = reg.try_get<PlayerState>(e);
        writePlayer(e, p, p.written, st ? st->velocity : glm::vec3(0.0f), st && st->onGround, moving);
        const auto keepFrom = [&](entt::entity who, const std::optional<Mover>& was) {
            if (!was) return;
            if (const auto m2 = movers.find(who); m2 != movers.end()) {
                m2->second.fromPos = was->fromPos;
                m2->second.fromRot = was->fromRot;
            }
        };
        keepFrom(e, body);
        keepFrom(p.head, head);
        return true;
    }

    // The head at eyeHeight above the feet, pitched; written only when it changes.
    void writeHead(entt::entity e, PlayerRecord& p, glm::vec3 fromPos, glm::quat fromRot, bool track) {
        const entt::entity head = headOf(e);
        if (head == entt::null) {
            p.head = entt::null;
            return;
        }
        const glm::vec3 local(0.0f, reg.get<PlayerController>(e).eyeHeight, 0.0f);
        const glm::quat localRot = pitchQuat(p.pitchDeg);
        if (track && head == p.head) {
            const glm::vec3 scale = reg.get<Transform>(e).scale;
            recordMove(head, fromPos + fromRot * (scale * p.headPos), fromRot * p.headRot, p.written + p.writtenRot * (scale * local),
                       p.writtenRot * localRot);
        } else {
            movers.erase(head);
        }
        p.head = head;
        p.headPos = local;
        p.headRot = localRot;
        const Transform& ht = reg.get<Transform>(head);
        if (ht.position != local || ht.rotation != localRot)
            writePose(head, [&](Transform& t) {
                t.position = local;
                t.rotation = localRot;
            });
    }

    // Characters for PlayerControllers added or changed since: the `player_spawn` sync system
    // (before `transform`, so a play scene's first frame is drawn from the spawn) and each step.
    void spawnPlayers() {
        const bool restored = std::exchange(restoredSinceSpawn, false);
        for (auto it = parked.begin(); it != parked.end();) {   // characters of destroyed entities go
            if (reg.valid(it->first)) {
                ++it;
                continue;
            }
            if (backend) backend->destroyCharacter(it->second.record.character);
            it = parked.erase(it);
        }
        if (playersTouched.empty()) return;
        std::vector<entt::entity> list;
        list.swap(playersTouched);
        for (const entt::entity e : list)
            if (reg.valid(e) && reg.all_of<PlayerController>(e)) createPlayer(e, restored);
    }

    // A player's footstep or landing on the ground body `ground` (spec "Footsteps").
    PhysicsEvent playerEvent(PhysicsEvent::Kind kind, entt::entity e, const pb::CharacterState& at, float speed) const {
        PhysicsEvent ev;
        ev.kind = kind;
        ev.a = e;
        ev.aId = idOf(e);
        if (const auto it = entityOf.find(at.groundBody); at.groundBody != pb::INVALID_BODY && it != entityOf.end()) {
            ev.b = it->second;
            ev.bId = idOf(ev.b);
        }
        const auto* c = ev.b != entt::null && reg.valid(ev.b) ? reg.try_get<Collider>(ev.b) : nullptr;
        ev.surface = c && !c->surface.empty() ? c->surface : std::string("default");
        ev.point = at.position;
        ev.normal = at.groundNormal;
        ev.speed = speed;
        return ev;
    }

    // Every player, once per step, before the bodies step (spec: the character moves first); its
    // footsteps and landing go into `out`.
    void updatePlayers(const InputMap& input, float dt, std::vector<PhysicsEvent>& out) {
        spawnPlayers();
        if (players.empty() || !(dt > 0.0f) || !backend) return;
        const glm::vec3 gravity = backend->getGravity();
        const glm::vec2 mouse = input.mouseDelta();
        for (auto& [e, p] : players) {
            const PlayerController& pc = reg.get<PlayerController>(e);
            const Transform& t = reg.get<Transform>(e);
            // Moved by anything but this system (an edit, a restore): it goes there, at rest (a
            // placement: the step draws from there).
            const bool posed = !posedExternally.empty() && posedExternally.contains(e);
            if (posed || differs(t.position, p.written, 1e-4f)) {
                backend->setCharacterPosition(p.character, t.position);
                backend->setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
                p.horizontal = glm::vec3(0.0f);
                p.vy = 0.0f;
                p.written = t.position;
                p.airTop = t.position.y;
                p.stride = 0.0f;
                standWhereItIs(p);
            }
            // Its motion written by a restore (BodyMotion): what it carried at the snapshot.
            if (!playerMotion.empty() && playerMotion.contains(e))
                if (const auto* m = reg.try_get<BodyMotion>(e)) {
                    p.horizontal = glm::vec3(m->linear.x, 0.0f, m->linear.z);
                    p.vy = m->linear.y;
                    backend->setCharacterLinearVelocity(p.character, m->linear);
                }
            if (posed || !sameRotation(t.rotation, p.writtenRot, 1e-4f)) {
                p.yawDeg = yawOf(t.rotation);
                p.writtenRot = yawQuat(p.yawDeg);
            }
            // A head turned by anything but this system sets the pitch.
            if (p.head != entt::null && reg.valid(p.head))
                if (const auto* ht = reg.try_get<Transform>(p.head);
                    ht && ((!posedExternally.empty() && posedExternally.contains(p.head)) ||
                           !sameRotation(ht->rotation, p.headRot, 1e-4f))) {
                    p.pitchDeg = std::clamp(pitchOf(ht->rotation), -89.0f, 89.0f);
                    p.headRot = pitchQuat(p.pitchDeg);
                }

            // What moves it: the world's input (a player), else its MoveIntent (an AI or an agent;
            // without one it stands, facing as it faces).
            const MoveIntent* intent = pc.input ? nullptr : reg.try_get<MoveIntent>(e);
            glm::vec2 wish(0.0f);
            bool sprinting = false, jumpPressed = false;
            // A dead player (Health 0; combat, sub-project 7) neither walks, turns nor jumps: it
            // stands (or falls) where it died until the round restarts.
            const auto* health = reg.try_get<Health>(e);
            const bool dead = health && !(health->current > 0.0f);
            if (pc.input && !dead) {
                // Look: yaw about +Y (mouse right turns right), pitch kept within 89 degrees.
                p.yawDeg -= (mouse.x + input.axis(pc.lookX)) * pc.lookDegPerUnit;
                p.pitchDeg -= (mouse.y + input.axis(pc.lookY)) * pc.lookDegPerUnit;
                wish = glm::vec2(input.axis(pc.moveRight), input.axis(pc.moveForward));
                sprinting = input.held(pc.sprint);
                jumpPressed = input.pressed(pc.jump);
            } else if (intent) {
                p.yawDeg = intent->yawDeg;
                p.pitchDeg = intent->pitchDeg;
                wish = glm::clamp(intent->move, glm::vec2(-1.0f), glm::vec2(1.0f));
                sprinting = intent->sprint;
                jumpPressed = intent->jump;
            }
            p.yawDeg = std::remainder(p.yawDeg, 360.0f);
            p.pitchDeg = std::clamp(p.pitchDeg, -89.0f, 89.0f);
            const glm::quat rot = yawQuat(p.yawDeg);
            backend->setCharacterRotation(p.character, rot);

            // The wanted horizontal velocity, in the yaw frame.
            if (glm::dot(wish, wish) > 1.0f) wish = glm::normalize(wish);
            const float speed = sprinting ? pc.sprintSpeed : pc.walkSpeed;
            const glm::vec3 target = (rot * glm::vec3(0.0f, 0.0f, -1.0f) * wish.y + rot * glm::vec3(1.0f, 0.0f, 0.0f) * wish.x) * speed;

            // On the ground (and not rising from a jump): walk and maybe jump; else fall.
            const pb::CharacterState before = backend->getCharacterState(p.character);
            const bool grounded = before.groundState == pb::GroundState::ON_GROUND && p.vy < 0.1f;
            const float accel = (grounded ? 1.0f : pc.airControl) * pc.acceleration * dt;
            const glm::vec3 dv = target - p.horizontal;
            const float dvLen = glm::length(dv);
            p.horizontal = dvLen <= accel || dvLen == 0.0f ? target : p.horizontal + dv * (accel / dvLen);
            bool jumped = false;
            if (grounded) {
                p.vy = 0.0f;
                if (jumpPressed) {
                    p.vy = pc.jumpSpeed;
                    jumped = true;
                }
            }
            // The move takes the step's average vertical velocity (half a step of gravity), so a
            // jump follows the exact parabola: apex jumpSpeed^2 / 2g.
            const bool falling = !grounded || jumped;
            const float vMove = falling ? p.vy + 0.5f * gravity.y * dt : 0.0f;
            backend->moveCharacter(p.character, dt, glm::vec3(p.horizontal.x, vMove, p.horizontal.z), gravity,
                                   pc.stepHeight, grounded && !jumped ? 0.5f : 0.0f);
            const pb::CharacterState after = backend->getCharacterState(p.character);
            // What the move left: sideways what a wall took away, vertically the step's end.
            p.horizontal = glm::vec3(after.linearVelocity.x, 0.0f, after.linearVelocity.z);
            if (falling) p.vy = after.linearVelocity.y + 0.5f * gravity.y * dt;
            const bool onGround = after.groundState == pb::GroundState::ON_GROUND && p.vy < 0.1f;
            if (onGround) p.vy = 0.0f;

            // Footsteps: one every stride walked on the ground; a land on touching it after a fall.
            if (onGround) {
                if (!p.grounded) {
                    if (p.airTop - after.position.y > kLandDrop)
                        out.push_back(playerEvent(PhysicsEvent::Kind::Land, e, after, std::max(0.0f, -vMove)));
                    p.stride = 0.0f;   // a new stride starts where it lands
                }
                const glm::vec3 moved = after.position - before.position;
                p.stride += glm::length(glm::vec2(moved.x, moved.z));
                const float stride = sprinting ? kSprintStride : kWalkStride;
                if (p.stride >= stride) {
                    p.stride -= stride;
                    out.push_back(playerEvent(PhysicsEvent::Kind::Footstep, e, after, glm::length(p.horizontal)));
                    ++p.footsteps;
                }
                p.airTop = after.position.y;
            } else {
                p.airTop = std::max(p.airTop, after.position.y);
            }
            p.grounded = onGround;
            p.placed = false;
            p.ground = onGround ? groundEntity(after) : entt::entity{entt::null};
            writePlayer(e, p, after.position, glm::vec3(p.horizontal.x, p.vy, p.horizontal.z), onGround, true);
        }
        playerMotion.clear();
    }

    void step(const InputMap& input, float dt) {
        using Clock = std::chrono::steady_clock;
        const auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto t0 = Clock::now();
        ++steps;
        lastSubsteps = 0;
        timing = {};
        std::vector<PhysicsEvent> fresh;
        if (bodies.empty() && touched.empty() && pending.empty() && players.empty() && playersTouched.empty() &&
            reg.view<PhysicsMoved>().empty()) {
            stepEvents.clear();   // a world without colliders or players: no work at all
            posedExternally.clear();
            return;
        }
        propagateTransforms(reg);
        stepDt = dt;
        sync();
        stepDt = 0.0f;
        for (auto& [e, mv] : movers) mv.moved = false;
        const auto tp = Clock::now();
        updatePlayers(input, dt, fresh);
        posedExternally.clear();   // the bodies took theirs in the sync, the players in their update
        auto t1 = Clock::now(), t2 = t1, t3 = t1;
        timing.playerMs = ms(tp, t1);
        if (backend && dt > 0.0f) {
            constexpr float kMaxDt = 1.0f / 60.0f;
            int n = static_cast<int>(std::ceil(dt / kMaxDt - 1e-4f));
            if (n > PhysicsScene::kMaxSubsteps) {
                if (!warnedLongStep)
                    OHAO_LOG(Warn, physics, "physics: a step of {} s is integrated as {} updates of {} s", dt,
                             PhysicsScene::kMaxSubsteps, dt / float(PhysicsScene::kMaxSubsteps));
                warnedLongStep = true;
                n = PhysicsScene::kMaxSubsteps;
            }
            n = std::max(n, 1);
            t1 = Clock::now();
            for (int i = 0; i < n; ++i) backend->step(dt / float(n));
            lastSubsteps = n;
            // Kinematic bodies moved this step stop at their targets until their entities move again.
            for (const pb::BodyHandle h : kinematicMoved) {
                backend->setLinearVelocity(h, glm::vec3(0.0f));
                backend->setAngularVelocity(h, glm::vec3(0.0f));
            }
            kinematicMoved.clear();
            t2 = Clock::now();
            drainContacts(fresh);
            t3 = Clock::now();
            writeBack();
        }
        // What this step did not move is drawn where it is.
        for (auto it = movers.begin(); it != movers.end();) it = it->second.moved ? std::next(it) : movers.erase(it);
        const auto t4 = Clock::now();
        publish(fresh);
        const auto t5 = Clock::now();
        timing.joltMs = ms(t1, t2);
        timing.eventsMs = ms(t2, t3) + ms(t4, t5);
        timing.writeMs = ms(t3, t4);
        timing.totalMs = ms(t0, t5);
    }

    // RenderPose for what the last step moved, between its last two step poses at alpha, and for
    // their subtrees (a descendant that moves by itself has its own entry). Alpha 1 (or outside
    // [0, 1)) draws the step's poses: every RenderPose goes. The step's state is not touched.
    void interpolate(float alpha) {
        ankerl::unordered_dense::set<entt::entity> was;
        was.swap(drawn);
        if (alpha >= 0.0f && alpha < 1.0f) {
            std::vector<entt::entity> stack;
            for (const auto& [e, mv] : movers) {
                if (!reg.valid(e) || !reg.all_of<WorldTransform>(e)) continue;
                if (mv.fromPos == mv.toPos && mv.fromRot == mv.toRot) continue;
                const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::mix(mv.fromPos, mv.toPos, alpha)) *
                                     glm::mat4_cast(glm::slerp(mv.fromRot, mv.toRot, alpha));
                const glm::mat4 to = glm::translate(glm::mat4(1.0f), mv.toPos) * glm::mat4_cast(mv.toRot);
                const glm::mat4 delta = at * glm::inverse(to);
                stack.assign(1, e);
                while (!stack.empty()) {
                    const entt::entity x = stack.back();
                    stack.pop_back();
                    if (!reg.valid(x) || (x != e && movers.contains(x))) continue;
                    if (const auto* wt = reg.try_get<WorldTransform>(x)) {
                        const auto* last = reg.try_get<RenderPose>(x);
                        const glm::mat4 prev = last && was.contains(x) ? last->world : wt->prevWorld;
                        reg.emplace_or_replace<RenderPose>(x, RenderPose{delta * wt->world, prev});
                        reg.emplace_or_replace<RenderDirty>(x);
                        drawn.insert(x);
                    }
                    if (const auto* ch = reg.try_get<Children>(x)) stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
                }
            }
        }
        for (const entt::entity x : was) {
            if (drawn.contains(x) || !reg.valid(x)) continue;
            reg.remove<RenderPose>(x);
            reg.emplace_or_replace<RenderDirty>(x);
        }
    }

    [[nodiscard]] std::vector<PhysicsEvent> since(uint64_t after, std::size_t max, uint64_t& dropped) const {
        const uint64_t last = nextSeq - 1;
        const uint64_t oldest = last > PhysicsScene::kEventLog ? last - PhysicsScene::kEventLog + 1 : 1;
        const uint64_t first = std::max(after + 1, oldest);
        dropped = first > after + 1 ? first - (after + 1) : 0;
        std::vector<PhysicsEvent> out;
        for (uint64_t s = first; s <= last && out.size() < max; ++s) out.push_back(log[(s - 1) % PhysicsScene::kEventLog]);
        return out;
    }
};

struct PhysicsScene::Impl::SavedSimulation {
    std::string backend;                                        // bodies, contact cache, characters
    std::vector<std::pair<pb::BodyHandle, uint64_t>> bodies;    // which body is whose (bodyIdentity)
    std::vector<SavedPlayer> players;
    std::vector<std::pair<uint64_t, Pair>> pairs;               // the contact bookkeeping, entities by id
    std::size_t dormant{0};
};

std::shared_ptr<const void> PhysicsScene::Impl::saveSimulation() const {
    if (!backend) return nullptr;
    auto s = std::make_shared<SavedSimulation>();
    s->backend = backend->saveState();
    s->bodies = bodyIdentity();
    for (const auto& [e, p] : players) s->players.push_back(SavedPlayer{idOf(e), idOf(p.head), idOf(p.ground), p});
    s->pairs.assign(pairs.begin(), pairs.end());
    s->dormant = dormant;
    return s;
}

// The saved simulation back in place, when its bodies and players are this world's (the same
// handles for the same entities); else nothing changes and false.
bool PhysicsScene::Impl::restoreSaved() {
    const std::shared_ptr<const SavedSimulation> s = std::move(savedPending);
    savedPending.reset();
    if (!backend || !s || s->backend.empty() || bodyIdentity() != s->bodies || s->players.size() != players.size())
        return false;
    ankerl::unordered_dense::map<uint64_t, entt::entity> byId;
    for (auto [e, id] : reg.view<const EntityId>().each()) byId.emplace(id.value, e);
    const auto entityOfId = [&](uint64_t id) {
        const auto it = byId.find(id);
        return id != 0 && it != byId.end() ? it->second : entt::entity{entt::null};
    };
    for (const SavedPlayer& sp : s->players) {
        const auto it = players.find(entityOfId(sp.id));
        if (it == players.end() || it->second.character != sp.record.character) return false;
    }
    if (!backend->restoreState(s->backend)) {
        OHAO_LOG(Warn, physics, "physics: a snapshot's simulation did not restore; bodies restart from their poses");
        return false;
    }
    for (auto& [e, r] : bodies) {
        r.wpos = backend->getPosition(r.handle);
        r.wrot = backend->getRotation(r.handle);
        r.awake = r.motion == Motion::Dynamic && backend->isAwake(r.handle);
    }
    for (const SavedPlayer& sp : s->players) {
        const entt::entity e = entityOfId(sp.id);
        PlayerRecord& p = players[e];
        p = sp.record;
        p.head = entityOfId(sp.headId);
        p.ground = entityOfId(sp.groundId);
        posedExternally.erase(e);
        if (p.head != entt::null) posedExternally.erase(p.head);
    }
    pairs.clear();
    for (const auto& [key, saved] : s->pairs) {
        Pair p = saved;
        p.a = entityOfId(p.aId);
        p.b = entityOfId(p.bId);
        pairs.emplace(key, p);
    }
    dormant = s->dormant;
    movers.clear();          // placed, not moved: nothing to draw between
    motionTouched.clear();   // the simulation carries every body's motion
    playerMotion.clear();
    return true;
}

PhysicsScene::PhysicsScene(entt::registry& reg, AssetResolver& assets) : m(std::make_unique<Impl>(reg, assets)) {}

std::shared_ptr<const void> PhysicsScene::saveSimulation() const { return m->saveSimulation(); }
void PhysicsScene::restoreSimulation(const std::shared_ptr<const void>& state) { m->restoreSimulation(state); }
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

std::optional<RayHit> PhysicsScene::raycastFrom(glm::vec3 from, glm::vec3 dir, float max, entt::entity ignore) const {
    if (!m->backend || glm::dot(dir, dir) == 0.0f || !(max > 0.0f)) return std::nullopt;
    const glm::vec3 d = glm::normalize(dir);
    for (const pb::RaycastHit& hit : m->backend->castRayAll(from, d, max)) {
        const auto it = m->entityOf.find(hit.bodyHandle);
        const entt::entity e = it == m->entityOf.end() ? entt::entity{entt::null} : it->second;
        if (e != entt::null) {
            if (e == ignore) continue;
            if (const auto r = m->bodies.find(e); r != m->bodies.end() && r->second.trigger) continue;
        }
        RayHit out;
        out.entity = e;
        out.point = hit.position;
        out.normal = hit.normal;
        out.dist = hit.fraction * max;
        return out;
    }
    return std::nullopt;
}

bool PhysicsScene::turnPlayer(entt::entity e, float yawDeg, float pitchDeg) { return m->turnPlayer(e, yawDeg, pitchDeg); }

PhysicsInfo PhysicsScene::info() const {
    PhysicsInfo out;
    using Kind = Impl::BodyRecord::Kind;
    for (const auto& [e, r] : m->bodies) {
        switch (r.motion) {
            case Motion::Static: ++out.bodies.statics; break;
            case Motion::Dynamic:
                ++out.bodies.dynamics;
                if (m->backend && m->backend->isAwake(r.handle)) ++out.awake;
                break;
            case Motion::Kinematic: ++out.bodies.kinematics; break;
        }
        if (r.trigger) ++out.triggers;
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
    out.steps = m->steps;
    out.contacts = static_cast<uint32_t>(m->pairs.size());
    out.players = static_cast<uint32_t>(m->players.size());
    return out;
}

std::vector<PhysicsContact> PhysicsScene::contacts() const {
    std::vector<PhysicsContact> out;
    out.reserve(m->pairs.size());
    for (const auto& [key, p] : m->pairs) out.push_back(PhysicsContact{p.a, p.b, p.aId, p.bId, p.point, p.normal, p.trigger, p.since});
    std::sort(out.begin(), out.end(), [](const PhysicsContact& x, const PhysicsContact& y) {
        return x.aId != y.aId ? x.aId < y.aId : x.bId < y.bId;
    });
    return out;
}

std::vector<entt::entity> PhysicsScene::players() const {
    std::vector<entt::entity> out;
    for (const auto& [e, p] : m->players)
        if (m->reg.valid(e)) out.push_back(e);
    std::sort(out.begin(), out.end(), [&](entt::entity a, entt::entity b) { return m->seqOf(a) < m->seqOf(b); });
    return out;
}

std::optional<PlayerInfo> PhysicsScene::playerInfo(entt::entity e) const {
    const auto it = m->players.find(e);
    if (it == m->players.end()) return std::nullopt;
    const Impl::PlayerRecord& p = it->second;
    PlayerInfo out;
    out.pos = p.written;
    out.yawDeg = p.yawDeg;
    out.pitchDeg = p.pitchDeg;
    out.velocity = glm::vec3(p.horizontal.x, p.vy, p.horizontal.z);
    out.onGround = p.grounded;
    out.footsteps = p.footsteps;
    if (p.grounded && p.ground != entt::null && m->reg.valid(p.ground)) {
        out.ground = p.ground;
        const auto* c = m->reg.try_get<Collider>(p.ground);
        out.surface = c && !c->surface.empty() ? c->surface : std::string("default");
    } else if (p.grounded) {
        out.surface = "default";
    }
    return out;
}

bool PhysicsScene::teleportPlayer(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg) {
    return m->teleport(e, pos, yawDeg);
}

void PhysicsScene::step(World& world, float dt) { m->step(world.input(), dt); }
void PhysicsScene::spawnPlayers() { m->spawnPlayers(); }
void PhysicsScene::interpolate(float alpha) { m->interpolate(alpha); }
std::span<const PhysicsEvent> PhysicsScene::stepEvents() const noexcept { return m->stepEvents; }
std::vector<PhysicsEvent> PhysicsScene::eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const {
    return m->since(since, max, dropped);
}
uint64_t PhysicsScene::nextEventSeq() const noexcept { return m->nextSeq; }
uint64_t PhysicsScene::steps() const noexcept { return m->steps; }
int PhysicsScene::lastSubsteps() const noexcept { return m->lastSubsteps; }
PhysicsScene::StepTiming PhysicsScene::lastStepTiming() const noexcept { return m->timing; }

const char* physicsEventKindName(PhysicsEvent::Kind k) noexcept {
    switch (k) {
        case PhysicsEvent::Kind::ContactBegin: return "contact_begin";
        case PhysicsEvent::Kind::ContactEnd: return "contact_end";
        case PhysicsEvent::Kind::TriggerEnter: return "trigger_enter";
        case PhysicsEvent::Kind::TriggerExit: return "trigger_exit";
        case PhysicsEvent::Kind::Footstep: return "footstep";
        case PhysicsEvent::Kind::Land: return "land";
    }
    return "?";
}

std::span<const PhysicsEvent> physicsEvents(const World& world) {
    const PhysicsScene* s = physicsScene(world);
    return s ? s->stepEvents() : std::span<const PhysicsEvent>{};
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
    // Snapshots carry the simulation, so a restore replays exactly (spec "Snapshots and replay").
    PhysicsScene* raw = scene.get();
    reg.ctx().emplace<SnapshotHook>(SnapshotHook{[raw] { return raw->saveSimulation(); },
                                                 [raw](const std::shared_ptr<const void>& s) { raw->restoreSimulation(s); }});
    // Players spawn before the transform system, so a play scene's first frame is the spawn's.
    world.addSyncBefore("transform", "player_spawn", [scene](World&) { scene->spawnPlayers(); });
    world.addSyncAfter("transform", "physics", [scene](World& w) { scene->sync(w.registry()); });
    // Render interpolation, before the render extract reads the poses (installRenderExtract
    // places itself after it when it comes later).
    const auto names = world.syncNames();
    const auto interpolate = [scene](World& w) { scene->interpolate(w.renderAlpha()); };
    if (std::find(names.begin(), names.end(), "render_extract") != names.end())
        world.addSyncBefore("render_extract", "interpolate", interpolate);
    else
        world.addSyncAfter("physics", "interpolate", interpolate);
    // Once per fixed step, after the simulate systems added so far (spin, fly controller).
    world.addSimulate("physics", [scene](World& w, float dt) { scene->step(w, dt); });
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
