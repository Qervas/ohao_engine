#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include "physics/world/physics_world.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"
#include "physics/collision/shapes/plane_shape.hpp"
#include "physics/material/physics_material.hpp"

namespace py = pybind11;
using namespace ohao::physics;

PYBIND11_MODULE(ohao_physics, m) {
    m.doc() = "OHAO Physics Engine - Python Bindings for Testing & AI";

    // ===== glm::vec3 binding =====
    py::class_<glm::vec3>(m, "Vec3")
        .def(py::init<float, float, float>())
        .def(py::init<>())
        .def_readwrite("x", &glm::vec3::x)
        .def_readwrite("y", &glm::vec3::y)
        .def_readwrite("z", &glm::vec3::z)
        .def("__repr__", [](const glm::vec3& v) {
            return "Vec3(" + std::to_string(v.x) + ", " +
                   std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
        })
        .def("length", [](const glm::vec3& v) { return glm::length(v); })
        .def("length_squared", [](const glm::vec3& v) { return glm::dot(v, v); })
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(py::self * float())
        .def(float() * py::self);

    // ===== PhysicsWorldConfig =====
    py::class_<PhysicsWorldConfig>(m, "PhysicsWorldConfig")
        .def(py::init<>())
        .def_readwrite("gravity", &PhysicsWorldConfig::gravity)
        .def_readwrite("time_step", &PhysicsWorldConfig::timeStep)
        .def_readwrite("max_sub_steps", &PhysicsWorldConfig::maxSubSteps)
        .def_readwrite("enable_sleeping", &PhysicsWorldConfig::enableSleeping);

    // ===== SimulationState =====
    py::enum_<SimulationState>(m, "SimulationState")
        .value("STOPPED", SimulationState::STOPPED)
        .value("RUNNING", SimulationState::RUNNING)
        .value("PAUSED", SimulationState::PAUSED)
        .export_values();

    // ===== RigidBodyType =====
    py::enum_<dynamics::RigidBodyType>(m, "RigidBodyType")
        .value("STATIC", dynamics::RigidBodyType::STATIC)
        .value("DYNAMIC", dynamics::RigidBodyType::DYNAMIC)
        .value("KINEMATIC", dynamics::RigidBodyType::KINEMATIC)
        .export_values();

    // ===== RigidBody =====
    py::class_<dynamics::RigidBody, std::shared_ptr<dynamics::RigidBody>>(m, "RigidBody")
        .def("set_position", &dynamics::RigidBody::setPosition)
        .def("get_position", &dynamics::RigidBody::getPosition)
        .def("set_velocity", &dynamics::RigidBody::setLinearVelocity)
        .def("get_velocity", &dynamics::RigidBody::getLinearVelocity)
        .def("set_mass", &dynamics::RigidBody::setMass)
        .def("get_mass", &dynamics::RigidBody::getMass)
        .def("get_restitution", &dynamics::RigidBody::getRestitution)
        .def("set_type", &dynamics::RigidBody::setType)
        .def("is_static", &dynamics::RigidBody::isStatic)
        .def("apply_force", &dynamics::RigidBody::applyForce)
        .def("apply_impulse", &dynamics::RigidBody::applyImpulse)
        // Helper methods for material properties (creates new material if needed)
        .def("set_restitution", [](dynamics::RigidBody& body, float restitution) {
            auto mat = body.getPhysicsMaterial();
            if (!mat) {
                mat = std::make_shared<PhysicsMaterial>("TestMaterial");
                body.setPhysicsMaterial(mat);
            }
            mat->setRestitution(restitution);
        })
        .def("set_friction", [](dynamics::RigidBody& body, float friction) {
            auto mat = body.getPhysicsMaterial();
            if (!mat) {
                mat = std::make_shared<PhysicsMaterial>("TestMaterial");
                body.setPhysicsMaterial(mat);
            }
            mat->setStaticFriction(friction);
            mat->setDynamicFriction(friction * 0.8f);  // Dynamic is typically lower
        })
        .def_property_readonly("kinetic_energy", [](const dynamics::RigidBody& body) -> float {
            const glm::vec3 v = body.getLinearVelocity();
            const float v_squared = glm::dot(v, v);
            return 0.5f * body.getMass() * v_squared;
        })
        .def_property_readonly("momentum", [](const dynamics::RigidBody& body) -> glm::vec3 {
            return body.getLinearVelocity() * body.getMass();
        });

    // ===== CollisionShape (base) =====
    py::class_<collision::CollisionShape, std::shared_ptr<collision::CollisionShape>>(m, "CollisionShape");

    // ===== BoxShape =====
    py::class_<collision::BoxShape, collision::CollisionShape, std::shared_ptr<collision::BoxShape>>(m, "BoxShape")
        .def(py::init<const glm::vec3&>())
        .def("get_half_extents", &collision::BoxShape::getHalfExtents);

    // ===== SphereShape =====
    py::class_<collision::SphereShape, collision::CollisionShape, std::shared_ptr<collision::SphereShape>>(m, "SphereShape")
        .def(py::init<float>())
        .def("get_radius", &collision::SphereShape::getRadius);

    // ===== PlaneShape =====
    py::class_<collision::PlaneShape, collision::CollisionShape, std::shared_ptr<collision::PlaneShape>>(m, "PlaneShape")
        .def(py::init<const glm::vec3&, float>());

    // ===== PhysicsWorld =====
    py::class_<PhysicsWorld>(m, "PhysicsWorld")
        .def(py::init<>())
        .def(py::init<const PhysicsWorldConfig&>())
        .def("step", &PhysicsWorld::step)
        .def("start", [](PhysicsWorld& world) {
            world.setSimulationState(SimulationState::RUNNING);
        })
        .def("stop", [](PhysicsWorld& world) {
            world.setSimulationState(SimulationState::STOPPED);
        })
        .def("pause", &PhysicsWorld::pause)
        .def("reset", &PhysicsWorld::reset)
        .def("set_gravity", &PhysicsWorld::setGravity)
        .def("get_stats", &PhysicsWorld::getStats)
        .def_property_readonly("total_energy", [](PhysicsWorld& world) {
            // Calculate total system energy
            float total = 0.0f;
            auto stats = world.getStats();
            // TODO: Iterate through bodies and calculate KE + PE
            return total;
        })
        .def("create_rigid_body_with_box",
            [](PhysicsWorld& world, const glm::vec3& halfExtents,
               const glm::vec3& position, float mass) {
                // Create a rigid body with box shape
                auto body = std::make_shared<dynamics::RigidBody>(nullptr);
                body->setMass(mass);
                body->setPosition(position);
                body->setType(mass > 0 ? dynamics::RigidBodyType::DYNAMIC
                                       : dynamics::RigidBodyType::STATIC);

                auto shape = std::make_shared<collision::BoxShape>(halfExtents);
                body->setCollisionShape(shape);

                // Register body with world for simulation
                world.addRigidBodyForTesting(body);

                return body;
            },
            py::arg("half_extents"),
            py::arg("position") = glm::vec3(0, 0, 0),
            py::arg("mass") = 1.0f
        )
        .def("create_rigid_body_with_sphere",
            [](PhysicsWorld& world, float radius,
               const glm::vec3& position, float mass) {
                auto body = std::make_shared<dynamics::RigidBody>(nullptr);
                body->setMass(mass);
                body->setPosition(position);
                body->setType(mass > 0 ? dynamics::RigidBodyType::DYNAMIC
                                       : dynamics::RigidBodyType::STATIC);

                auto shape = std::make_shared<collision::SphereShape>(radius);
                body->setCollisionShape(shape);

                // Register body with world for simulation
                world.addRigidBodyForTesting(body);

                return body;
            },
            py::arg("radius"),
            py::arg("position") = glm::vec3(0, 0, 0),
            py::arg("mass") = 1.0f
        );

    // ===== Helper functions =====
    m.def("calculate_kinetic_energy", [](const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
        float total = 0.0f;
        for (const auto& body : bodies) {
            if (!body) continue;
            float v_sq = glm::dot(body->getLinearVelocity(), body->getLinearVelocity());
            total += 0.5f * body->getMass() * v_sq;
        }
        return total;
    });

    m.def("calculate_potential_energy", [](const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies,
                                            const glm::vec3& gravity) {
        float total = 0.0f;
        for (const auto& body : bodies) {
            if (!body) continue;
            total += body->getMass() * std::abs(gravity.y) * body->getPosition().y;
        }
        return total;
    });

    m.def("calculate_total_momentum", [](const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
        glm::vec3 total(0, 0, 0);
        for (const auto& body : bodies) {
            if (!body) continue;
            total += body->getLinearVelocity() * body->getMass();
        }
        return total;
    });
}
