#include "rigid_body.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/component/transform_component.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"
#include "physics/collision/shapes/cylinder_shape.hpp"
#include "physics/collision/shapes/capsule_shape.hpp"
#include "ui/components/console_widget.hpp"

namespace ohao {
namespace physics {
namespace dynamics {

RigidBody::RigidBody(PhysicsComponent* component) 
    : m_component(component) {
    // Initialize with default material
    m_material = MaterialLibrary::getInstance().getDefault();
    updateMassProperties();
}

RigidBody::~RigidBody() {
    // Cleanup handled by PhysicsWorld
}

// === TYPE & STATE ===
void RigidBody::setType(RigidBodyType type) {
    if (m_type == type) return;
    
    m_type = type;
    updateMassProperties();
    
    // Static bodies should not move
    if (isStatic()) {
        m_linearVelocity = glm::vec3(0.0f);
        m_angularVelocity = glm::vec3(0.0f);
        clearForces();
        setAwake(false);
    } else {
        setAwake(true);
    }
}

// === MASS PROPERTIES ===
void RigidBody::setMass(float mass) {
    if (isStatic()) {
        m_mass = 0.0f;
        m_invMass = 0.0f;
        return;
    }
    
    m_mass = math::clamp(mass, constants::MIN_MASS, constants::MAX_MASS);
    m_invMass = 1.0f / m_mass;
    
    // Recalculate inertia if we have a shape
    calculateInertiaFromShape();
}

void RigidBody::setInertiaTensor(const glm::mat3& tensor) {
    if (isStatic()) {
        m_inertiaTensor = glm::mat3(0.0f);
        m_invInertiaTensor = glm::mat3(0.0f);
        return;
    }
    
    m_inertiaTensor = tensor;
    m_invInertiaTensor = inertia::calculateInverse(tensor);
}

glm::mat3 RigidBody::getWorldInverseInertiaTensor() const {
    if (isStatic()) {
        return glm::mat3(0.0f);
    }
    
    // Transform inverse inertia to world space: I_world^-1 = R * I_local^-1 * R^T
    return inertia::transformToWorldSpace(m_invInertiaTensor, m_rotation);
}

void RigidBody::calculateInertiaFromShape() {
    if (!m_collisionShape || isStatic()) {
        if (isStatic()) {
            m_inertiaTensor = glm::mat3(0.0f);
            m_invInertiaTensor = glm::mat3(0.0f);
        } else {
            // Default unit sphere inertia
            setInertiaTensor(inertia::calculateSphereTensor(m_mass, 1.0f));
        }
        return;
    }
    
    // Calculate inertia based on shape type
    glm::mat3 localInertia{1.0f};
    
    switch (m_collisionShape->getType()) {
        case collision::ShapeType::BOX: {
            auto* boxShape = static_cast<const collision::BoxShape*>(m_collisionShape.get());
            glm::vec3 dimensions = boxShape->getHalfExtents() * 2.0f;
            localInertia = inertia::calculateBoxTensor(m_mass, dimensions);
            break;
        }
        case collision::ShapeType::SPHERE: {
            auto* sphereShape = static_cast<const collision::SphereShape*>(m_collisionShape.get());
            float radius = sphereShape->getRadius();
            localInertia = inertia::calculateSphereTensor(m_mass, radius);
            break;
        }
        case collision::ShapeType::CYLINDER: {
            auto* cylinderShape = static_cast<const collision::CylinderShape*>(m_collisionShape.get());
            float radius = cylinderShape->getRadius();
            float height = cylinderShape->getHeight();
            localInertia = inertia::calculateCylinderTensor(m_mass, radius, height);
            break;
        }
        case collision::ShapeType::CAPSULE: {
            auto* capsuleShape = static_cast<const collision::CapsuleShape*>(m_collisionShape.get());
            float radius = capsuleShape->getRadius();
            float height = capsuleShape->getHeight();
            localInertia = inertia::calculateCapsuleTensor(m_mass, radius, height);
            break;
        }
        default:
            // Use bounding box approximation for complex shapes
            math::AABB bounds = getAABB();
            glm::vec3 dimensions = bounds.getSize();
            localInertia = inertia::calculateBoxTensor(m_mass, dimensions);
            break;
    }
    
    setInertiaTensor(localInertia);
}

// === MATERIAL PROPERTIES ===
float RigidBody::getRestitution() const {
    return m_material ? m_material->getRestitution() : 0.3f;
}

float RigidBody::getStaticFriction() const {
    return m_material ? m_material->getStaticFriction() : 0.6f;
}

float RigidBody::getDynamicFriction() const {
    return m_material ? m_material->getDynamicFriction() : 0.4f;
}

float RigidBody::getDensity() const {
    return m_material ? m_material->getDensity() : 1000.0f;
}

// === VELOCITY ===
void RigidBody::setLinearVelocity(const glm::vec3& velocity) {
    if (isStatic()) return;
    
    m_linearVelocity = math::clampLength(velocity, constants::MAX_LINEAR_VELOCITY);
    
    if (!math::isNearZero(m_linearVelocity)) {
        setAwake(true);
    }
}

void RigidBody::setAngularVelocity(const glm::vec3& velocity) {
    if (isStatic()) return;
    
    m_angularVelocity = math::clampLength(velocity, constants::MAX_ANGULAR_VELOCITY);
    
    if (!math::isNearZero(m_angularVelocity)) {
        setAwake(true);
    }
}

// === FORCES ===
void RigidBody::applyForce(const glm::vec3& force, const glm::vec3& relativePos) {
    if (isStatic() || !math::isFinite(force)) return;
    
    m_accumulatedForce += force;
    
    if (!math::isNearZero(relativePos)) {
        glm::vec3 torque = glm::cross(relativePos, force);
        m_accumulatedTorque += torque;
    }
    
    setAwake(true);
}

void RigidBody::applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos) {
    if (isStatic() || !math::isFinite(impulse)) return;
    
    // Apply linear impulse
    m_linearVelocity += impulse * m_invMass;
    
    // Apply angular impulse if not applied at center of mass
    if (!math::isNearZero(relativePos)) {
        glm::vec3 angularImpulse = glm::cross(relativePos, impulse);
        glm::mat3 worldInvInertia = getWorldInverseInertiaTensor();
        m_angularVelocity += worldInvInertia * angularImpulse;
    }
    
    // Clamp velocities
    setLinearVelocity(m_linearVelocity);
    setAngularVelocity(m_angularVelocity);
    
    setAwake(true);
}

void RigidBody::applyTorque(const glm::vec3& torque) {
    if (isStatic() || !math::isFinite(torque)) return;
    
    m_accumulatedTorque += torque;
    setAwake(true);
}

void RigidBody::clearForces() {
    m_accumulatedForce = glm::vec3(0.0f);
    m_accumulatedTorque = glm::vec3(0.0f);
}

void RigidBody::applyForceAtWorldPoint(const glm::vec3& force, const glm::vec3& worldPoint) {
    glm::vec3 relativePos = worldPoint - m_position;
    applyForce(force, relativePos);
}

void RigidBody::applyImpulseAtWorldPoint(const glm::vec3& impulse, const glm::vec3& worldPoint) {
    glm::vec3 relativePos = worldPoint - m_position;
    applyImpulse(impulse, relativePos);
}

// === COLLISION SHAPE ===
void RigidBody::setCollisionShape(std::shared_ptr<collision::CollisionShape> shape) {
    m_collisionShape = shape;
    calculateInertiaFromShape();
}

math::AABB RigidBody::getAABB() const {
    if (m_collisionShape) {
        return m_collisionShape->getAABB(m_position, m_rotation);
    }
    return math::AABB(m_position, glm::vec3(0.5f)); // Default 1x1x1 box
}

// === INTEGRATION ===
void RigidBody::integrate(float deltaTime) {
    if (isStatic() || !m_isAwake) return;
    
    validateState();
    Integrator::integratePhysics(this, deltaTime);
}

// === SLEEP/WAKE SYSTEM ===
void RigidBody::setAwake(bool awake) {
    if (isStatic()) {
        m_isAwake = false;
        return;
    }
    
    if (awake && !m_isAwake) {
        m_sleepTimer = 0.0f;
    }
    
    m_isAwake = awake;
    
    if (!awake) {
        m_linearVelocity = glm::vec3(0.0f);
        m_angularVelocity = glm::vec3(0.0f);
        clearForces();
    }
}

void RigidBody::updateSleepState(float deltaTime) {
    if (isStatic()) return;
    
    float kineticEnergy = getKineticEnergy();
    
    if (kineticEnergy < constants::SLEEP_LINEAR_THRESHOLD) {
        m_sleepTimer += deltaTime;
        if (m_sleepTimer > constants::SLEEP_TIMEOUT) {
            setAwake(false);
        }
    } else {
        m_sleepTimer = 0.0f;
        setAwake(true);
    }
}

// === ENERGY & MOMENTUM ===
float RigidBody::getKineticEnergy() const {
    if (isStatic()) return 0.0f;
    
    float linearKE = 0.5f * m_mass * math::lengthSquared(m_linearVelocity);
    
    // Calculate rotational kinetic energy: KE = 0.5 * ω^T * I * ω
    glm::mat3 worldInertia = inertia::transformToWorldSpace(m_inertiaTensor, m_rotation);
    glm::vec3 angularMomentum = worldInertia * m_angularVelocity;
    float angularKE = 0.5f * glm::dot(m_angularVelocity, angularMomentum);
    
    return linearKE + angularKE;
}

glm::vec3 RigidBody::getAngularMomentum() const {
    if (isStatic()) return glm::vec3(0.0f);
    
    glm::mat3 worldInertia = inertia::transformToWorldSpace(m_inertiaTensor, m_rotation);
    return worldInertia * m_angularVelocity;
}

// === COMPONENT SYNC ===
void RigidBody::updateTransformComponent() {
    if (m_component) {
        auto transform = m_component->getTransformComponent();
        if (transform) {
            // Update transform component from physics state
            transform->setPosition(m_position);
            transform->setRotation(m_rotation);
        }
    }
}

// === PRIVATE METHODS ===
void RigidBody::updateMassProperties() {
    if (isStatic()) {
        m_mass = 0.0f;
        m_invMass = 0.0f;
        m_inertiaTensor = glm::mat3(0.0f);
        m_invInertiaTensor = glm::mat3(0.0f);
    } else if (m_mass <= 0.0f) {
        // Auto-calculate mass from material and shape
        if (m_collisionShape && m_material) {
            math::AABB bounds = getAABB();
            float volume = bounds.getVolume();
            float density = m_material->getDensity();
            setMass(volume * density);
        } else {
            setMass(1.0f); // Default mass
        }
    }
    
    // Recalculate inertia
    calculateInertiaFromShape();
}

void RigidBody::validateState() {
    // Ensure all values are finite
    if (!math::isFinite(m_position)) {
        OHAO_LOG_ERROR("RigidBody position is not finite, resetting to origin");
        m_position = glm::vec3(0.0f);
    }
    
    if (!math::isFinite(m_linearVelocity)) {
        OHAO_LOG_ERROR("RigidBody linear velocity is not finite, resetting to zero");
        m_linearVelocity = glm::vec3(0.0f);
    }
    
    if (!math::isFinite(m_angularVelocity)) {
        OHAO_LOG_ERROR("RigidBody angular velocity is not finite, resetting to zero");
        m_angularVelocity = glm::vec3(0.0f);
    }
    
    // Ensure quaternion is normalized
    m_rotation = math::safeNormalize(m_rotation);
}

} // namespace dynamics
} // namespace physics
} // namespace ohao