#include "ohao_physics_body.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// TODO: Include OHAO headers when linking is set up
// #include "physics/dynamics/rigid_body.hpp"
// #include "physics/world/physics_world.hpp"

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

void OhaoPhysicsBody::_ready() {
    UtilityFunctions::print("[OHAO] PhysicsBody ready: ", get_name());
    add_to_physics_world();
}

void OhaoPhysicsBody::_physics_process(double delta) {
    if (!m_in_physics_world || !m_rigid_body) {
        return;
    }

    // TODO: Sync transform from OHAO physics body
    // auto* body = static_cast<ohao::physics::dynamics::RigidBody*>(m_rigid_body);
    // glm::vec3 pos = body->getPosition();
    // glm::quat rot = body->getRotation();
    // set_global_position(Vector3(pos.x, pos.y, pos.z));
    // set_quaternion(Quaternion(rot.x, rot.y, rot.z, rot.w));
}

void OhaoPhysicsBody::set_body_type(int type) {
    m_body_type = static_cast<BodyType>(type);
    // TODO: Update OHAO rigid body type
}

void OhaoPhysicsBody::set_shape_type(int type) {
    m_shape_type = static_cast<ShapeType>(type);
    // TODO: Recreate collision shape
}

void OhaoPhysicsBody::set_mass(float mass) {
    m_mass = mass;
    // TODO: Update OHAO rigid body mass
}

void OhaoPhysicsBody::set_friction(float friction) {
    m_friction = friction;
    // TODO: Update OHAO material
}

void OhaoPhysicsBody::set_restitution(float restitution) {
    m_restitution = restitution;
    // TODO: Update OHAO material
}

void OhaoPhysicsBody::set_gravity_enabled(bool enabled) {
    m_gravity_enabled = enabled;
    // TODO: Update OHAO rigid body gravity flag
}

void OhaoPhysicsBody::set_linear_velocity(Vector3 velocity) {
    m_linear_velocity = velocity;
    // TODO: Update OHAO rigid body velocity
}

void OhaoPhysicsBody::set_angular_velocity(Vector3 velocity) {
    m_angular_velocity = velocity;
    // TODO: Update OHAO rigid body angular velocity
}

void OhaoPhysicsBody::apply_force(Vector3 force, Vector3 position) {
    // TODO: Apply force to OHAO rigid body
    // auto* body = static_cast<ohao::physics::dynamics::RigidBody*>(m_rigid_body);
    // body->applyForce(glm::vec3(force.x, force.y, force.z), glm::vec3(position.x, position.y, position.z));
}

void OhaoPhysicsBody::apply_impulse(Vector3 impulse, Vector3 position) {
    // TODO: Apply impulse to OHAO rigid body
}

void OhaoPhysicsBody::apply_torque(Vector3 torque) {
    // TODO: Apply torque to OHAO rigid body
}

void OhaoPhysicsBody::add_to_physics_world() {
    if (m_in_physics_world) {
        return;
    }

    UtilityFunctions::print("[OHAO] Adding body to physics world: ", get_name());

    // TODO: Create OHAO rigid body and add to physics world
    // m_rigid_body = physics_world->createRigidBody(...);

    m_in_physics_world = true;
}

void OhaoPhysicsBody::remove_from_physics_world() {
    if (!m_in_physics_world) {
        return;
    }

    // TODO: Remove from OHAO physics world
    // physics_world->removeRigidBody(m_rigid_body);

    m_rigid_body = nullptr;
    m_in_physics_world = false;
}

} // namespace godot
