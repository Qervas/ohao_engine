#include "ohao_physics_body.h"
#include "ohao_viewport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "physics/components/physics_component.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/world/physics_world.hpp"
#include "physics/collision/shapes/shape_factory.hpp"
#include "renderer/offscreen/offscreen_renderer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace godot {

void OhaoPhysicsBody::_bind_methods() {
    // Body type
    ClassDB::bind_method(D_METHOD("set_body_type", "type"), &OhaoPhysicsBody::set_body_type);
    ClassDB::bind_method(D_METHOD("get_body_type"), &OhaoPhysicsBody::get_body_type);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "body_type", PROPERTY_HINT_ENUM, "Dynamic,Static,Kinematic"),
                 "set_body_type", "get_body_type");

    // Shape type
    ClassDB::bind_method(D_METHOD("set_shape_type", "type"), &OhaoPhysicsBody::set_shape_type);
    ClassDB::bind_method(D_METHOD("get_shape_type"), &OhaoPhysicsBody::get_shape_type);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_type", PROPERTY_HINT_ENUM, "Box,Sphere,Capsule,Mesh"),
                 "set_shape_type", "get_shape_type");

    // Mass
    ClassDB::bind_method(D_METHOD("set_mass", "mass"), &OhaoPhysicsBody::set_mass);
    ClassDB::bind_method(D_METHOD("get_mass"), &OhaoPhysicsBody::get_mass);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "0.001,1000,0.01"),
                 "set_mass", "get_mass");

    // Friction
    ClassDB::bind_method(D_METHOD("set_friction", "friction"), &OhaoPhysicsBody::set_friction);
    ClassDB::bind_method(D_METHOD("get_friction"), &OhaoPhysicsBody::get_friction);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_friction", "get_friction");

    // Restitution
    ClassDB::bind_method(D_METHOD("set_restitution", "restitution"), &OhaoPhysicsBody::set_restitution);
    ClassDB::bind_method(D_METHOD("get_restitution"), &OhaoPhysicsBody::get_restitution);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "restitution", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_restitution", "get_restitution");

    // Gravity
    ClassDB::bind_method(D_METHOD("set_gravity_enabled", "enabled"), &OhaoPhysicsBody::set_gravity_enabled);
    ClassDB::bind_method(D_METHOD("get_gravity_enabled"), &OhaoPhysicsBody::get_gravity_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gravity_enabled"), "set_gravity_enabled", "get_gravity_enabled");

    // Velocity
    ClassDB::bind_method(D_METHOD("set_linear_velocity", "velocity"), &OhaoPhysicsBody::set_linear_velocity);
    ClassDB::bind_method(D_METHOD("get_linear_velocity"), &OhaoPhysicsBody::get_linear_velocity);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "linear_velocity"), "set_linear_velocity", "get_linear_velocity");

    ClassDB::bind_method(D_METHOD("set_angular_velocity", "velocity"), &OhaoPhysicsBody::set_angular_velocity);
    ClassDB::bind_method(D_METHOD("get_angular_velocity"), &OhaoPhysicsBody::get_angular_velocity);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "angular_velocity"), "set_angular_velocity", "get_angular_velocity");

    // Forces
    ClassDB::bind_method(D_METHOD("apply_force", "force", "position"), &OhaoPhysicsBody::apply_force, DEFVAL(Vector3()));
    ClassDB::bind_method(D_METHOD("apply_impulse", "impulse", "position"), &OhaoPhysicsBody::apply_impulse, DEFVAL(Vector3()));
    ClassDB::bind_method(D_METHOD("apply_torque", "torque"), &OhaoPhysicsBody::apply_torque);

    // Enums
    BIND_ENUM_CONSTANT(BODY_DYNAMIC);
    BIND_ENUM_CONSTANT(BODY_STATIC);
    BIND_ENUM_CONSTANT(BODY_KINEMATIC);

    BIND_ENUM_CONSTANT(SHAPE_BOX);
    BIND_ENUM_CONSTANT(SHAPE_SPHERE);
    BIND_ENUM_CONSTANT(SHAPE_CAPSULE);
    BIND_ENUM_CONSTANT(SHAPE_MESH);
}

OhaoPhysicsBody::OhaoPhysicsBody() {
}

OhaoPhysicsBody::~OhaoPhysicsBody() {
    remove_from_physics_world();
}

OhaoViewport* OhaoPhysicsBody::find_viewport() const {
    Node* current = get_parent();
    while (current) {
        OhaoViewport* viewport = Object::cast_to<OhaoViewport>(current);
        if (viewport) return viewport;
        current = current->get_parent();
    }
    return nullptr;
}

void OhaoPhysicsBody::_ready() {
    UtilityFunctions::print("[OHAO] PhysicsBody ready: ", get_name());
    add_to_physics_world();
}

void OhaoPhysicsBody::_physics_process(double delta) {
    if (!m_in_physics_world || !m_physics_comp) return;

    auto rigidBody = m_physics_comp->getRigidBody();
    if (!rigidBody) return;

    // Sync OHAO physics transform -> Godot Node3D transform
    // Use set_position() not set_global_position() — the Node3D may be
    // parented to a Control (OhaoViewport) with no 3D ancestor chain.
    glm::vec3 pos = rigidBody->getPosition();
    glm::quat rot = rigidBody->getRotation();

    set_position(Vector3(pos.x, pos.y, pos.z));
    set_quaternion(Quaternion(rot.x, rot.y, rot.z, rot.w));

    // Update cached velocity values
    glm::vec3 linVel = rigidBody->getLinearVelocity();
    glm::vec3 angVel = rigidBody->getAngularVelocity();
    m_linear_velocity = Vector3(linVel.x, linVel.y, linVel.z);
    m_angular_velocity = Vector3(angVel.x, angVel.y, angVel.z);
}

void OhaoPhysicsBody::set_body_type(int type) {
    m_body_type = static_cast<BodyType>(type);
    if (m_physics_comp) {
        ohao::physics::dynamics::RigidBodyType ohaoType;
        switch (m_body_type) {
            case BODY_STATIC:    ohaoType = ohao::physics::dynamics::RigidBodyType::STATIC; break;
            case BODY_KINEMATIC: ohaoType = ohao::physics::dynamics::RigidBodyType::KINEMATIC; break;
            default:             ohaoType = ohao::physics::dynamics::RigidBodyType::DYNAMIC; break;
        }
        m_physics_comp->setRigidBodyType(ohaoType);
    }
}

void OhaoPhysicsBody::set_shape_type(int type) {
    m_shape_type = static_cast<ShapeType>(type);
    if (m_physics_comp) {
        // Recreate collision shape with current dimensions
        Vector3 scale = get_scale();
        switch (m_shape_type) {
            case SHAPE_BOX:
                m_physics_comp->createBoxShape(glm::vec3(
                    scale.x * m_shape_extents.x,
                    scale.y * m_shape_extents.y,
                    scale.z * m_shape_extents.z));
                break;
            case SHAPE_SPHERE:
                m_physics_comp->createSphereShape(
                    std::max({scale.x, scale.y, scale.z}) * m_shape_radius);
                break;
            case SHAPE_CAPSULE:
                m_physics_comp->createCapsuleShape(
                    std::max(scale.x, scale.z) * m_shape_radius,
                    scale.y * m_shape_height);
                break;
            default:
                m_physics_comp->createBoxShape(glm::vec3(
                    scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f));
                break;
        }
    }
}

void OhaoPhysicsBody::set_mass(float mass) {
    m_mass = mass;
    if (m_physics_comp) {
        m_physics_comp->setMass(mass);
    }
}

void OhaoPhysicsBody::set_friction(float friction) {
    m_friction = friction;
    if (m_physics_comp) {
        m_physics_comp->setFriction(friction);
    }
}

void OhaoPhysicsBody::set_restitution(float restitution) {
    m_restitution = restitution;
    if (m_physics_comp) {
        m_physics_comp->setRestitution(restitution);
    }
}

void OhaoPhysicsBody::set_gravity_enabled(bool enabled) {
    m_gravity_enabled = enabled;
    if (m_physics_comp) {
        m_physics_comp->setGravityEnabled(enabled);
    }
}

void OhaoPhysicsBody::set_linear_velocity(Vector3 velocity) {
    m_linear_velocity = velocity;
    if (m_physics_comp) {
        m_physics_comp->setLinearVelocity(glm::vec3(velocity.x, velocity.y, velocity.z));
    }
}

void OhaoPhysicsBody::set_angular_velocity(Vector3 velocity) {
    m_angular_velocity = velocity;
    if (m_physics_comp) {
        m_physics_comp->setAngularVelocity(glm::vec3(velocity.x, velocity.y, velocity.z));
    }
}

void OhaoPhysicsBody::apply_force(Vector3 force, Vector3 position) {
    if (m_physics_comp) {
        m_physics_comp->applyForce(
            glm::vec3(force.x, force.y, force.z),
            glm::vec3(position.x, position.y, position.z));
    }
}

void OhaoPhysicsBody::apply_impulse(Vector3 impulse, Vector3 position) {
    if (m_physics_comp) {
        m_physics_comp->applyImpulse(
            glm::vec3(impulse.x, impulse.y, impulse.z),
            glm::vec3(position.x, position.y, position.z));
    }
}

void OhaoPhysicsBody::apply_torque(Vector3 torque) {
    if (m_physics_comp) {
        m_physics_comp->applyTorque(glm::vec3(torque.x, torque.y, torque.z));
    }
}

void OhaoPhysicsBody::add_to_physics_world() {
    if (m_in_physics_world) return;

    // Find the OhaoViewport to access the OHAO scene
    OhaoViewport* viewport = find_viewport();
    if (!viewport || !viewport->is_renderer_initialized()) {
        UtilityFunctions::print("[OHAO] PhysicsBody: No initialized OhaoViewport found in parent tree");
        return;
    }

    m_ohao_scene = viewport->get_ohao_scene();
    if (!m_ohao_scene) {
        UtilityFunctions::print("[OHAO] PhysicsBody: OHAO scene not available");
        return;
    }

    // Create an OHAO actor for this physics body
    String gdName = get_name();
    std::string actorName = std::string("phys_") + gdName.utf8().get_data();
    m_ohao_actor = m_ohao_scene->createActor(actorName);
    if (!m_ohao_actor) {
        UtilityFunctions::printerr("[OHAO] PhysicsBody: Failed to create actor");
        return;
    }

    // Set initial transform from Godot
    // Use get_position() not get_global_position() — when Node3D is parented
    // to a Control (OhaoViewport), get_global_position() returns (0,0,0)
    // because there's no 3D ancestor chain.
    Vector3 pos = get_position();
    Quaternion rot = get_quaternion();
    Vector3 scale = get_scale();

    auto transform = m_ohao_actor->getTransform();
    if (transform) {
        transform->setPosition(glm::vec3(pos.x, pos.y, pos.z));
        transform->setRotation(glm::quat(rot.w, rot.x, rot.y, rot.z));
        transform->setScale(glm::vec3(scale.x, scale.y, scale.z));
    }

    // Add PhysicsComponent to the actor
    m_physics_comp = m_ohao_actor->addComponent<ohao::PhysicsComponent>();
    if (!m_physics_comp) {
        UtilityFunctions::printerr("[OHAO] PhysicsBody: Failed to add PhysicsComponent");
        m_ohao_scene->removeActor(actorName);
        m_ohao_actor.reset();
        return;
    }

    // Connect transform component
    m_physics_comp->setTransformComponent(transform);

    // Set rigid body type
    ohao::physics::dynamics::RigidBodyType ohaoType;
    switch (m_body_type) {
        case BODY_STATIC:    ohaoType = ohao::physics::dynamics::RigidBodyType::STATIC; break;
        case BODY_KINEMATIC: ohaoType = ohao::physics::dynamics::RigidBodyType::KINEMATIC; break;
        default:             ohaoType = ohao::physics::dynamics::RigidBodyType::DYNAMIC; break;
    }
    m_physics_comp->setRigidBodyType(ohaoType);

    // Create collision shape based on shape_type
    switch (m_shape_type) {
        case SHAPE_BOX:
            m_physics_comp->createBoxShape(glm::vec3(
                scale.x * m_shape_extents.x,
                scale.y * m_shape_extents.y,
                scale.z * m_shape_extents.z));
            break;
        case SHAPE_SPHERE:
            m_physics_comp->createSphereShape(
                std::max({scale.x, scale.y, scale.z}) * m_shape_radius);
            break;
        case SHAPE_CAPSULE:
            m_physics_comp->createCapsuleShape(
                std::max(scale.x, scale.z) * m_shape_radius,
                scale.y * m_shape_height);
            break;
        default:
            m_physics_comp->createBoxShape(glm::vec3(
                scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f));
            break;
    }

    // Set physics properties
    m_physics_comp->setMass(m_mass);
    m_physics_comp->setFriction(m_friction);
    m_physics_comp->setRestitution(m_restitution);
    m_physics_comp->setGravityEnabled(m_gravity_enabled);

    // Register with the OHAO scene's physics world
    m_ohao_scene->onPhysicsComponentAdded(m_physics_comp.get());

    // Set initial velocities
    if (m_linear_velocity.length_squared() > 0.0f) {
        m_physics_comp->setLinearVelocity(glm::vec3(
            m_linear_velocity.x, m_linear_velocity.y, m_linear_velocity.z));
    }
    if (m_angular_velocity.length_squared() > 0.0f) {
        m_physics_comp->setAngularVelocity(glm::vec3(
            m_angular_velocity.x, m_angular_velocity.y, m_angular_velocity.z));
    }

    m_in_physics_world = true;
    UtilityFunctions::print("[OHAO] PhysicsBody '", get_name(), "' added to physics world (",
                            m_body_type == BODY_STATIC ? "static" :
                            m_body_type == BODY_KINEMATIC ? "kinematic" : "dynamic", ")");
}

void OhaoPhysicsBody::remove_from_physics_world() {
    if (!m_in_physics_world) return;

    if (m_ohao_scene && m_ohao_actor) {
        // Remove the OHAO actor from the scene
        m_ohao_scene->removeActor(m_ohao_actor->getName());
        m_ohao_actor = nullptr;
        m_physics_comp = nullptr;
    }

    m_ohao_scene = nullptr;
    m_in_physics_world = false;
}

} // namespace godot
