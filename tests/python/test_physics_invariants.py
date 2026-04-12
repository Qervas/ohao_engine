"""
Physics Invariants Tests - Self-Validating Based on Physical Laws

These tests verify correctness by checking that fundamental physics laws hold.
No reference data needed - the laws of physics are the ground truth.
"""

import pytest
import numpy as np
try:
    import ohao_physics as physics
except ImportError:
    pytest.skip("ohao_physics module not built yet", allow_module_level=True)


class TestEnergyConservation:
    """Energy must be conserved (or decrease with dissipation)"""

    def test_free_fall_energy_conservation(self):
        """In free fall, KE + PE should remain constant (no air resistance)"""
        world = physics.PhysicsWorld()
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, -9.81, 0)
        world = physics.PhysicsWorld(config)

        # Create falling ball
        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 10, 0),
            mass=1.0
        )

        bodies = [ball]
        gravity = physics.Vec3(0, -9.81, 0)

        # Initial energy
        initial_ke = physics.calculate_kinetic_energy(bodies)
        initial_pe = physics.calculate_potential_energy(bodies, gravity)
        initial_total = initial_ke + initial_pe

        energies = []
        for frame in range(120):  # 2 seconds
            world.step(1.0 / 60.0)

            ke = physics.calculate_kinetic_energy(bodies)
            pe = physics.calculate_potential_energy(bodies, gravity)
            total = ke + pe
            energies.append(total)

        # Energy should remain constant (within numerical tolerance)
        for energy in energies:
            assert abs(energy - initial_total) < 0.1, \
                f"Energy not conserved! Initial: {initial_total:.2f}, Current: {energy:.2f}"

    def test_elastic_collision_energy_conservation(self):
        """In elastic collision, total KE should be conserved"""
        world = physics.PhysicsWorld()
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)  # No gravity
        world = physics.PhysicsWorld(config)

        # Two balls colliding head-on
        ball1 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(-5, 0, 0),
            mass=1.0
        )
        ball1.set_velocity(physics.Vec3(10, 0, 0))
        ball1.set_restitution(1.0)  # Perfectly elastic

        ball2 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(5, 0, 0),
            mass=1.0
        )
        ball2.set_velocity(physics.Vec3(-10, 0, 0))
        ball2.set_restitution(1.0)

        bodies = [ball1, ball2]
        initial_ke = physics.calculate_kinetic_energy(bodies)

        # Simulate until collision and beyond
        for _ in range(200):
            world.step(1.0 / 60.0)

        final_ke = physics.calculate_kinetic_energy(bodies)

        # Kinetic energy should be conserved in elastic collision
        assert abs(final_ke - initial_ke) < 0.1, \
            f"KE not conserved in elastic collision! Before: {initial_ke:.2f}, After: {final_ke:.2f}"

    def test_energy_decreases_with_friction(self):
        """With friction, total energy should monotonically decrease"""
        world = physics.PhysicsWorld()
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, -9.81, 0)
        world = physics.PhysicsWorld(config)

        # Ball sliding on ground with friction
        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 0.5, 0),
            mass=1.0
        )
        ball.set_velocity(physics.Vec3(10, 0, 0))
        ball.set_friction(0.5)

        ground = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(50, 0.5, 50),
            position=physics.Vec3(0, -0.5, 0),
            mass=0.0  # Static
        )
        ground.set_friction(0.5)

        bodies = [ball]
        gravity = physics.Vec3(0, -9.81, 0)

        energies = []
        for _ in range(300):  # 5 seconds
            world.step(1.0 / 60.0)
            ke = physics.calculate_kinetic_energy(bodies)
            pe = physics.calculate_potential_energy(bodies, gravity)
            energies.append(ke + pe)

        # Energy should never increase
        for i in range(1, len(energies)):
            assert energies[i] <= energies[i-1] + 0.01, \
                f"Energy increased with friction! Frame {i}: {energies[i]:.2f} > {energies[i-1]:.2f}"


class TestMomentumConservation:
    """Linear momentum must be conserved in isolated systems"""

    def test_elastic_collision_momentum_conservation(self):
        """Momentum conserved in elastic collision"""
        world = physics.PhysicsWorld()
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)  # No gravity
        world = physics.PhysicsWorld(config)

        # Two balls with different masses
        ball1 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(-5, 0, 0),
            mass=2.0
        )
        ball1.set_velocity(physics.Vec3(5, 0, 0))

        ball2 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(5, 0, 0),
            mass=1.0
        )
        ball2.set_velocity(physics.Vec3(-3, 0, 0))

        bodies = [ball1, ball2]
        initial_momentum = physics.calculate_total_momentum(bodies)

        # Simulate collision
        for _ in range(200):
            world.step(1.0 / 60.0)

        final_momentum = physics.calculate_total_momentum(bodies)

        # Momentum should be conserved (vector comparison)
        diff = physics.Vec3(
            final_momentum.x - initial_momentum.x,
            final_momentum.y - initial_momentum.y,
            final_momentum.z - initial_momentum.z
        )
        assert diff.length() < 0.1, \
            f"Momentum not conserved! Initial: {initial_momentum}, Final: {final_momentum}"


class TestNoPenetration:
    """Objects should never interpenetrate significantly"""

    def test_box_on_ground_no_penetration(self):
        """Box resting on ground should not penetrate"""
        world = physics.PhysicsWorld()

        box = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(0.5, 0.5, 0.5),
            position=physics.Vec3(0, 5, 0),
            mass=1.0
        )

        ground = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(10, 0.5, 10),
            position=physics.Vec3(0, -0.5, 0),
            mass=0.0  # Static
        )

        # Let box fall and settle
        for _ in range(300):  # 5 seconds
            world.step(1.0 / 60.0)

            # Check penetration (box bottom should be >= ground top)
            box_bottom = box.get_position().y - 0.5
            ground_top = ground.get_position().y + 0.5

            penetration = ground_top - box_bottom
            assert penetration < 0.05, \
                f"Box penetrating ground by {penetration:.4f}m! (tolerance: 0.05m)"

    def test_stacked_boxes_no_penetration(self):
        """Stacked boxes should not penetrate each other"""
        world = physics.PhysicsWorld()

        boxes = []
        for i in range(5):
            box = world.create_rigid_body_with_box(
                half_extents=physics.Vec3(0.5, 0.5, 0.5),
                position=physics.Vec3(0, i * 1.0 + 0.5, 0),
                mass=1.0
            )
            boxes.append(box)

        # Simulate
        for _ in range(300):
            world.step(1.0 / 60.0)

            # Check penetration between adjacent boxes
            for i in range(len(boxes) - 1):
                box_top = boxes[i].get_position().y + 0.5
                box_bottom = boxes[i+1].get_position().y - 0.5

                gap = box_bottom - box_top
                assert gap > -0.05, \
                    f"Boxes {i} and {i+1} penetrating by {-gap:.4f}m!"


class TestStability:
    """Stable configurations should remain stable"""

    def test_object_at_rest_stays_at_rest(self):
        """Newton's first law: Object at rest stays at rest"""
        world = physics.PhysicsWorld()
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)  # No forces
        world = physics.PhysicsWorld(config)

        box = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(0.5, 0.5, 0.5),
            position=physics.Vec3(0, 0, 0),
            mass=1.0
        )
        box.set_velocity(physics.Vec3(0, 0, 0))

        initial_position = box.get_position()

        # Simulate 5 seconds
        for _ in range(300):
            world.step(1.0 / 60.0)

        final_position = box.get_position()

        # Should not have moved
        diff = physics.Vec3(
            final_position.x - initial_position.x,
            final_position.y - initial_position.y,
            final_position.z - initial_position.z
        )
        assert diff.length() < 0.001, \
            f"Object moved {diff.length():.6f}m without forces!"

    def test_tower_remains_stable(self):
        """Stack of boxes should not jitter or collapse"""
        world = physics.PhysicsWorld()

        # Ground
        ground = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(10, 0.5, 10),
            position=physics.Vec3(0, -0.5, 0),
            mass=0.0
        )

        # Stack 5 boxes
        boxes = []
        for i in range(5):
            box = world.create_rigid_body_with_box(
                half_extents=physics.Vec3(0.5, 0.5, 0.5),
                position=physics.Vec3(0, i * 1.0 + 0.5, 0),
                mass=1.0
            )
            boxes.append(box)

        # Let settle for 2 seconds
        for _ in range(120):
            world.step(1.0 / 60.0)

        # Record positions after settling
        settled_positions = [box.get_position().y for box in boxes]

        # Simulate 3 more seconds - should remain stable
        max_drift = 0.0
        for _ in range(180):
            world.step(1.0 / 60.0)

            for i, box in enumerate(boxes):
                drift = abs(box.get_position().y - settled_positions[i])
                max_drift = max(max_drift, drift)

        # Boxes should not drift significantly
        assert max_drift < 0.1, \
            f"Tower unstable! Max drift: {max_drift:.4f}m (tolerance: 0.1m)"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
