#pragma once

#include <godot_cpp/classes/node3d.hpp>

namespace godot {

/**
 * OhaoPhysicsBody - Physics body using OHAO's physics engine
 *
 * This replaces Godot's RigidBody3D with our custom physics simulation.
 * Attach to any Node3D to give it physics behavior.
 */
class OhaoPhysicsBody : public Node3D {
    GDCLASS(OhaoPhysicsBody, Node3D)

public:
    enum BodyType {
        BODY_DYNAMIC = 0,
        BODY_STATIC = 1,
        BODY_KINEMATIC = 2
    };

    enum ShapeType {
        SHAPE_BOX = 0,
        SHAPE_SPHERE = 1,
        SHAPE_CAPSULE = 2,
        SHAPE_MESH = 3
    };

private:
    // Physics properties
    BodyType m_body_type = BODY_DYNAMIC;
    ShapeType m_shape_type = SHAPE_BOX;
    float m_mass = 1.0f;
    float m_friction = 0.5f;
    float m_restitution = 0.0f;
    Vector3 m_linear_velocity;
    Vector3 m_angular_velocity;
    bool m_gravity_enabled = true;

    // OHAO physics pointer
    void* m_rigid_body = nullptr;
    bool m_in_physics_world = false;

protected:
    static void _bind_methods();

public:
    OhaoPhysicsBody();
    ~OhaoPhysicsBody();

    void _ready() override;
    void _physics_process(double delta) override;

    // Body type
    void set_body_type(int type);
    int get_body_type() const { return m_body_type; }

    // Shape
    void set_shape_type(int type);
    int get_shape_type() const { return m_shape_type; }

    // Mass
    void set_mass(float mass);
    float get_mass() const { return m_mass; }

    // Friction
    void set_friction(float friction);
    float get_friction() const { return m_friction; }

    // Restitution (bounciness)
    void set_restitution(float restitution);
    float get_restitution() const { return m_restitution; }

    // Gravity
    void set_gravity_enabled(bool enabled);
    bool get_gravity_enabled() const { return m_gravity_enabled; }

    // Velocity
    void set_linear_velocity(Vector3 velocity);
    Vector3 get_linear_velocity() const { return m_linear_velocity; }

    void set_angular_velocity(Vector3 velocity);
    Vector3 get_angular_velocity() const { return m_angular_velocity; }

    // Forces
    void apply_force(Vector3 force, Vector3 position = Vector3());
    void apply_impulse(Vector3 impulse, Vector3 position = Vector3());
    void apply_torque(Vector3 torque);

    // Physics world integration
    void add_to_physics_world();
    void remove_from_physics_world();
    bool is_in_physics_world() const { return m_in_physics_world; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::OhaoPhysicsBody::BodyType);
VARIANT_ENUM_CAST(godot::OhaoPhysicsBody::ShapeType);
