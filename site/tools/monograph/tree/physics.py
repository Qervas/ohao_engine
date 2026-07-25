"""Tree units: 11 · Physics."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'physics',
    "title": '11 · Physics',
    "hub": 'physics.html',
    "children": [
        page(
            'backend',
            'Backend plugin',
            'IPhysicsBackend, factory, null fallback, Jolt.',
            files=['ohao/physics/backend/physics_backend.hpp', 'ohao/physics/backend/backend_factory.cpp', 'ohao/physics/backend/jolt/jolt_backend.hpp', 'ohao/physics/backend/jolt/jolt_backend.cpp', 'ohao/physics/backend/jolt/jolt_helpers.hpp', 'ohao/physics/physics_module.hpp'],
            design=['IPhysicsBackend abstracts create/step/destroy; factory picks Jolt or null.', 'jolt_helpers convert OHAO shapes/math into Jolt types without leaking Jolt into scene headers.'],
        ),
        page(
            'world',
            'PhysicsWorld',
            'Config, state machine, step, body registry.',
            files=['ohao/physics/world/physics_world.hpp', 'ohao/physics/world/physics_world.cpp', 'ohao/physics/world/physics_settings.hpp', 'ohao/physics/world/profile_manager.hpp', 'ohao/physics/world/profile_manager.cpp', 'ohao/physics/world/simulation_profile.hpp', 'ohao/physics/world/simulation_profile.cpp'],
            design=['PhysicsWorldConfig: gravity, dt, substeps, sleeping, CCD, body caps.', 'SimulationState: STOPPED | RUNNING | PAUSED | STEPPING.', 'ProfileManager + SimulationProfile tune quality/perf presets.'],
            workflow=['construct with config', 'add bodies / register forces', 'step(dt)', 'sync transforms to components'],
        ),
        page(
            'bodies',
            'Bodies & shapes',
            'RigidBody, all shape types, factory, PhysicsComponent.',
            files=['ohao/physics/dynamics/rigid_body.hpp', 'ohao/physics/dynamics/rigid_body.cpp', 'ohao/physics/collision/shapes/collision_shape.hpp', 'ohao/physics/collision/shapes/shape_factory.hpp', 'ohao/physics/collision/shapes/box_shape.hpp', 'ohao/physics/collision/shapes/sphere_shape.hpp', 'ohao/physics/collision/shapes/capsule_shape.hpp', 'ohao/physics/collision/shapes/cylinder_shape.hpp', 'ohao/physics/collision/shapes/plane_shape.hpp', 'ohao/physics/collision/shapes/triangle_mesh_shape.hpp', 'ohao/physics/components/physics_component.hpp', 'ohao/physics/components/physics_component.cpp'],
            design=['Shape hierarchy + ShapeFactory for box/sphere/capsule/cylinder/plane/triangle mesh.', 'PhysicsComponent binds a body to an Actor; Scene owns the world.'],
        ),
        page(
            'forces',
            'Force generators',
            'Registry, gravity, spring, drag, volumes, fields, presets.',
            files=['ohao/physics/forces/force_registry.hpp', 'ohao/physics/forces/force_registry.cpp', 'ohao/physics/forces/forces.hpp', 'ohao/physics/forces/force_generator.hpp', 'ohao/physics/forces/force_generator.cpp', 'ohao/physics/forces/gravity_force.hpp', 'ohao/physics/forces/gravity_force.cpp', 'ohao/physics/forces/spring_force.hpp', 'ohao/physics/forces/spring_force.cpp', 'ohao/physics/forces/drag_force.hpp', 'ohao/physics/forces/drag_force.cpp', 'ohao/physics/forces/force_volume.hpp', 'ohao/physics/forces/force_volume.cpp', 'ohao/physics/forces/field_force.hpp', 'ohao/physics/forces/field_force.cpp', 'ohao/physics/forces/environmental_force.hpp', 'ohao/physics/forces/environmental_force.cpp', 'ohao/physics/forces/force_presets.cpp', 'ohao/physics/examples/force_system_example.hpp'],
            design=['ForceRegistry maps generators → bodies each step.', 'Volumes/fields for wind, attractors; presets for common setups.', 'force_system_example.hpp is a documented usage sketch, not a binary.'],
        ),
        page(
            'materials-phys',
            'Physics materials',
            'Friction/restitution style materials.',
            files=['ohao/physics/material/physics_material.hpp', 'ohao/physics/material/physics_material.cpp'],
        ),
        page(
            'math-constants',
            'Physics math & constants',
            'Shared scalars, clamp helpers, math utils.',
            files=['ohao/physics/common/physics_constants.hpp', 'ohao/physics/common/physics_constants.cpp', 'ohao/physics/utils/physics_math.hpp', 'ohao/physics/utils/physics_math.cpp'],
        ),
        page(
            'debug',
            'Force debugger',
            'Debug visualization hooks.',
            files=['ohao/physics/debug/force_debugger.hpp', 'ohao/physics/debug/force_debugger.cpp'],
        ),
    ],
}
