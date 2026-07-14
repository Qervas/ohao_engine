// Only compile Jolt backend if Jolt is available
#if !defined(OHAO_HAS_JOLT) || OHAO_HAS_JOLT

#include "jolt_backend.hpp"
#include "jolt_helpers.hpp"
#include "physics/world/physics_world.hpp"

// Jolt includes
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>

// Shapes
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

// Raycasting & queries
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

// Contact listener
#include <Jolt/Physics/Collision/ContactListener.h>

// Constraints
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

// Character
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <iostream>
#include <thread>
#include <cstring>
#include <cmath>
#include <algorithm>


JPH_SUPPRESS_WARNINGS

namespace ohao {
namespace physics {
namespace backend {

// ============================================================================
// Collision Layer System (16 layers → 2 broadphase layers)
// ============================================================================

// Non-moving broadphase layers: STATIC(1), TERRAIN(10)
static bool isNonMovingLayer(uint16_t layer) {
    return layer == CollisionLayer::STATIC || layer == CollisionLayer::TERRAIN;
}

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr unsigned int NUM_LAYERS = 2;
}

// ============================================================================
// Broadphase layer interface - maps 16 object layers → 2 broadphase layers
// ============================================================================

struct JoltPhysicsBackend::BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return isNonMovingLayer(static_cast<uint16_t>(inLayer))
            ? BroadPhaseLayers::NON_MOVING
            : BroadPhaseLayers::MOVING;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
            default: return "UNKNOWN";
        }
    }
#endif
};

// ============================================================================
// Object vs Broadphase layer filter
// ============================================================================

struct JoltPhysicsBackend::ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        bool layer1IsNonMoving = isNonMovingLayer(static_cast<uint16_t>(inLayer1));
        if (layer1IsNonMoving) {
            // Non-moving only collides with moving broadphase
            return inLayer2 == BroadPhaseLayers::MOVING;
        }
        // Moving collides with everything
        return true;
    }
};

// ============================================================================
// Object layer pair filter (16x16 collision matrix)
// ============================================================================

struct JoltPhysicsBackend::ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
    bool collisionMatrix[CollisionLayer::NUM_LAYERS][CollisionLayer::NUM_LAYERS];

    ObjectLayerPairFilterImpl() {
        // Default: everything collides with everything
        for (int i = 0; i < CollisionLayer::NUM_LAYERS; i++)
            for (int j = 0; j < CollisionLayer::NUM_LAYERS; j++)
                collisionMatrix[i][j] = true;

        // Non-moving objects don't collide with each other
        setCollision(CollisionLayer::STATIC, CollisionLayer::STATIC, false);
        setCollision(CollisionLayer::STATIC, CollisionLayer::TERRAIN, false);
        setCollision(CollisionLayer::TERRAIN, CollisionLayer::TERRAIN, false);

        // Triggers don't collide with static/terrain (they only detect dynamic/character)
        setCollision(CollisionLayer::TRIGGER, CollisionLayer::STATIC, false);
        setCollision(CollisionLayer::TRIGGER, CollisionLayer::TERRAIN, false);
        setCollision(CollisionLayer::TRIGGER, CollisionLayer::TRIGGER, false);

        // Debris doesn't collide with other debris (performance)
        setCollision(CollisionLayer::DEBRIS, CollisionLayer::DEBRIS, false);
        // Debris doesn't collide with triggers
        setCollision(CollisionLayer::DEBRIS, CollisionLayer::TRIGGER, false);
    }

    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
        if (inLayer1 >= CollisionLayer::NUM_LAYERS || inLayer2 >= CollisionLayer::NUM_LAYERS)
            return true; // Fallback: unknown layers collide
        return collisionMatrix[inLayer1][inLayer2];
    }

    void setCollision(uint16_t l1, uint16_t l2, bool collide) {
        if (l1 < CollisionLayer::NUM_LAYERS && l2 < CollisionLayer::NUM_LAYERS) {
            collisionMatrix[l1][l2] = collide;
            collisionMatrix[l2][l1] = collide;
        }
    }
};

// ============================================================================
// Layer mask filter for raycasts/queries
// ============================================================================

class LayerMaskObjectFilter : public JPH::ObjectLayerFilter {
public:
    explicit LayerMaskObjectFilter(uint16_t mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer inLayer) const override {
        if (inLayer >= CollisionLayer::NUM_LAYERS) return true;
        return (m_mask & (1 << inLayer)) != 0;
    }
private:
    uint16_t m_mask;
};

// ============================================================================
// Contact Listener (Jolt thread-safe → event queue)
// ============================================================================

struct JoltPhysicsBackend::JoltContactListenerImpl : public JPH::ContactListener {
    JoltPhysicsBackend* backend = nullptr;

    explicit JoltContactListenerImpl(JoltPhysicsBackend* b) : backend(b) {}

    JPH::ValidateResult OnContactValidate(
        const JPH::Body& /*inBody1*/, const JPH::Body& /*inBody2*/,
        JPH::RVec3Arg /*inBaseOffset*/,
        const JPH::CollideShapeResult& /*inCollisionResult*/) override
    {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(
        const JPH::Body& inBody1, const JPH::Body& inBody2,
        const JPH::ContactManifold& inManifold,
        JPH::ContactSettings& /*ioSettings*/) override
    {
        ContactEvent event;
        event.type = ContactEvent::Type::BEGIN;
        event.body1 = backend->lookupHandle(inBody1.GetID());
        event.body2 = backend->lookupHandle(inBody2.GetID());
        if (inManifold.mRelativeContactPointsOn1.size() > 0) {
            event.contactPoint = toGLM(JPH::Vec3(inManifold.mRelativeContactPointsOn1[0])
                                       + JPH::Vec3(inManifold.mBaseOffset));
        }
        event.contactNormal = toGLM(inManifold.mWorldSpaceNormal);
        event.penetrationDepth = inManifold.mPenetrationDepth;
        // Compute relative velocity
        event.relativeVelocity = toGLM(inBody1.GetLinearVelocity() - inBody2.GetLinearVelocity());
        backend->pushContactEvent(event);
    }

    void OnContactPersisted(
        const JPH::Body& inBody1, const JPH::Body& inBody2,
        const JPH::ContactManifold& inManifold,
        JPH::ContactSettings& /*ioSettings*/) override
    {
        ContactEvent event;
        event.type = ContactEvent::Type::PERSIST;
        event.body1 = backend->lookupHandle(inBody1.GetID());
        event.body2 = backend->lookupHandle(inBody2.GetID());
        if (inManifold.mRelativeContactPointsOn1.size() > 0) {
            event.contactPoint = toGLM(JPH::Vec3(inManifold.mRelativeContactPointsOn1[0])
                                       + JPH::Vec3(inManifold.mBaseOffset));
        }
        event.contactNormal = toGLM(inManifold.mWorldSpaceNormal);
        event.penetrationDepth = inManifold.mPenetrationDepth;
        event.relativeVelocity = toGLM(inBody1.GetLinearVelocity() - inBody2.GetLinearVelocity());
        backend->pushContactEvent(event);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override {
        ContactEvent event;
        event.type = ContactEvent::Type::END;
        event.body1 = backend->lookupHandle(inSubShapePair.GetBody1ID());
        event.body2 = backend->lookupHandle(inSubShapePair.GetBody2ID());
        backend->pushContactEvent(event);
    }
};

// ============================================================================
// Static Jolt initialization
// ============================================================================

static bool s_joltGlobalInit = false;

static void ensureJoltGlobalInit() {
    if (s_joltGlobalInit) return;

    JPH::RegisterDefaultAllocator();

    if (!JPH::Factory::sInstance) {
        JPH::Factory::sInstance = new JPH::Factory();
    }

    JPH::RegisterTypes();

    s_joltGlobalInit = true;
    std::cout << "[Jolt] Global initialization complete" << std::endl;
}

// ============================================================================
// Helper: compute a perpendicular normal axis for constraints
// ============================================================================

static glm::vec3 computeNormalAxis(const glm::vec3& axis) {
    glm::vec3 n = glm::normalize(axis);
    if (std::abs(n.y) < 0.9f)
        return glm::normalize(glm::cross(n, glm::vec3(0, 1, 0)));
    return glm::normalize(glm::cross(n, glm::vec3(1, 0, 0)));
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

JoltPhysicsBackend::JoltPhysicsBackend() = default;

JoltPhysicsBackend::~JoltPhysicsBackend() {
    if (m_initialized) {
        shutdown();
    }
}

// ============================================================================
// Initialize
// ============================================================================

bool JoltPhysicsBackend::initialize(const PhysicsWorldConfig& config) {
    try {
        ensureJoltGlobalInit();

        // Temp allocator (10 MB)
        m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

        // Job system
        unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
        if (!config.enableMultithreading) {
            numThreads = 0;
        } else if (config.workerThreads > 0) {
            numThreads = static_cast<unsigned int>(config.workerThreads);
        }
        m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, numThreads
        );

        // Collision filtering (16-layer system)
        m_broadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterfaceImpl>();
        m_objectVsBroadPhaseFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
        m_objectLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();

        // Contact listener
        m_contactListener = std::make_unique<JoltContactListenerImpl>(this);

        // Physics system
        m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();

        const JPH::uint maxBodies = static_cast<JPH::uint>(config.maxBodies);
        const JPH::uint numBodyMutexes = 0;
        const JPH::uint maxBodyPairs = maxBodies * 2;
        const JPH::uint maxContactConstraints = maxBodies;

        m_physicsSystem->Init(
            maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints,
            *m_broadPhaseLayerInterface,
            *m_objectVsBroadPhaseFilter,
            *m_objectLayerPairFilter
        );

        m_physicsSystem->SetGravity(toJolt(config.gravity));
        m_physicsSystem->SetContactListener(m_contactListener.get());

        m_collisionSteps = 1;
        m_initialized = true;

        std::cout << "[Jolt] Backend initialized ("
                  << numThreads << " threads, "
                  << maxBodies << " max bodies, 16-layer collision)" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[Jolt] Init failed: " << e.what() << std::endl;
        shutdown();
        return false;
    } catch (...) {
        std::cerr << "[Jolt] Init failed with unknown error" << std::endl;
        shutdown();
        return false;
    }
}

// ============================================================================
// Shutdown
// ============================================================================

void JoltPhysicsBackend::shutdown() {
    // Destroy characters
    m_characters.clear();

    // Destroy constraints
    if (m_physicsSystem) {
        for (auto& [handle, data] : m_constraints) {
            m_physicsSystem->RemoveConstraint(data.constraint);
        }
    }
    m_constraints.clear();

    // Destroy bodies
    if (m_physicsSystem) {
        JPH::BodyInterface& bi = m_physicsSystem->GetBodyInterface();
        for (auto& [handle, bodyId] : m_handleToBody) {
            if (!bodyId.IsInvalid()) {
                bi.RemoveBody(bodyId);
                bi.DestroyBody(bodyId);
            }
        }
    }
    m_handleToBody.clear();
    m_bodyIdToHandle.clear();
    m_bodyLayers.clear();
    m_nextHandle = 0;
    m_nextConstraintHandle = 0;
    m_nextCharacterHandle = 0;

    // Clear events
    {
        std::lock_guard<std::mutex> lock(m_contactEventsMutex);
        m_contactEvents.clear();
    }
    m_userContactListener = nullptr;

    // Shutdown in reverse init order
    m_physicsSystem.reset();
    m_contactListener.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();
    m_objectLayerPairFilter.reset();
    m_objectVsBroadPhaseFilter.reset();
    m_broadPhaseLayerInterface.reset();

    m_initialized = false;
    std::cout << "[Jolt] Backend shut down" << std::endl;
}

// ============================================================================
// Step
// ============================================================================

void JoltPhysicsBackend::step(float deltaTime) {
    if (!m_initialized || !m_physicsSystem) return;

    try {
        // Update characters before physics step
        // (done externally via updateCharacter calls)

        m_physicsSystem->Update(deltaTime, m_collisionSteps, m_tempAllocator.get(), m_jobSystem.get());

        // Check constraint breaking thresholds
        m_brokenConstraints.clear();
        std::erase_if(m_constraints, [&](auto& entry) {
            auto& data = entry.second;
            if (data.breakingForce <= 0.0f && data.breakingTorque <= 0.0f) {
                return false;
            }
            JPH::Constraint* c = data.constraint.GetPtr();
            float linearImpulse  = 0.0f;
            float angularImpulse = 0.0f;
            switch (data.type) {
                case ConstraintType::FIXED: {
                    auto* fc = static_cast<JPH::FixedConstraint*>(c);
                    linearImpulse  = fc->GetTotalLambdaPosition().Length();
                    angularImpulse = fc->GetTotalLambdaRotation().Length();
                    break;
                }
                case ConstraintType::POINT: {
                    auto* pc = static_cast<JPH::PointConstraint*>(c);
                    linearImpulse = pc->GetTotalLambdaPosition().Length();
                    break;
                }
                case ConstraintType::HINGE: {
                    auto* hc = static_cast<JPH::HingeConstraint*>(c);
                    linearImpulse  = hc->GetTotalLambdaPosition().Length();
                    angularImpulse = hc->GetTotalLambdaRotation().Length();
                    break;
                }
                case ConstraintType::SLIDER: {
                    auto* sc = static_cast<JPH::SliderConstraint*>(c);
                    linearImpulse  = sc->GetTotalLambdaPosition().Length();
                    angularImpulse = sc->GetTotalLambdaRotation().Length();
                    break;
                }
                case ConstraintType::CONE_TWIST: {
                    auto* cc = static_cast<JPH::ConeConstraint*>(c);
                    linearImpulse  = cc->GetTotalLambdaPosition().Length();
                    angularImpulse = std::abs(cc->GetTotalLambdaRotation());
                    break;
                }
                case ConstraintType::DISTANCE: {
                    auto* dc = static_cast<JPH::DistanceConstraint*>(c);
                    linearImpulse = std::abs(dc->GetTotalLambdaPosition());
                    break;
                }
                case ConstraintType::SIX_DOF: {
                    auto* sc = static_cast<JPH::SixDOFConstraint*>(c);
                    linearImpulse  = sc->GetTotalLambdaPosition().Length();
                    angularImpulse = sc->GetTotalLambdaRotation().Length();
                    break;
                }
                default: break;
            }
            bool broken = (data.breakingForce  > 0.0f && linearImpulse  > data.breakingForce) ||
                          (data.breakingTorque > 0.0f && angularImpulse > data.breakingTorque);
            if (broken) {
                m_brokenConstraints.push_back(entry.first);
                m_physicsSystem->RemoveConstraint(data.constraint);
                return true;
            }
            return false;
        });

        // Dispatch contact events to user listener
        if (m_userContactListener) {
            std::vector<ContactEvent> events;
            {
                std::lock_guard<std::mutex> lock(m_contactEventsMutex);
                events = std::move(m_contactEvents);
                m_contactEvents.clear();
            }
            for (const auto& event : events) {
                switch (event.type) {
                    case ContactEvent::Type::BEGIN:   m_userContactListener->onContactBegin(event); break;
                    case ContactEvent::Type::PERSIST: m_userContactListener->onContactPersist(event); break;
                    case ContactEvent::Type::END:     m_userContactListener->onContactEnd(event); break;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Jolt] Step error: " << e.what() << std::endl;
    }
}

// ============================================================================
// Body Management
// ============================================================================

BodyHandle JoltPhysicsBackend::createBody(const BodyCreationInfo& info) {
    if (!m_initialized || !m_physicsSystem) return INVALID_BODY;

    try {
        JPH::ShapeRefC shape = createJoltShape(info.shape);
        if (!shape) {
            std::cerr << "[Jolt] Failed to create shape" << std::endl;
            return INVALID_BODY;
        }

        JPH::EMotionType motionType = toJoltMotionType(info.motionType);

        // Determine collision layer
        uint16_t layer = info.layer;
        if (layer == CollisionLayer::DEFAULT) {
            layer = autoAssignLayer(info.motionType);
        }

        JPH::BodyCreationSettings bodySettings(
            shape, toJoltR(info.position), toJolt(info.rotation),
            motionType, static_cast<JPH::ObjectLayer>(layer)
        );

        bodySettings.mFriction = info.friction;
        bodySettings.mRestitution = info.restitution;
        bodySettings.mLinearDamping = info.linearDamping;
        bodySettings.mAngularDamping = info.angularDamping;
        bodySettings.mGravityFactor = info.gravityEnabled ? info.gravityScale : 0.0f;
        bodySettings.mAllowSleeping = true;

        // CCD
        if (info.useCCD) {
            bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }

        // Mass
        if (info.motionType == MotionType::DYNAMIC) {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = info.mass;
        }

        JPH::BodyInterface& bi = m_physicsSystem->GetBodyInterface();
        JPH::BodyID bodyId = bi.CreateAndAddBody(
            bodySettings,
            motionType == JPH::EMotionType::Static
                ? JPH::EActivation::DontActivate
                : JPH::EActivation::Activate
        );

        if (bodyId.IsInvalid()) {
            std::cerr << "[Jolt] Failed to create body (max bodies reached?)" << std::endl;
            return INVALID_BODY;
        }

        BodyHandle handle = m_nextHandle++;
        m_handleToBody[handle] = bodyId;
        m_bodyIdToHandle[bodyId.GetIndexAndSequenceNumber()] = handle;
        m_bodyLayers[handle] = layer;

        return handle;

    } catch (const std::exception& e) {
        std::cerr << "[Jolt] createBody error: " << e.what() << std::endl;
        return INVALID_BODY;
    }
}

void JoltPhysicsBackend::destroyBody(BodyHandle handle) {
    auto it = m_handleToBody.find(handle);
    if (it == m_handleToBody.end()) return;

    if (m_physicsSystem) {
        JPH::BodyInterface& bi = m_physicsSystem->GetBodyInterface();
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
    }

    m_bodyIdToHandle.erase(it->second.GetIndexAndSequenceNumber());
    m_bodyLayers.erase(handle);
    m_handleToBody.erase(it);
}

bool JoltPhysicsBackend::isValidBody(BodyHandle handle) const {
    return m_handleToBody.find(handle) != m_handleToBody.end();
}

JPH::BodyID JoltPhysicsBackend::lookupBodyID(BodyHandle handle) const {
    auto it = m_handleToBody.find(handle);
    if (it != m_handleToBody.end()) return it->second;
    return JPH::BodyID();
}

BodyHandle JoltPhysicsBackend::lookupHandle(JPH::BodyID joltId) const {
    auto it = m_bodyIdToHandle.find(joltId.GetIndexAndSequenceNumber());
    if (it != m_bodyIdToHandle.end()) return it->second;
    return INVALID_BODY;
}

void JoltPhysicsBackend::pushContactEvent(const ContactEvent& event) {
    std::lock_guard<std::mutex> lock(m_contactEventsMutex);
    m_contactEvents.push_back(event);
}

// ============================================================================
// Transform
// ============================================================================

void JoltPhysicsBackend::setPosition(BodyHandle h, const glm::vec3& pos) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetPosition(id, toJoltR(pos), JPH::EActivation::DontActivate);
}

glm::vec3 JoltPhysicsBackend::getPosition(BodyHandle h) const {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return glm::vec3(0.0f);
    return toGLM(m_physicsSystem->GetBodyInterface().GetPosition(id));
}

void JoltPhysicsBackend::setRotation(BodyHandle h, const glm::quat& rot) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetRotation(id, toJolt(rot), JPH::EActivation::DontActivate);
}

glm::quat JoltPhysicsBackend::getRotation(BodyHandle h) const {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return glm::quat(1, 0, 0, 0);
    return toGLM(m_physicsSystem->GetBodyInterface().GetRotation(id));
}

// ============================================================================
// Velocity
// ============================================================================

void JoltPhysicsBackend::setLinearVelocity(BodyHandle h, const glm::vec3& vel) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetLinearVelocity(id, toJolt(vel));
}

glm::vec3 JoltPhysicsBackend::getLinearVelocity(BodyHandle h) const {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return glm::vec3(0.0f);
    return toGLM(m_physicsSystem->GetBodyInterface().GetLinearVelocity(id));
}

void JoltPhysicsBackend::setAngularVelocity(BodyHandle h, const glm::vec3& vel) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetAngularVelocity(id, toJolt(vel));
}

glm::vec3 JoltPhysicsBackend::getAngularVelocity(BodyHandle h) const {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return glm::vec3(0.0f);
    return toGLM(m_physicsSystem->GetBodyInterface().GetAngularVelocity(id));
}

// ============================================================================
// Forces
// ============================================================================

void JoltPhysicsBackend::applyForce(BodyHandle h, const glm::vec3& force, const glm::vec3& relPos) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;

    JPH::BodyInterface& bi = m_physicsSystem->GetBodyInterface();
    if (glm::length2(relPos) > 0.0001f) {
        JPH::RVec3 worldPos = bi.GetPosition(id) + JPH::Vec3(relPos.x, relPos.y, relPos.z);
        bi.AddForce(id, toJolt(force), worldPos);
    } else {
        bi.AddForce(id, toJolt(force));
    }
}

void JoltPhysicsBackend::applyImpulse(BodyHandle h, const glm::vec3& impulse, const glm::vec3& relPos) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;

    JPH::BodyInterface& bi = m_physicsSystem->GetBodyInterface();
    if (glm::length2(relPos) > 0.0001f) {
        JPH::RVec3 worldPos = bi.GetPosition(id) + JPH::Vec3(relPos.x, relPos.y, relPos.z);
        bi.AddImpulse(id, toJolt(impulse), worldPos);
    } else {
        bi.AddImpulse(id, toJolt(impulse));
    }
}

void JoltPhysicsBackend::applyTorque(BodyHandle h, const glm::vec3& torque) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().AddTorque(id, toJolt(torque));
}

// ============================================================================
// Body Properties
// ============================================================================

void JoltPhysicsBackend::setMotionType(BodyHandle h, MotionType type) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetMotionType(id, toJoltMotionType(type), JPH::EActivation::Activate);
}

void JoltPhysicsBackend::setMass(BodyHandle h, float mass) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;

    JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        JPH::Body& body = lock.GetBody();
        JPH::MotionProperties* mp = body.GetMotionProperties();
        if (mp) {
            float oldInvMass = mp->GetInverseMass();
            float oldMass = (oldInvMass > 0.0f) ? (1.0f / oldInvMass) : 1.0f;
            float scale = (oldMass > 0.0f) ? (mass / oldMass) : 1.0f;
            float invMass = (mass > 0.0f) ? (1.0f / mass) : 0.0f;
            mp->SetInverseMass(invMass);
            if (scale > 0.0f) {
                JPH::Vec3 invInertia = mp->GetInverseInertiaDiagonal();
                mp->SetInverseInertia(invInertia / scale, mp->GetInertiaRotation());
            }
        }
    }
}

void JoltPhysicsBackend::setFriction(BodyHandle h, float friction) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetFriction(id, friction);
}

void JoltPhysicsBackend::setRestitution(BodyHandle h, float restitution) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetRestitution(id, restitution);
}

void JoltPhysicsBackend::setLinearDamping(BodyHandle h, float damping) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        JPH::Body& body = lock.GetBody();
        if (body.GetMotionProperties())
            body.GetMotionProperties()->SetLinearDamping(damping);
    }
}

void JoltPhysicsBackend::setAngularDamping(BodyHandle h, float damping) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        JPH::Body& body = lock.GetBody();
        if (body.GetMotionProperties())
            body.GetMotionProperties()->SetAngularDamping(damping);
    }
}

void JoltPhysicsBackend::setGravityEnabled(BodyHandle h, bool enabled) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetGravityFactor(id, enabled ? 1.0f : 0.0f);
}

void JoltPhysicsBackend::setGravityScale(BodyHandle h, float scale) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    m_physicsSystem->GetBodyInterface().SetGravityFactor(id, scale);
}

// ============================================================================
// Sleep
// ============================================================================

void JoltPhysicsBackend::setAwake(BodyHandle h, bool awake) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    if (awake)
        m_physicsSystem->GetBodyInterface().ActivateBody(id);
    else
        m_physicsSystem->GetBodyInterface().DeactivateBody(id);
}

bool JoltPhysicsBackend::isAwake(BodyHandle h) const {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return false;
    return m_physicsSystem->GetBodyInterface().IsActive(id);
}

// ============================================================================
// Shape
// ============================================================================

void JoltPhysicsBackend::setShape(BodyHandle h, const ShapeInfo& shapeInfo) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;

    JPH::ShapeRefC newShape = createJoltShape(shapeInfo);
    if (!newShape) return;

    m_physicsSystem->GetBodyInterface().SetShape(id, newShape, true, JPH::EActivation::Activate);
}

// ============================================================================
// World Properties
// ============================================================================

void JoltPhysicsBackend::setGravity(const glm::vec3& gravity) {
    if (!m_physicsSystem) return;
    m_physicsSystem->SetGravity(toJolt(gravity));
}

glm::vec3 JoltPhysicsBackend::getGravity() const {
    if (!m_physicsSystem) return glm::vec3(0, -9.81f, 0);
    return toGLM(m_physicsSystem->GetGravity());
}

// ============================================================================
// CCD
// ============================================================================

void JoltPhysicsBackend::setCCDEnabled(BodyHandle h, bool enabled) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;

    m_physicsSystem->GetBodyInterface().SetMotionQuality(
        id, enabled ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete
    );
}

// ============================================================================
// Collision Layers
// ============================================================================

void JoltPhysicsBackend::setBodyLayer(BodyHandle h, uint16_t layer) {
    JPH::BodyID id = lookupBodyID(h);
    if (id.IsInvalid() || !m_physicsSystem) return;
    if (layer >= CollisionLayer::NUM_LAYERS) return;

    m_bodyLayers[h] = layer;
    m_physicsSystem->GetBodyInterface().SetObjectLayer(id, static_cast<JPH::ObjectLayer>(layer));
}

uint16_t JoltPhysicsBackend::getBodyLayer(BodyHandle h) const {
    auto it = m_bodyLayers.find(h);
    if (it != m_bodyLayers.end()) return it->second;
    return CollisionLayer::DEFAULT;
}

void JoltPhysicsBackend::setLayerCollision(uint16_t layer1, uint16_t layer2, bool shouldCollide) {
    if (m_objectLayerPairFilter) {
        m_objectLayerPairFilter->setCollision(layer1, layer2, shouldCollide);
    }
}

// ============================================================================
// Raycasting & Queries
// ============================================================================

bool JoltPhysicsBackend::castRay(const glm::vec3& origin, const glm::vec3& direction,
                                  float maxDistance, RaycastHit& outHit, uint16_t layerMask) const {
    if (!m_physicsSystem) return false;

    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    JPH::RRayCast ray(toJoltR(origin), toJolt(dir));

    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::RayCastResult result;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    bool hit = query.CastRay(ray, result, {}, layerFilter);

    if (hit) {
        outHit.bodyHandle = lookupHandle(result.mBodyID);
        outHit.fraction = result.mFraction;
        outHit.position = origin + direction * (maxDistance * result.mFraction);

        // Get surface normal
        JPH::BodyLockRead lock(m_physicsSystem->GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            outHit.normal = toGLM(body.GetWorldSpaceSurfaceNormal(
                result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
            outHit.layer = static_cast<uint16_t>(body.GetObjectLayer());
        }
    }

    return hit;
}

std::vector<RaycastHit> JoltPhysicsBackend::castRayAll(const glm::vec3& origin, const glm::vec3& direction,
                                                        float maxDistance, uint16_t layerMask) const {
    std::vector<RaycastHit> results;
    if (!m_physicsSystem) return results;

    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    JPH::RRayCast ray(toJoltR(origin), toJolt(dir));

    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::RayCastSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    query.CastRay(ray, settings, collector, {}, layerFilter);

    // Sort by distance
    collector.Sort();

    for (const auto& hit : collector.mHits) {
        RaycastHit outHit;
        outHit.bodyHandle = lookupHandle(hit.mBodyID);
        outHit.fraction = hit.mFraction;
        outHit.position = origin + direction * (maxDistance * hit.mFraction);

        JPH::BodyLockRead lock(m_physicsSystem->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            outHit.normal = toGLM(body.GetWorldSpaceSurfaceNormal(
                hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
            outHit.layer = static_cast<uint16_t>(body.GetObjectLayer());
        }

        results.push_back(outHit);
    }

    return results;
}

bool JoltPhysicsBackend::castSphere(const glm::vec3& origin, const glm::vec3& direction,
                                     float radius, float maxDistance, ShapeCastResult& outHit,
                                     uint16_t layerMask) const {
    if (!m_physicsSystem) return false;

    JPH::SphereShape sphere(radius);
    glm::vec3 dir = glm::normalize(direction) * maxDistance;

    JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
        &sphere, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(toJoltR(origin)),
        toJolt(dir)
    );

    JPH::ShapeCastSettings settings;
    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    query.CastShape(shapeCast, settings, toJoltR(origin), collector, {}, layerFilter);

    if (collector.HadHit()) {
        outHit.bodyHandle = lookupHandle(collector.mHit.mBodyID2);
        outHit.fraction = collector.mHit.mFraction;
        outHit.contactPoint = toGLM(JPH::Vec3(collector.mHit.mContactPointOn2));
        outHit.contactNormal = toGLM(collector.mHit.mPenetrationAxis.Normalized());
        return true;
    }

    return false;
}

bool JoltPhysicsBackend::castBox(const glm::vec3& origin, const glm::vec3& direction,
                                  const glm::vec3& halfExtents, const glm::quat& rotation,
                                  float maxDistance, ShapeCastResult& outHit, uint16_t layerMask) const {
    if (!m_physicsSystem) return false;

    JPH::BoxShape box(toJolt(halfExtents));
    glm::vec3 dir = glm::normalize(direction) * maxDistance;

    JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
        &box, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(toJolt(rotation), toJoltR(origin)),
        toJolt(dir)
    );

    JPH::ShapeCastSettings settings;
    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    query.CastShape(shapeCast, settings, toJoltR(origin), collector, {}, layerFilter);

    if (collector.HadHit()) {
        outHit.bodyHandle = lookupHandle(collector.mHit.mBodyID2);
        outHit.fraction = collector.mHit.mFraction;
        outHit.contactPoint = toGLM(JPH::Vec3(collector.mHit.mContactPointOn2));
        outHit.contactNormal = toGLM(collector.mHit.mPenetrationAxis.Normalized());
        return true;
    }

    return false;
}

std::vector<BodyHandle> JoltPhysicsBackend::overlapSphere(const glm::vec3& center, float radius,
                                                           uint16_t layerMask) const {
    std::vector<BodyHandle> result;
    if (!m_physicsSystem) return result;

    JPH::SphereShape sphere(radius);
    JPH::CollideShapeSettings settings;
    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    query.CollideShape(
        &sphere, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(toJoltR(center)),
        settings, toJoltR(center), collector, {}, layerFilter
    );

    for (const auto& hit : collector.mHits) {
        BodyHandle h = lookupHandle(hit.mBodyID2);
        if (h != INVALID_BODY) result.push_back(h);
    }

    return result;
}

std::vector<BodyHandle> JoltPhysicsBackend::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                        const glm::quat& rotation, uint16_t layerMask) const {
    std::vector<BodyHandle> result;
    if (!m_physicsSystem) return result;

    JPH::BoxShape box(toJolt(halfExtents));
    JPH::CollideShapeSettings settings;
    LayerMaskObjectFilter layerFilter(layerMask);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    const JPH::NarrowPhaseQuery& query = m_physicsSystem->GetNarrowPhaseQuery();
    query.CollideShape(
        &box, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(toJolt(rotation), toJoltR(center)),
        settings, toJoltR(center), collector, {}, layerFilter
    );

    for (const auto& hit : collector.mHits) {
        BodyHandle h = lookupHandle(hit.mBodyID2);
        if (h != INVALID_BODY) result.push_back(h);
    }

    return result;
}

// ============================================================================
// Contact Callbacks
// ============================================================================

void JoltPhysicsBackend::setContactListener(IContactListener* listener) {
    m_userContactListener = listener;
}

std::vector<ContactEvent> JoltPhysicsBackend::getContactEvents() {
    std::lock_guard<std::mutex> lock(m_contactEventsMutex);
    std::vector<ContactEvent> events = std::move(m_contactEvents);
    m_contactEvents.clear();
    return events;
}

// ============================================================================
// Constraints
// ============================================================================

ConstraintHandle JoltPhysicsBackend::createConstraint(const ConstraintSettings& settings) {
    if (!m_physicsSystem) return INVALID_CONSTRAINT;

    JPH::BodyID bodyId1 = lookupBodyID(settings.body1);
    if (bodyId1.IsInvalid()) return INVALID_CONSTRAINT;

    // Body2 can be INVALID_BODY (world anchor) — use JPH::Body::sFixedToWorld
    JPH::BodyID bodyId2;
    bool body2IsWorld = (settings.body2 == INVALID_BODY);
    if (!body2IsWorld) {
        bodyId2 = lookupBodyID(settings.body2);
        if (bodyId2.IsInvalid()) return INVALID_CONSTRAINT;
    }

    JPH::Ref<JPH::Constraint> constraint;

    // Helper: lock bodies and create constraint from settings
    auto lockAndCreate = [&](JPH::TwoBodyConstraintSettings& cs) -> JPH::Ref<JPH::Constraint> {
        if (body2IsWorld) {
            JPH::BodyLockWrite lock1(m_physicsSystem->GetBodyLockInterface(), bodyId1);
            if (!lock1.Succeeded()) return nullptr;
            return cs.Create(lock1.GetBody(), JPH::Body::sFixedToWorld);
        } else {
            JPH::BodyID ids[] = { bodyId1, bodyId2 };
            JPH::BodyLockMultiWrite lock(m_physicsSystem->GetBodyLockInterface(), ids, 2);
            JPH::Body* b1 = lock.GetBody(0);
            JPH::Body* b2 = lock.GetBody(1);
            if (!b1 || !b2) return nullptr;
            return cs.Create(*b1, *b2);
        }
    };

    try {
        switch (settings.type) {
            case ConstraintType::FIXED: {
                JPH::FixedConstraintSettings cs;
                cs.mAutoDetectPoint = true;
                cs.mPoint1 = toJoltR(settings.anchor1);
                cs.mPoint2 = toJoltR(settings.anchor2);
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::POINT: {
                JPH::PointConstraintSettings cs;
                cs.mPoint1 = toJoltR(settings.anchor1);
                cs.mPoint2 = toJoltR(settings.anchor2);
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::HINGE: {
                JPH::HingeConstraintSettings cs;
                cs.mPoint1 = toJoltR(settings.anchor1);
                cs.mHingeAxis1 = toJolt(glm::normalize(settings.axis1));
                cs.mNormalAxis1 = toJolt(computeNormalAxis(settings.axis1));
                cs.mPoint2 = toJoltR(settings.anchor2);
                cs.mHingeAxis2 = toJolt(glm::normalize(settings.axis2));
                cs.mNormalAxis2 = toJolt(computeNormalAxis(settings.axis2));
                if (settings.enableLimits) {
                    cs.mLimitsMin = settings.limitMin;
                    cs.mLimitsMax = settings.limitMax;
                }
                if (settings.enableMotor) {
                    cs.mMotorSettings = JPH::MotorSettings(settings.motorMaxForce, 0.0f);
                }
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::SLIDER: {
                JPH::SliderConstraintSettings cs;
                cs.mAutoDetectPoint = true;
                cs.mSliderAxis1 = toJolt(glm::normalize(settings.axis1));
                cs.mNormalAxis1 = toJolt(computeNormalAxis(settings.axis1));
                cs.mSliderAxis2 = toJolt(glm::normalize(settings.axis2));
                cs.mNormalAxis2 = toJolt(computeNormalAxis(settings.axis2));
                if (settings.enableLimits) {
                    cs.mLimitsMin = settings.limitMin;
                    cs.mLimitsMax = settings.limitMax;
                }
                if (settings.enableMotor) {
                    cs.mMotorSettings = JPH::MotorSettings(settings.motorMaxForce, 0.0f);
                }
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::CONE_TWIST: {
                JPH::ConeConstraintSettings cs;
                cs.mPoint1 = toJoltR(settings.anchor1);
                cs.mTwistAxis1 = toJolt(glm::normalize(settings.axis1));
                cs.mPoint2 = toJoltR(settings.anchor2);
                cs.mTwistAxis2 = toJolt(glm::normalize(settings.axis2));
                if (settings.enableLimits) {
                    cs.mHalfConeAngle = settings.limitMax;
                }
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::DISTANCE: {
                JPH::DistanceConstraintSettings cs;
                cs.mPoint1 = toJoltR(settings.anchor1);
                cs.mPoint2 = toJoltR(settings.anchor2);
                if (settings.maxDistance >= 0.0f) {
                    cs.mMinDistance = settings.minDistance;
                    cs.mMaxDistance = settings.maxDistance;
                }
                if (settings.enableSpring) {
                    cs.mLimitsSpringSettings.mFrequency = settings.springFrequency;
                    cs.mLimitsSpringSettings.mDamping = settings.springDamping;
                }
                constraint = lockAndCreate(cs);
                break;
            }
            case ConstraintType::SIX_DOF: {
                JPH::SixDOFConstraintSettings cs;
                cs.mPosition1 = toJoltR(settings.anchor1);
                cs.mPosition2 = toJoltR(settings.anchor2);
                cs.mAxisX1 = toJolt(glm::normalize(settings.axis1));
                cs.mAxisY1 = toJolt(computeNormalAxis(settings.axis1));
                cs.mAxisX2 = toJolt(glm::normalize(settings.axis2));
                cs.mAxisY2 = toJolt(computeNormalAxis(settings.axis2));
                for (int i = 0; i < 6; i++) {
                    auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
                    if (settings.axisMode[i] == 1)
                        cs.MakeFixedAxis(axis);
                    else if (settings.axisMode[i] == 2)
                        cs.SetLimitedAxis(axis, settings.axisLimitMin[i], settings.axisLimitMax[i]);
                    else
                        cs.MakeFreeAxis(axis);
                }
                constraint = lockAndCreate(cs);
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Jolt] createConstraint error: " << e.what() << std::endl;
        return INVALID_CONSTRAINT;
    }

    if (!constraint) return INVALID_CONSTRAINT;

    m_physicsSystem->AddConstraint(constraint);

    ConstraintHandle handle = m_nextConstraintHandle++;
    m_constraints[handle] = { constraint, settings.type, settings.breakingForce, settings.breakingTorque };
    return handle;
}

void JoltPhysicsBackend::destroyConstraint(ConstraintHandle handle) {
    auto it = m_constraints.find(handle);
    if (it == m_constraints.end()) return;

    if (m_physicsSystem) {
        m_physicsSystem->RemoveConstraint(it->second.constraint);
    }
    m_constraints.erase(it);
}

bool JoltPhysicsBackend::isValidConstraint(ConstraintHandle handle) const {
    return m_constraints.find(handle) != m_constraints.end();
}

void JoltPhysicsBackend::setConstraintEnabled(ConstraintHandle handle, bool enabled) {
    auto it = m_constraints.find(handle);
    if (it == m_constraints.end()) return;

    it->second.constraint->SetEnabled(enabled);
}

void JoltPhysicsBackend::setConstraintMotorState(ConstraintHandle handle, bool enabled, float speed, float maxForce) {
    auto it = m_constraints.find(handle);
    if (it == m_constraints.end()) return;

    // Use stored type to static_cast (Jolt disables RTTI, so dynamic_cast crashes)
    JPH::Constraint* c = it->second.constraint.GetPtr();

    if (it->second.type == ConstraintType::HINGE) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(c);
        if (enabled) {
            hinge->SetMotorState(JPH::EMotorState::Velocity);
            hinge->SetTargetAngularVelocity(speed);
        } else {
            hinge->SetMotorState(JPH::EMotorState::Off);
        }
    }
    else if (it->second.type == ConstraintType::SLIDER) {
        auto* slider = static_cast<JPH::SliderConstraint*>(c);
        if (enabled) {
            slider->SetMotorState(JPH::EMotorState::Velocity);
            slider->SetTargetVelocity(speed);
        } else {
            slider->SetMotorState(JPH::EMotorState::Off);
        }
    }
}

void JoltPhysicsBackend::setConstraintLimits(ConstraintHandle handle, float min, float max) {
    auto it = m_constraints.find(handle);
    if (it == m_constraints.end()) return;

    JPH::Constraint* c = it->second.constraint.GetPtr();

    if (it->second.type == ConstraintType::HINGE) {
        static_cast<JPH::HingeConstraint*>(c)->SetLimits(min, max);
    }
    else if (it->second.type == ConstraintType::SLIDER) {
        static_cast<JPH::SliderConstraint*>(c)->SetLimits(min, max);
    }
}

size_t JoltPhysicsBackend::getConstraintCount() const {
    return m_constraints.size();
}

void JoltPhysicsBackend::setConstraintBreaking(ConstraintHandle handle, float maxForce, float maxTorque) {
    auto it = m_constraints.find(handle);
    if (it == m_constraints.end()) return;
    it->second.breakingForce  = maxForce;
    it->second.breakingTorque = maxTorque;
}

std::vector<ConstraintHandle> JoltPhysicsBackend::getAndClearBrokenConstraints() {
    std::vector<ConstraintHandle> result = std::move(m_brokenConstraints);
    m_brokenConstraints.clear();
    return result;
}

// ============================================================================
// Character Controller
// ============================================================================

CharacterHandle JoltPhysicsBackend::createCharacter(const CharacterCreationInfo& info) {
    if (!m_physicsSystem || !m_tempAllocator) return INVALID_CHARACTER;

    try {
        // Create capsule shape
        float halfCylinderHeight = (info.capsuleHeight - 2.0f * info.capsuleRadius) * 0.5f;
        if (halfCylinderHeight < 0.001f) halfCylinderHeight = 0.001f;

        JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(halfCylinderHeight, info.capsuleRadius);

        // Rotate capsule to stand upright (Jolt capsules are along Y by default, which is what we want)
        JPH::CharacterVirtualSettings settings;
        settings.mShape = capsule;
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(info.maxSlopeAngleDeg);
        settings.mMass = info.mass;
        settings.mMaxStrength = info.maxStrength;
        settings.mPenetrationRecoverySpeed = 1.0f;
        settings.mPredictiveContactDistance = 0.1f;

        auto character = new JPH::CharacterVirtual(
            &settings, toJoltR(info.position), toJolt(info.rotation),
            0, // userData
            m_physicsSystem.get()
        );

        CharacterHandle handle = m_nextCharacterHandle++;
        CharacterData data;
        data.character = character;
        data.layer = info.layer;
        m_characters[handle] = std::move(data);

        return handle;

    } catch (const std::exception& e) {
        std::cerr << "[Jolt] createCharacter error: " << e.what() << std::endl;
        return INVALID_CHARACTER;
    }
}

void JoltPhysicsBackend::destroyCharacter(CharacterHandle handle) {
    m_characters.erase(handle);
}

bool JoltPhysicsBackend::isValidCharacter(CharacterHandle handle) const {
    return m_characters.find(handle) != m_characters.end();
}

CharacterState JoltPhysicsBackend::getCharacterState(CharacterHandle handle) const {
    CharacterState state;
    auto it = m_characters.find(handle);
    if (it == m_characters.end()) return state;

    const auto& character = it->second.character;
    state.position = toGLM(character->GetPosition());
    state.rotation = toGLM(character->GetRotation());
    state.linearVelocity = toGLM(character->GetLinearVelocity());

    switch (character->GetGroundState()) {
        case JPH::CharacterVirtual::EGroundState::OnGround:
            state.groundState = GroundState::ON_GROUND;
            break;
        case JPH::CharacterVirtual::EGroundState::OnSteepGround:
            state.groundState = GroundState::ON_STEEP_GROUND;
            break;
        case JPH::CharacterVirtual::EGroundState::NotSupported:
            state.groundState = GroundState::NOT_SUPPORTED;
            break;
        case JPH::CharacterVirtual::EGroundState::InAir:
        default:
            state.groundState = GroundState::IN_AIR;
            break;
    }

    state.groundNormal = toGLM(character->GetGroundNormal());
    state.groundBody = lookupHandle(character->GetGroundBodyID());

    return state;
}

void JoltPhysicsBackend::setCharacterPosition(CharacterHandle handle, const glm::vec3& pos) {
    auto it = m_characters.find(handle);
    if (it == m_characters.end()) return;
    it->second.character->SetPosition(toJoltR(pos));
}

void JoltPhysicsBackend::setCharacterRotation(CharacterHandle handle, const glm::quat& rot) {
    auto it = m_characters.find(handle);
    if (it == m_characters.end()) return;
    it->second.character->SetRotation(toJolt(rot));
}

void JoltPhysicsBackend::setCharacterLinearVelocity(CharacterHandle handle, const glm::vec3& vel) {
    auto it = m_characters.find(handle);
    if (it == m_characters.end()) return;
    it->second.character->SetLinearVelocity(toJolt(vel));
}

void JoltPhysicsBackend::updateCharacter(CharacterHandle handle, float deltaTime,
                                          const glm::vec3& gravity, const glm::vec3& movementInput) {
    auto it = m_characters.find(handle);
    if (it == m_characters.end() || !m_physicsSystem || !m_tempAllocator) return;

    auto& character = it->second.character;

    // Compute desired velocity from input and current state
    glm::vec3 currentVel = toGLM(character->GetLinearVelocity());
    glm::vec3 desiredVel = movementInput;

    // Add gravity
    desiredVel += gravity * deltaTime;

    // If grounded, project gravity out
    auto groundState = character->GetGroundState();
    if (groundState == JPH::CharacterVirtual::EGroundState::OnGround) {
        // Keep current vertical velocity if grounded (no gravity accumulation)
        desiredVel.y = std::max(desiredVel.y, 0.0f);
    }

    character->SetLinearVelocity(toJolt(desiredVel));

    // Extended update handles stairs, slopes, etc.
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0);
    updateSettings.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);

    LayerMaskObjectFilter layerFilter(CollisionLayer::ALL_MASK);

    character->ExtendedUpdate(
        deltaTime,
        toJolt(gravity),
        updateSettings,
        {},  // BroadPhaseLayerFilter (default: all)
        layerFilter,
        {},  // BodyFilter (default: all)
        {},  // ShapeFilter (default: all)
        *m_tempAllocator
    );
}

size_t JoltPhysicsBackend::getCharacterCount() const {
    return m_characters.size();
}

// ============================================================================
// Stats
// ============================================================================

BackendStats JoltPhysicsBackend::getStats() const {
    BackendStats stats;
    if (m_physicsSystem) {
        stats.totalBodies = m_handleToBody.size();
        stats.activeBodies = m_physicsSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        stats.sleepingBodies = stats.totalBodies - stats.activeBodies;
    }
    stats.constraintCount = m_constraints.size();
    stats.characterCount = m_characters.size();
    {
        // Don't lock in const method; just report approximate count
        stats.contactEventCount = m_contactEvents.size();
    }
    return stats;
}

// ============================================================================
// Shape creation
// ============================================================================

JPH::ShapeRefC JoltPhysicsBackend::createJoltShape(const ShapeInfo& info) const {
    JPH::ShapeSettings::ShapeResult result;

    switch (info.type) {
        case ShapeInfo::Type::BOX: {
            // Use minimal convex radius so collision box matches visual box exactly
            JPH::BoxShapeSettings settings(toJolt(info.halfExtents), 0.001f);
            result = settings.Create();
            break;
        }
        case ShapeInfo::Type::SPHERE: {
            JPH::SphereShapeSettings settings(info.radius);
            result = settings.Create();
            break;
        }
        case ShapeInfo::Type::CAPSULE: {
            float halfCylinderHeight = (info.height - 2.0f * info.radius) * 0.5f;
            if (halfCylinderHeight < 0.001f) halfCylinderHeight = 0.001f;
            JPH::CapsuleShapeSettings settings(halfCylinderHeight, info.radius);
            result = settings.Create();
            break;
        }
        case ShapeInfo::Type::CYLINDER: {
            float halfHeight = info.height * 0.5f;
            JPH::CylinderShapeSettings settings(halfHeight, info.radius, 0.001f);
            result = settings.Create();
            break;
        }
        case ShapeInfo::Type::PLANE: {
            // Very thin box as ground plane
            JPH::BoxShapeSettings settings(JPH::Vec3(100.0f, 0.01f, 100.0f));
            result = settings.Create();
            break;
        }
        case ShapeInfo::Type::MESH: {
            if (info.meshVertices && info.meshIndices && info.meshVertexCount > 0 && info.meshIndexCount >= 3) {
                JPH::TriangleList triangles;
                triangles.reserve(info.meshIndexCount / 3);
                for (uint32_t i = 0; i + 2 < info.meshIndexCount; i += 3) {
                    const glm::vec3& v0 = info.meshVertices[info.meshIndices[i]];
                    const glm::vec3& v1 = info.meshVertices[info.meshIndices[i + 1]];
                    const glm::vec3& v2 = info.meshVertices[info.meshIndices[i + 2]];
                    triangles.push_back(JPH::Triangle(
                        JPH::Float3(v0.x, v0.y, v0.z),
                        JPH::Float3(v1.x, v1.y, v1.z),
                        JPH::Float3(v2.x, v2.y, v2.z)
                    ));
                }
                JPH::MeshShapeSettings settings(triangles);
                result = settings.Create();
            } else {
                JPH::BoxShapeSettings settings(JPH::Vec3(0.5f, 0.5f, 0.5f));
                result = settings.Create();
            }
            break;
        }
        case ShapeInfo::Type::HEIGHTFIELD: {
            if (info.heightfieldData && info.heightfieldSizeX > 1 && info.heightfieldSizeZ > 1) {
                // Jolt HeightFieldShape expects a square grid, size must be power-of-2 + 1
                // We'll use the X dimension and scale appropriately
                uint32_t sampleCount = info.heightfieldSizeX;

                JPH::HeightFieldShapeSettings settings(
                    info.heightfieldData,
                    toJoltR(glm::vec3(0.0f)), // offset
                    toJolt(info.heightfieldScale),
                    sampleCount
                );
                settings.mMinHeightValue = info.heightfieldMinHeight;
                settings.mMaxHeightValue = info.heightfieldMaxHeight;

                result = settings.Create();
            } else {
                // Fallback to flat box
                JPH::BoxShapeSettings settings(JPH::Vec3(50.0f, 0.01f, 50.0f));
                result = settings.Create();
            }
            break;
        }
        case ShapeInfo::Type::CONVEX_HULL: {
            if (info.meshVertices && info.meshVertexCount > 0) {
                JPH::Array<JPH::Vec3> points;
                points.reserve(info.meshVertexCount);
                for (uint32_t i = 0; i < info.meshVertexCount; i++) {
                    points.push_back(toJolt(info.meshVertices[i]));
                }
                JPH::ConvexHullShapeSettings settings(points, JPH::cDefaultConvexRadius);
                result = settings.Create();
            } else {
                JPH::BoxShapeSettings settings(JPH::Vec3(0.5f, 0.5f, 0.5f));
                result = settings.Create();
            }
            break;
        }
        default: {
            JPH::BoxShapeSettings settings(JPH::Vec3(0.5f, 0.5f, 0.5f));
            result = settings.Create();
            break;
        }
    }

    if (result.HasError()) {
        std::cerr << "[Jolt] Shape error: " << result.GetError().c_str() << std::endl;
        JPH::BoxShapeSettings fallback(JPH::Vec3(0.5f, 0.5f, 0.5f));
        result = fallback.Create();
    }

    return result.Get();
}

// ============================================================================
// Conversion helpers
// ============================================================================

JPH::EMotionType JoltPhysicsBackend::toJoltMotionType(MotionType type) {
    switch (type) {
        case MotionType::STATIC:    return JPH::EMotionType::Static;
        case MotionType::KINEMATIC: return JPH::EMotionType::Kinematic;
        case MotionType::DYNAMIC:
        default:                    return JPH::EMotionType::Dynamic;
    }
}

uint16_t JoltPhysicsBackend::autoAssignLayer(MotionType type) {
    switch (type) {
        case MotionType::STATIC:    return CollisionLayer::STATIC;
        case MotionType::KINEMATIC: return CollisionLayer::KINEMATIC;
        case MotionType::DYNAMIC:
        default:                    return CollisionLayer::DYNAMIC;
    }
}

} // namespace backend
} // namespace physics
} // namespace ohao

#endif // OHAO_HAS_JOLT
