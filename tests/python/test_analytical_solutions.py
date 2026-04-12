"""
Analytical Solutions Tests - Compare Against Known Mathematical Solutions

These tests verify correctness by comparing simulation results against
exact analytical solutions from physics equations.
"""

import pytest
import numpy as np
import math
try:
    import ohao_physics as physics
except ImportError:
    pytest.skip("ohao_physics module not built yet", allow_module_level=True)


class TestFreeFall:
    """Free fall motion: h(t) = h0 - 0.5*g*t^2, v(t) = -g*t"""

    def test_free_fall_position(self):
        """Position should match kinematic equation"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, -9.81, 0)
        world = physics.PhysicsWorld(config)
        world.start()  # Start simulation!

        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 100, 0),
            mass=1.0
        )

        g = 9.81
        h0 = 100.0

        max_error = 0.0
        for frame in range(120):  # 2 seconds
            world.step(1.0 / 60.0)
            t = (frame + 1) / 60.0  # Time AFTER this step

            # Analytical solution: h = h0 - 0.5*g*t^2
            expected_y = h0 - 0.5 * g * t * t
            actual_y = ball.get_position().y

            error = abs(actual_y - expected_y)
            max_error = max(max_error, error)

            assert error < 0.1, \
                f"At t={t:.2f}s: position error {error:.4f}m " \
                f"(expected: {expected_y:.2f}, actual: {actual_y:.2f})"

        print(f"✓ Free fall position: max error = {max_error:.4f}m")

    def test_free_fall_velocity(self):
        """Velocity should match v = -g*t"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, -9.81, 0)
        world = physics.PhysicsWorld(config)
        world.start()

        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 100, 0),
            mass=1.0
        )

        g = 9.81

        max_error = 0.0
        for frame in range(120):
            world.step(1.0 / 60.0)
            t = (frame + 1) / 60.0  # Time AFTER this step

            # Analytical solution: v = -g*t
            expected_v = -g * t
            actual_v = ball.get_velocity().y

            error = abs(actual_v - expected_v)
            max_error = max(max_error, error)

            assert error < 0.5, \
                f"At t={t:.2f}s: velocity error {error:.4f}m/s " \
                f"(expected: {expected_v:.2f}, actual: {actual_v:.2f})"

        print(f"✓ Free fall velocity: max error = {max_error:.4f}m/s")


class TestProjectileMotion:
    """Projectile motion with known trajectory"""

    def test_projectile_range(self):
        """Horizontal range: R = (v0^2 * sin(2θ)) / g"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, -9.81, 0)
        world = physics.PhysicsWorld(config)
        world.start()

        # Launch at 45 degrees
        v0 = 20.0  # m/s
        angle = math.pi / 4  # 45 degrees
        vx = v0 * math.cos(angle)
        vy = v0 * math.sin(angle)

        projectile = world.create_rigid_body_with_sphere(
            radius=0.1,
            position=physics.Vec3(0, 0, 0),
            mass=1.0
        )
        projectile.set_velocity(physics.Vec3(vx, vy, 0))

        # Simulate until it lands
        for _ in range(300):
            world.step(1.0 / 60.0)
            if projectile.get_position().y < 0:
                break

        # Theoretical range: R = v0^2 * sin(2θ) / g
        g = 9.81
        expected_range = (v0 * v0 * math.sin(2 * angle)) / g
        actual_range = projectile.get_position().x

        error = abs(actual_range - expected_range)
        assert error < 1.0, \
            f"Range error: {error:.2f}m (expected: {expected_range:.2f}, actual: {actual_range:.2f})"

        print(f"✓ Projectile range: error = {error:.2f}m")


class TestElasticCollision:
    """Elastic collision: exact velocity exchange formulas"""

    def test_equal_mass_head_on_collision(self):
        """Equal mass balls exchange velocities in head-on elastic collision"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)
        world = physics.PhysicsWorld(config)
        world.start()

        ball1 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(-5, 0, 0),
            mass=1.0
        )
        ball1.set_velocity(physics.Vec3(10, 0, 0))
        ball1.set_restitution(1.0)

        ball2 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(5, 0, 0),
            mass=1.0
        )
        ball2.set_velocity(physics.Vec3(0, 0, 0))
        ball2.set_restitution(1.0)

        initial_v1 = 10.0
        initial_v2 = 0.0

        # Simulate until collision and beyond
        for _ in range(200):
            world.step(1.0 / 60.0)

        # After collision, velocities should have exchanged
        final_v1 = ball1.get_velocity().x
        final_v2 = ball2.get_velocity().x

        # Theoretical: v1' = 0, v2' = 10
        assert abs(final_v1 - initial_v2) < 0.5, \
            f"Ball1 velocity wrong: {final_v1:.2f} (expected: {initial_v2:.2f})"
        assert abs(final_v2 - initial_v1) < 0.5, \
            f"Ball2 velocity wrong: {final_v2:.2f} (expected: {initial_v1:.2f})"

        print(f"✓ Equal mass collision: v1={final_v1:.2f}, v2={final_v2:.2f}")

    def test_elastic_collision_formula(self):
        """v1' = ((m1-m2)*v1 + 2*m2*v2)/(m1+m2)"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)
        world = physics.PhysicsWorld(config)
        world.start()

        m1, m2 = 2.0, 1.0
        v1, v2 = 5.0, -3.0

        ball1 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(-5, 0, 0),
            mass=m1
        )
        ball1.set_velocity(physics.Vec3(v1, 0, 0))
        ball1.set_restitution(1.0)

        ball2 = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(5, 0, 0),
            mass=m2
        )
        ball2.set_velocity(physics.Vec3(v2, 0, 0))
        ball2.set_restitution(1.0)

        # Analytical solution
        expected_v1 = ((m1 - m2) * v1 + 2 * m2 * v2) / (m1 + m2)
        expected_v2 = ((m2 - m1) * v2 + 2 * m1 * v1) / (m1 + m2)

        # Simulate
        for _ in range(200):
            world.step(1.0 / 60.0)

        actual_v1 = ball1.get_velocity().x
        actual_v2 = ball2.get_velocity().x

        error1 = abs(actual_v1 - expected_v1)
        error2 = abs(actual_v2 - expected_v2)

        assert error1 < 0.5, \
            f"Ball1 velocity error: {error1:.2f} (expected: {expected_v1:.2f}, actual: {actual_v1:.2f})"
        assert error2 < 0.5, \
            f"Ball2 velocity error: {error2:.2f} (expected: {expected_v2:.2f}, actual: {actual_v2:.2f})"

        print(f"✓ Elastic collision formula: errors = {error1:.2f}, {error2:.2f}")


class TestPendulum:
    """Simple pendulum: T = 2π√(L/g)"""

    def test_pendulum_period(self):
        """Period should match theoretical formula for small angles"""
        pytest.skip("Requires joint constraints (TODO)")


class TestSpring:
    """Spring oscillation: x(t) = A*cos(ωt), ω = √(k/m)"""

    def test_spring_frequency(self):
        """Oscillation frequency should match ω = √(k/m)"""
        pytest.skip("Requires spring constraints (TODO)")


class TestRestitution:
    """Coefficient of restitution tests"""

    def test_perfectly_elastic_bounce(self):
        """Ball with e=1.0 should bounce back to original height"""
        world = physics.PhysicsWorld()
        world.start()

        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 10, 0),
            mass=1.0
        )
        ball.set_restitution(1.0)  # Perfectly elastic

        ground = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(10, 0.5, 10),
            position=physics.Vec3(0, -0.5, 0),
            mass=0.0
        )
        ground.set_restitution(1.0)

        initial_height = 10.0

        # Drop and wait for bounce
        for _ in range(200):
            world.step(1.0 / 60.0)

        # Track maximum height after bounce
        max_height_after_bounce = 0.0
        for _ in range(100):
            world.step(1.0 / 60.0)
            max_height_after_bounce = max(max_height_after_bounce, ball.get_position().y)

        # Should bounce back to nearly same height (some energy loss is acceptable)
        height_ratio = max_height_after_bounce / initial_height
        assert height_ratio > 0.9, \
            f"Ball only bounced to {height_ratio*100:.1f}% of original height (expected: >90%)"

        print(f"✓ Elastic bounce: {height_ratio*100:.1f}% height recovery")

    def test_restitution_coefficient(self):
        """v_separation = -e * v_approach"""
        config = physics.PhysicsWorldConfig()
        config.gravity = physics.Vec3(0, 0, 0)
        world = physics.PhysicsWorld(config)
        world.start()

        e = 0.7  # Coefficient of restitution

        ball = world.create_rigid_body_with_sphere(
            radius=0.5,
            position=physics.Vec3(0, 5, 0),
            mass=1.0
        )
        ball.set_velocity(physics.Vec3(0, -10, 0))
        ball.set_restitution(e)

        wall = world.create_rigid_body_with_box(
            half_extents=physics.Vec3(10, 0.5, 10),
            position=physics.Vec3(0, 0, 0),
            mass=0.0
        )
        wall.set_restitution(e)

        v_approach = 10.0
        expected_v_separation = e * v_approach

        # Simulate collision
        for _ in range(100):
            world.step(1.0 / 60.0)

        v_after = abs(ball.get_velocity().y)
        error = abs(v_after - expected_v_separation)

        assert error < 1.0, \
            f"Restitution error: {error:.2f}m/s (expected: {expected_v_separation:.2f}, actual: {v_after:.2f})"

        print(f"✓ Restitution e={e}: separation velocity = {v_after:.2f}m/s")


class TestFriction:
    """Friction force: f = μ*N"""

    def test_sliding_friction(self):
        """Block sliding down incline: a = g*(sinθ - μ*cosθ)"""
        pytest.skip("Requires inclined plane support (TODO)")

    def test_static_friction_prevents_motion(self):
        """Static friction should prevent motion below threshold"""
        pytest.skip("Requires static friction model (TODO)")


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
