#include "rigid_body.hpp"
#include "../component/physics_component.hpp"
#include "../component/transform_component.hpp"
#include "collision_shape.hpp"
#include "../../ui/components/console_widget.hpp"

namespace ohao {

RigidBody::RigidBody(PhysicsComponent* component) 
    : m_component(component) {
    // TODO: Initialize with component's transform
    // if (component && component->getTransform()) {
    //     auto transform = component->getTransform();
    //     m_position = transform->getPosition();
    //     m_rotation = transform->getRotation();
    // }
}

RigidBody::~RigidBody() {
    // TODO: Cleanup Bullet Physics rigid body
    // if (m_bulletBody) {
    //     delete m_bulletBody;
    // }
}

void RigidBody::setMass(float mass) {
    m_mass = mass;
    // TODO: Update Bullet Physics
    // updateBulletProperties();
}

float RigidBody::getMass() const {
    return m_mass;
}

void RigidBody::setRestitution(float restitution) {
    m_restitution = restitution;
    // TODO: Update Bullet Physics
    // if (m_bulletBody) {
    //     m_bulletBody->setRestitution(restitution);
    // }
}

float RigidBody::getRestitution() const {
    return m_restitution;
}

void RigidBody::setFriction(float friction) {
    m_friction = friction;
    // TODO: Update Bullet Physics
    // if (m_bulletBody) {
    //     m_bulletBody->setFriction(friction);
    // }
}

float RigidBody::getFriction() const {
    return m_friction;
}

void RigidBody::setLinearDamping(float damping) {
    m_linearDamping = damping;
    // TODO: Update Bullet Physics
    // if (m_bulletBody) {
    //     m_bulletBody->setDamping(damping, m_angularDamping);
    // }
}

float RigidBody::getLinearDamping() const {
    return m_linearDamping;
}

void RigidBody::setAngularDamping(float damping) {
    m_angularDamping = damping;
    // TODO: Update Bullet Physics
    // if (m_bulletBody) {
    //     m_bulletBody->setDamping(m_linearDamping, damping);
    // }
}

float RigidBody::getAngularDamping() const {
    return m_angularDamping;
}

void RigidBody::setPosition(const glm::vec3& position) {
  m_position = position;
}

glm::vec3 RigidBody::getPosition() const {
  return m_position;
}

void RigidBody::setRotation(const glm::quat& rotation) {
  m_rotation = rotation;
  // TODO: Update Bullet Physics
  // syncToBullet();
}

glm::quat RigidBody::getRotation() const {
  // TODO: Sync from Bullet Physics
  // syncFromBullet();
  return m_rotation;
}

void RigidBody::setTransform(const glm::vec3& position, const glm::quat& rotation) {
  m_position = position;
  m_rotation = rotation;
  // TODO: Update Bullet Physics
  // syncToBullet();
}

glm::mat4 RigidBody::getTransformMatrix() const {
  glm::mat4 translation = glm::translate(glm::mat4(1.0f), m_position);
  glm::mat4 rotationMat = glm::mat4_cast(m_rotation);
  return translation * rotationMat;
}

void RigidBody::setLinearVelocity(const glm::vec3& velocity) {
  m_linearVelocity = velocity;
  // TODO: Update Bullet Physics
  // if (m_bulletBody) {
  //     m_bulletBody->setLinearVelocity(btVector3(velocity.x, velocity.y, velocity.z));
  // }
}

glm::vec3 RigidBody::getLinearVelocity() const {
  return m_linearVelocity;
}

void RigidBody::setAngularVelocity(const glm::vec3& velocity) {
  m_angularVelocity = velocity;
  // TODO: Update Bullet Physics
  // if (m_bulletBody) {
  //     m_bulletBody->setAngularVelocity(btVector3(velocity.x, velocity.y, velocity.z));
  // }
}

glm::vec3 RigidBody::getAngularVelocity() const {
  // TODO: Get from Bullet Physics
  // if (m_bulletBody) {
  //     btVector3 vel = m_bulletBody->getAngularVelocity();
  //     return glm::vec3(vel.x(), vel.y(), vel.z());
  // }
  return m_angularVelocity;
}

void RigidBody::applyForce(const glm::vec3& force, const glm::vec3& relativePos) {
  m_accumulatedForce += force;
}

void RigidBody::applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos) {
  // TODO: Apply impulse using Bullet Physics
  // if (m_bulletBody) {
  //     btVector3 btImpulse(impulse.x, impulse.y, impulse.z);
  //     btVector3 btRelPos(relativePos.x, relativePos.y, relativePos.z);
  //     m_bulletBody->applyImpulse(btImpulse, btRelPos);
  // }
  OHAO_LOG_WARNING("Impulse application not implemented");
}

void RigidBody::applyTorque(const glm::vec3& torque) {
  // TODO: Apply torque using Bullet Physics
  // if (m_bulletBody) {
  //     btVector3 btTorque(torque.x, torque.y, torque.z);
  //     m_bulletBody->applyTorque(btTorque);
  // }
  OHAO_LOG_WARNING("Torque application not implemented");
}

void RigidBody::clearForces() {
  m_accumulatedForce = glm::vec3{0.0, 0.0f, 0.0f};
}

glm::vec3 RigidBody::getAccumulatedForce() const {
  return m_accumulatedForce;
}

void RigidBody::setCollisionShape(std::shared_ptr<CollisionShape> shape) {
  m_collisionShape = shape;
  // TODO: Update Bullet Physics rigid body
  // createBulletRigidBody();
}

std::shared_ptr<CollisionShape> RigidBody::getCollisionShape() const {
  return m_collisionShape;
}

void RigidBody::setType(RigidBodyType type) {
  m_type = type;
  // TODO: Update Bullet Physics
  // updateBulletProperties();
}

RigidBodyType RigidBody::getType() const {
  return m_type;
}

void RigidBody::activate() {
  // TODO: Activate using Bullet Physics
  // if (m_bulletBody) {
  //     m_bulletBody->activate();
  // }
}

void RigidBody::setActivationState(bool active) {
  // TODO: Set activation state using Bullet Physics
  // if (m_bulletBody) {
  //     if (active) {
  //         m_bulletBody->activate();
  //     } else {
  //         m_bulletBody->setActivationState(WANTS_DEACTIVATION);
  //     }
  // }
}

bool RigidBody::isActive() const {
  // TODO: Get activation state from Bullet Physics
  // if (m_bulletBody) {
  //     return m_bulletBody->isActive();
  // }
  return true;
}

PhysicsComponent* RigidBody::getComponent() const {
  return m_component;
}

void RigidBody::updateTransform() {
    if(m_component && m_component->getTransformComponent()){

        auto transform = m_component->getTransformComponent();
        if(transform){
           // sync physics position/rotation to transform component
            transform->setPosition(m_position);
            transform->setRotation(m_rotation);
        }
    }
}

} // namespace ohao
