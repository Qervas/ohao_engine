/**
 * force_generator_tests.cpp - Unit tests for force generator subsystem
 *
 * Tests:
 *   1. ForceVolume (box and sphere, shouldAffectBody, applyForce)
 *   2. WindForce (shouldAffectBody, applyForce direction)
 *   3. BuoyancyForce (shouldAffectBody above/below liquid level)
 *   4. SpringForce / AnchorSpringForce / BungeeSpringForce (Hooke's law)
 *   5. ForceRegistry (register, count, enable/disable, unregister)
 *   6. RigidBody force accumulation and clearForces()
 *   7. AABB math (contains, intersects, expand, combine)
 *   8. ForceGenerator enable/disable
 */

#include <iostream>
#include <memory>
#include <cmath>
#include <vector>

#include "physics/forces/force_volume.hpp"
#include "physics/forces/spring_force.hpp"
#include "physics/forces/environmental_force.hpp"
#include "physics/forces/force_registry.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/utils/physics_math.hpp"
#include "physics/common/physics_constants.hpp"

using namespace ohao;
using namespace ohao::physics;
using namespace ohao::physics::dynamics;
using namespace ohao::physics::forces;
using namespace ohao::physics::math;

// =============================================================================
// TEST FRAMEWORK (matches physics_backend_tests.cpp)
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_BEGIN(name) \
    do { \
        testsRun++; \
        std::cout << "  TEST: " << name << "... " << std::flush; \
    } while(0)

#define TEST_PASS() \
    do { \
        testsPassed++; \
        std::cout << "\033[32mPASS\033[0m" << std::endl; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        testsFailed++; \
        std::cout << "\033[31mFAIL: " << msg << "\033[0m" << std::endl; \
    } while(0)

#define EXPECT(expr, msg) \
    do { \
        if (!(expr)) { TEST_FAIL(msg); return; } \
    } while(0)

#define EXPECT_NEAR(a, b, eps, msg) \
    do { \
        if (std::abs((a) - (b)) > (eps)) { \
            TEST_FAIL(std::string(msg) + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")"); \
            return; \
        } \
    } while(0)

// Helpers
static std::shared_ptr<RigidBody> makeDynamic(const glm::vec3& pos = glm::vec3(0.0f), float mass = 1.0f) {
    auto body = std::make_shared<RigidBody>(nullptr);
    body->setType(RigidBodyType::DYNAMIC);
    body->setMass(mass);
    body->setPosition(pos);
    return body;
}

static std::shared_ptr<RigidBody> makeStatic(const glm::vec3& pos = glm::vec3(0.0f)) {
    auto body = std::make_shared<RigidBody>(nullptr);
    body->setType(RigidBodyType::STATIC);
    body->setPosition(pos);
    return body;
}

// =============================================================================
// SECTION 1 — AABB MATH
// =============================================================================

static void runAABBTests() {
    std::cout << "\n[AABB Math]\n";

    TEST_BEGIN("AABB contains point inside");
    {
        AABB box(glm::vec3(0.0f), glm::vec3(1.0f));  // center (0,0,0), half-extents (1,1,1)
        EXPECT(box.contains(glm::vec3(0.0f)), "center should be inside");
        EXPECT(box.contains(glm::vec3(0.5f, 0.5f, 0.5f)), "interior point should be inside");
        TEST_PASS();
    }

    TEST_BEGIN("AABB contains point outside");
    {
        AABB box(glm::vec3(0.0f), glm::vec3(1.0f));
        EXPECT(!box.contains(glm::vec3(2.0f, 0.0f, 0.0f)), "outside X should fail");
        EXPECT(!box.contains(glm::vec3(0.0f, -1.5f, 0.0f)), "outside Y should fail");
        TEST_PASS();
    }

    TEST_BEGIN("AABB contains point on boundary");
    {
        AABB box(glm::vec3(0.0f), glm::vec3(1.0f));
        EXPECT(box.contains(glm::vec3(1.0f, 0.0f, 0.0f)), "boundary point should be inside");
        EXPECT(box.contains(glm::vec3(-1.0f, -1.0f, -1.0f)), "corner should be inside");
        TEST_PASS();
    }

    TEST_BEGIN("AABB intersects overlapping");
    {
        AABB a(glm::vec3(0.0f), glm::vec3(1.0f));
        AABB b(glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(1.0f));
        EXPECT(a.intersects(b), "overlapping boxes should intersect");
        TEST_PASS();
    }

    TEST_BEGIN("AABB intersects separated");
    {
        AABB a(glm::vec3(0.0f), glm::vec3(1.0f));
        AABB b(glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        EXPECT(!a.intersects(b), "separated boxes should not intersect");
        TEST_PASS();
    }

    TEST_BEGIN("AABB expand");
    {
        AABB box(glm::vec3(0.0f), glm::vec3(1.0f));
        AABB expanded = box.expand(0.5f);
        // half-extents should now be 1.5
        EXPECT(expanded.contains(glm::vec3(1.4f, 0.0f, 0.0f)), "expanded box should contain farther point");
        EXPECT(!expanded.contains(glm::vec3(2.0f, 0.0f, 0.0f)), "expanded box boundary check");
        TEST_PASS();
    }

    TEST_BEGIN("AABB combine");
    {
        AABB a(glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        AABB b(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        AABB combined = a.combine(b);
        EXPECT(combined.contains(glm::vec3(-2.0f, 0.0f, 0.0f)), "combined should contain a's center");
        EXPECT(combined.contains(glm::vec3(2.0f, 0.0f, 0.0f)), "combined should contain b's center");
        EXPECT(combined.contains(glm::vec3(0.0f, 0.0f, 0.0f)), "combined should contain midpoint");
        TEST_PASS();
    }

    TEST_BEGIN("AABB getCenter and getExtents");
    {
        AABB box(glm::vec3(3.0f, 1.0f, -2.0f), glm::vec3(1.0f, 2.0f, 0.5f));
        glm::vec3 center = box.getCenter();
        EXPECT_NEAR(center.x, 3.0f, 1e-5f, "center X");
        EXPECT_NEAR(center.y, 1.0f, 1e-5f, "center Y");
        EXPECT_NEAR(center.z, -2.0f, 1e-5f, "center Z");
        glm::vec3 ext = box.getExtents();
        EXPECT_NEAR(ext.x, 1.0f, 1e-5f, "extents X");
        EXPECT_NEAR(ext.y, 2.0f, 1e-5f, "extents Y");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 2 — RIGID BODY FORCE ACCUMULATION
// =============================================================================

static void runRigidBodyForceTests() {
    std::cout << "\n[RigidBody Force Accumulation]\n";

    TEST_BEGIN("applyForce accumulates");
    {
        auto body = makeDynamic();
        body->applyForce(glm::vec3(10.0f, 0.0f, 0.0f));
        body->applyForce(glm::vec3(0.0f, 5.0f, 0.0f));
        glm::vec3 f = body->getAccumulatedForce();
        EXPECT_NEAR(f.x, 10.0f, 1e-5f, "force X");
        EXPECT_NEAR(f.y, 5.0f, 1e-5f, "force Y");
        EXPECT_NEAR(f.z, 0.0f, 1e-5f, "force Z");
        TEST_PASS();
    }

    TEST_BEGIN("clearForces resets accumulator");
    {
        auto body = makeDynamic();
        body->applyForce(glm::vec3(100.0f, 100.0f, 100.0f));
        body->clearForces();
        glm::vec3 f = body->getAccumulatedForce();
        EXPECT_NEAR(f.x, 0.0f, 1e-5f, "cleared force X");
        EXPECT_NEAR(f.y, 0.0f, 1e-5f, "cleared force Y");
        EXPECT_NEAR(f.z, 0.0f, 1e-5f, "cleared force Z");
        TEST_PASS();
    }

    TEST_BEGIN("applyTorque accumulates");
    {
        auto body = makeDynamic();
        body->applyTorque(glm::vec3(0.0f, 3.0f, 0.0f));
        glm::vec3 t = body->getAccumulatedTorque();
        EXPECT_NEAR(t.y, 3.0f, 1e-5f, "torque Y");
        TEST_PASS();
    }

    TEST_BEGIN("isDynamic / isStatic flags");
    {
        auto dyn = makeDynamic();
        auto stat = makeStatic();
        EXPECT(dyn->isDynamic(), "dynamic body should report isDynamic");
        EXPECT(!dyn->isStatic(), "dynamic body should not report isStatic");
        EXPECT(stat->isStatic(), "static body should report isStatic");
        EXPECT(!stat->isDynamic(), "static body should not report isDynamic");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 3 — FORCE VOLUME
// =============================================================================

static void runForceVolumeTests() {
    std::cout << "\n[ForceVolume]\n";

    TEST_BEGIN("BOX: shouldAffectBody - body inside");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(2.0f), glm::vec3(0.0f, 10.0f, 0.0f));
        auto body = makeDynamic(glm::vec3(0.0f));
        EXPECT(vol.shouldAffectBody(body.get()), "body at center should be inside box");
        TEST_PASS();
    }

    TEST_BEGIN("BOX: shouldAffectBody - body outside");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f, 10.0f, 0.0f));
        auto body = makeDynamic(glm::vec3(5.0f, 0.0f, 0.0f));
        EXPECT(!vol.shouldAffectBody(body.get()), "body far outside box should not be affected");
        TEST_PASS();
    }

    TEST_BEGIN("BOX: shouldAffectBody - static body excluded");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(5.0f), glm::vec3(0.0f, 10.0f, 0.0f));
        auto stat = makeStatic(glm::vec3(0.0f));
        EXPECT(!vol.shouldAffectBody(stat.get()), "static body should not be affected");
        TEST_PASS();
    }

    TEST_BEGIN("SPHERE: shouldAffectBody - inside radius");
    {
        ForceVolume vol(glm::vec3(0.0f), 3.0f, glm::vec3(0.0f, 10.0f, 0.0f));
        auto body = makeDynamic(glm::vec3(2.0f, 0.0f, 0.0f));
        EXPECT(vol.shouldAffectBody(body.get()), "body within sphere radius should be affected");
        TEST_PASS();
    }

    TEST_BEGIN("SPHERE: shouldAffectBody - outside radius");
    {
        ForceVolume vol(glm::vec3(0.0f), 2.0f, glm::vec3(0.0f, 10.0f, 0.0f));
        auto body = makeDynamic(glm::vec3(3.0f, 0.0f, 0.0f));
        EXPECT(!vol.shouldAffectBody(body.get()), "body outside sphere radius should not be affected");
        TEST_PASS();
    }

    TEST_BEGIN("applyForce adds to accumulator");
    {
        glm::vec3 force(0.0f, 25.0f, 0.0f);
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(5.0f), force);
        auto body = makeDynamic(glm::vec3(0.0f));
        body->clearForces();
        vol.applyForce(body.get(), 0.016f);
        glm::vec3 f = body->getAccumulatedForce();
        EXPECT_NEAR(f.y, 25.0f, 1e-4f, "accumulated force should match volume force");
        TEST_PASS();
    }

    TEST_BEGIN("setForce updates force");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(5.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        vol.setForce(glm::vec3(0.0f, 99.0f, 0.0f));
        EXPECT_NEAR(vol.getForce().y, 99.0f, 1e-5f, "getForce should return updated value");
        TEST_PASS();
    }

    TEST_BEGIN("getName returns ForceVolume");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f));
        EXPECT(vol.getName() == "ForceVolume", "getName mismatch");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 4 — WIND FORCE
// =============================================================================

static void runWindForceTests() {
    std::cout << "\n[WindForce]\n";

    TEST_BEGIN("shouldAffectBody - dynamic body");
    {
        WindForce wind(glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
        auto body = makeDynamic();
        EXPECT(wind.shouldAffectBody(body.get()), "dynamic body should be affected by wind");
        TEST_PASS();
    }

    TEST_BEGIN("shouldAffectBody - static body excluded");
    {
        WindForce wind(glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
        auto stat = makeStatic();
        EXPECT(!wind.shouldAffectBody(stat.get()), "static body should not be affected by wind");
        TEST_PASS();
    }

    TEST_BEGIN("applyForce applies non-zero force");
    {
        WindForce wind(glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
        wind.setTurbulence(0.0f);  // Disable turbulence for deterministic test
        auto body = makeDynamic();
        body->clearForces();
        wind.applyForce(body.get(), 0.016f);
        glm::vec3 f = body->getAccumulatedForce();
        // Base force should be direction * strength = (1,0,0) * 20 = 20 in X
        EXPECT_NEAR(f.x, 20.0f, 1e-4f, "wind force X");
        EXPECT_NEAR(f.y, 0.0f, 1e-4f, "wind force Y should be zero (no turbulence)");
        TEST_PASS();
    }

    TEST_BEGIN("setDirection updates direction");
    {
        WindForce wind(glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
        wind.setDirection(glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 d = wind.getDirection();
        EXPECT_NEAR(d.y, 1.0f, 1e-5f, "wind direction Y after update");
        EXPECT_NEAR(d.x, 0.0f, 1e-5f, "wind direction X after update");
        TEST_PASS();
    }

    TEST_BEGIN("getName returns WindForce");
    {
        WindForce wind;
        EXPECT(wind.getName() == "WindForce", "getName mismatch");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 5 — BUOYANCY FORCE
// =============================================================================

static void runBuoyancyForceTests() {
    std::cout << "\n[BuoyancyForce]\n";

    TEST_BEGIN("shouldAffectBody - body below liquid level");
    {
        BuoyancyForce buoy(1000.0f, 0.0f);  // liquid at y=0
        auto body = makeDynamic(glm::vec3(0.0f, -1.0f, 0.0f));  // 1 unit below surface
        EXPECT(buoy.shouldAffectBody(body.get()), "submerged body should be affected");
        TEST_PASS();
    }

    TEST_BEGIN("shouldAffectBody - body above liquid level");
    {
        BuoyancyForce buoy(1000.0f, 0.0f);  // liquid at y=0
        auto body = makeDynamic(glm::vec3(0.0f, 2.0f, 0.0f));  // above surface
        EXPECT(!buoy.shouldAffectBody(body.get()), "body above surface should not be affected");
        TEST_PASS();
    }

    TEST_BEGIN("applyForce applies upward force when submerged");
    {
        BuoyancyForce buoy(1000.0f, 0.0f);  // liquid at y=0
        auto body = makeDynamic(glm::vec3(0.0f, -2.0f, 0.0f), 8.0f);  // mass=8, submerged
        body->clearForces();
        buoy.applyForce(body.get(), 0.016f);
        glm::vec3 f = body->getAccumulatedForce();
        // Buoyancy force should be upward (positive Y)
        EXPECT(f.y > 0.0f, "buoyant force should be upward");
        TEST_PASS();
    }

    TEST_BEGIN("getLiquidLevel/setLiquidLevel");
    {
        BuoyancyForce buoy(1000.0f, 5.0f);
        EXPECT_NEAR(buoy.getLiquidLevel(), 5.0f, 1e-5f, "liquid level getter");
        buoy.setLiquidLevel(10.0f);
        EXPECT_NEAR(buoy.getLiquidLevel(), 10.0f, 1e-5f, "liquid level after set");
        TEST_PASS();
    }

    TEST_BEGIN("getName returns BuoyancyForce");
    {
        BuoyancyForce buoy;
        EXPECT(buoy.getName() == "BuoyancyForce", "getName mismatch");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 6 — SPRING FORCES
// =============================================================================

static void runSpringForceTests() {
    std::cout << "\n[SpringForce / AnchorSpringForce / BungeeSpringForce]\n";

    TEST_BEGIN("SpringForce: stretched pulls bodyA toward bodyB");
    {
        // Body A at -2, Body B at +2, rest length = 2 → stretched by 2
        auto bodyA = makeDynamic(glm::vec3(-2.0f, 0.0f, 0.0f));
        auto bodyB = makeDynamic(glm::vec3(2.0f, 0.0f, 0.0f));
        SpringForce spring(bodyA.get(), bodyB.get(), 10.0f, 2.0f, 0.0f);  // k=10, rest=2, damp=0

        bodyA->clearForces();
        bodyB->clearForces();
        spring.applyForce(bodyA.get(), 0.0f);

        // Body A should be pulled toward B (positive X direction)
        glm::vec3 fa = bodyA->getAccumulatedForce();
        EXPECT(fa.x > 0.0f, "bodyA should be pulled toward bodyB (positive X)");
        TEST_PASS();
    }

    TEST_BEGIN("SpringForce: stretched pulls bodyB toward bodyA");
    {
        auto bodyA = makeDynamic(glm::vec3(-2.0f, 0.0f, 0.0f));
        auto bodyB = makeDynamic(glm::vec3(2.0f, 0.0f, 0.0f));
        SpringForce spring(bodyA.get(), bodyB.get(), 10.0f, 2.0f, 0.0f);

        bodyA->clearForces();
        bodyB->clearForces();
        spring.applyForce(bodyB.get(), 0.0f);

        // Body B should be pulled toward A (negative X direction)
        glm::vec3 fb = bodyB->getAccumulatedForce();
        EXPECT(fb.x < 0.0f, "bodyB should be pulled toward bodyA (negative X)");
        TEST_PASS();
    }

    TEST_BEGIN("SpringForce: getCurrentLength");
    {
        auto bodyA = makeDynamic(glm::vec3(0.0f, 0.0f, 0.0f));
        auto bodyB = makeDynamic(glm::vec3(3.0f, 0.0f, 0.0f));
        SpringForce spring(bodyA.get(), bodyB.get(), 10.0f, 1.0f, 0.0f);
        EXPECT_NEAR(spring.getCurrentLength(), 3.0f, 1e-5f, "spring length should be distance between bodies");
        EXPECT_NEAR(spring.getCurrentExtension(), 2.0f, 1e-5f, "extension = currentLength - restLength");
        TEST_PASS();
    }

    TEST_BEGIN("AnchorSpringForce: pulls body toward anchor when stretched");
    {
        // Anchor at origin, body at (5, 0, 0), rest length = 1 → stretched by 4
        auto body = makeDynamic(glm::vec3(5.0f, 0.0f, 0.0f));
        AnchorSpringForce anchor(body.get(), glm::vec3(0.0f), 10.0f, 1.0f, 0.0f);

        body->clearForces();
        anchor.applyForce(body.get(), 0.0f);

        // Should be pulled toward origin (negative X)
        glm::vec3 f = body->getAccumulatedForce();
        EXPECT(f.x < 0.0f, "body should be pulled toward anchor (negative X)");
        TEST_PASS();
    }

    TEST_BEGIN("AnchorSpringForce: getCurrentLength");
    {
        auto body = makeDynamic(glm::vec3(3.0f, 4.0f, 0.0f));  // distance from origin = 5
        AnchorSpringForce anchor(body.get(), glm::vec3(0.0f), 10.0f, 1.0f, 0.0f);
        EXPECT_NEAR(anchor.getCurrentLength(), 5.0f, 1e-4f, "distance to anchor");
        TEST_PASS();
    }

    TEST_BEGIN("BungeeSpringForce: no force when compressed");
    {
        // Bodies only 0.5 apart, rest length = 2 → compressed → no force
        auto bodyA = makeDynamic(glm::vec3(0.0f, 0.0f, 0.0f));
        auto bodyB = makeDynamic(glm::vec3(0.5f, 0.0f, 0.0f));
        BungeeSpringForce bungee(bodyA.get(), bodyB.get(), 10.0f, 2.0f);

        bodyA->clearForces();
        bungee.applyForce(bodyA.get(), 0.016f);

        glm::vec3 fa = bodyA->getAccumulatedForce();
        EXPECT_NEAR(fa.x, 0.0f, 1e-5f, "compressed bungee should apply no force");
        EXPECT_NEAR(fa.y, 0.0f, 1e-5f, "compressed bungee Y should be zero");
        TEST_PASS();
    }

    TEST_BEGIN("BungeeSpringForce: force when stretched");
    {
        // Bodies 5 apart, rest length = 2 → stretched → pulls toward each other
        auto bodyA = makeDynamic(glm::vec3(0.0f, 0.0f, 0.0f));
        auto bodyB = makeDynamic(glm::vec3(5.0f, 0.0f, 0.0f));
        BungeeSpringForce bungee(bodyA.get(), bodyB.get(), 10.0f, 2.0f);

        bodyA->clearForces();
        bungee.applyForce(bodyA.get(), 0.016f);

        glm::vec3 fa = bodyA->getAccumulatedForce();
        EXPECT(fa.x > 0.0f, "stretched bungee should pull bodyA toward bodyB");
        TEST_PASS();
    }

    TEST_BEGIN("SpringForce getName");
    {
        auto bodyA = makeDynamic();
        auto bodyB = makeDynamic();
        SpringForce spring(bodyA.get(), bodyB.get());
        EXPECT(spring.getName() == "SpringForce", "getName mismatch");
        TEST_PASS();
    }

    TEST_BEGIN("AnchorSpringForce getName");
    {
        auto body = makeDynamic();
        AnchorSpringForce anchor(body.get(), glm::vec3(0.0f));
        EXPECT(anchor.getName() == "AnchorSpringForce", "getName mismatch");
        TEST_PASS();
    }

    TEST_BEGIN("BungeeSpringForce getName");
    {
        auto bodyA = makeDynamic();
        auto bodyB = makeDynamic();
        BungeeSpringForce bungee(bodyA.get(), bodyB.get());
        EXPECT(bungee.getName() == "BungeeSpringForce", "getName mismatch");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 7 — FORCE GENERATOR BASE (enable/disable)
// =============================================================================

static void runForceGeneratorBaseTests() {
    std::cout << "\n[ForceGenerator Base]\n";

    TEST_BEGIN("ForceVolume enabled by default");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f));
        EXPECT(vol.isEnabled(), "force generator should be enabled by default");
        TEST_PASS();
    }

    TEST_BEGIN("setEnabled(false) disables generator");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f));
        vol.setEnabled(false);
        EXPECT(!vol.isEnabled(), "generator should report disabled");
        TEST_PASS();
    }

    TEST_BEGIN("setEnabled(true) re-enables generator");
    {
        WindForce wind;
        wind.setEnabled(false);
        wind.setEnabled(true);
        EXPECT(wind.isEnabled(), "generator should be re-enabled");
        TEST_PASS();
    }

    TEST_BEGIN("priority defaults to 0");
    {
        ForceVolume vol(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f));
        EXPECT(vol.getPriority() == 0, "default priority should be 0");
        TEST_PASS();
    }

    TEST_BEGIN("setPriority");
    {
        WindForce wind;
        wind.setPriority(5);
        EXPECT(wind.getPriority() == 5, "priority should be updated");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 8 — FORCE REGISTRY
// =============================================================================

static void runForceRegistryTests() {
    std::cout << "\n[ForceRegistry]\n";

    TEST_BEGIN("registerForce increments count");
    {
        ForceRegistry reg;
        EXPECT(reg.getForceCount() == 0, "empty registry should have 0 forces");

        reg.registerForce(std::make_unique<WindForce>(), "wind1");
        EXPECT(reg.getForceCount() == 1, "count after 1 registration");

        reg.registerForce(std::make_unique<WindForce>(), "wind2");
        EXPECT(reg.getForceCount() == 2, "count after 2 registrations");
        TEST_PASS();
    }

    TEST_BEGIN("unregisterForce decrements count");
    {
        ForceRegistry reg;
        size_t id = reg.registerForce(std::make_unique<WindForce>(), "wind");
        EXPECT(reg.getForceCount() == 1, "count before unregister");
        bool ok = reg.unregisterForce(id);
        EXPECT(ok, "unregisterForce should return true for valid ID");
        EXPECT(reg.getForceCount() == 0, "count after unregister");
        TEST_PASS();
    }

    TEST_BEGIN("unregisterForce returns false for invalid ID");
    {
        ForceRegistry reg;
        bool ok = reg.unregisterForce(9999);
        EXPECT(!ok, "unregisterForce should return false for unknown ID");
        TEST_PASS();
    }

    TEST_BEGIN("setForceEnabled disables and re-enables");
    {
        ForceRegistry reg;
        size_t id = reg.registerForce(std::make_unique<WindForce>(), "wind");
        EXPECT(reg.getActiveForceCount() == 1, "1 active force after register");

        reg.setForceEnabled(id, false);
        EXPECT(reg.getActiveForceCount() == 0, "0 active forces after disable");

        reg.setForceEnabled(id, true);
        EXPECT(reg.getActiveForceCount() == 1, "1 active force after re-enable");
        TEST_PASS();
    }

    TEST_BEGIN("clear removes all forces");
    {
        ForceRegistry reg;
        reg.registerForce(std::make_unique<WindForce>(), "wind1");
        reg.registerForce(std::make_unique<WindForce>(), "wind2");
        reg.registerForce(std::make_unique<WindForce>(), "wind3");
        EXPECT(reg.getForceCount() == 3, "count before clear");
        reg.clear();
        EXPECT(reg.getForceCount() == 0, "count after clear");
        TEST_PASS();
    }

    TEST_BEGIN("applyForces applies to dynamic bodies");
    {
        ForceRegistry reg;
        // Wind blowing in +X
        reg.registerForce(std::make_unique<WindForce>(glm::vec3(1.0f, 0.0f, 0.0f), 10.0f), "wind");

        auto body = makeDynamic();
        body->clearForces();
        std::vector<RigidBody*> bodies = { body.get() };
        reg.applyForces(bodies, 0.016f);

        glm::vec3 f = body->getAccumulatedForce();
        // Wind without turbulence (turbulence defaults to 0.1 — small), force dominated by X=10
        EXPECT(f.x > 0.0f, "wind should have applied positive X force");
        TEST_PASS();
    }

    TEST_BEGIN("applyForces skips disabled forces");
    {
        ForceRegistry reg;
        size_t id = reg.registerForce(
            std::make_unique<WindForce>(glm::vec3(1.0f, 0.0f, 0.0f), 50.0f), "wind");
        reg.setForceEnabled(id, false);

        auto body = makeDynamic();
        body->clearForces();
        std::vector<RigidBody*> bodies = { body.get() };
        reg.applyForces(bodies, 0.016f);

        glm::vec3 f = body->getAccumulatedForce();
        // Disabled force should not be applied — force should remain zero
        EXPECT_NEAR(f.x, 0.0f, 1e-4f, "disabled force should not be applied");
        TEST_PASS();
    }

    TEST_BEGIN("addBodyToForce / removeBodyFromForce");
    {
        ForceRegistry reg;
        auto body = makeDynamic();

        size_t id = reg.registerForce(std::make_unique<WindForce>(), "wind");
        bool added = reg.addBodyToForce(id, body.get());
        EXPECT(added, "addBodyToForce should return true");

        bool removed = reg.removeBodyFromForce(id, body.get());
        EXPECT(removed, "removeBodyFromForce should return true");
        TEST_PASS();
    }

    TEST_BEGIN("getForceNames returns registered names");
    {
        ForceRegistry reg;
        reg.registerForce(std::make_unique<WindForce>(), "alpha");
        reg.registerForce(std::make_unique<WindForce>(), "beta");

        auto names = reg.getForceNames();
        EXPECT(names.size() == 2, "should have 2 names");
        TEST_PASS();
    }
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    std::cout << "================================================\n";
    std::cout << "  OHAO Force Generator Tests\n";
    std::cout << "================================================\n";

    runAABBTests();
    runRigidBodyForceTests();
    runForceVolumeTests();
    runWindForceTests();
    runBuoyancyForceTests();
    runSpringForceTests();
    runForceGeneratorBaseTests();
    runForceRegistryTests();

    std::cout << "\n================================================\n";
    std::cout << "  Results: " << testsPassed << "/" << testsRun << " passed";
    if (testsFailed > 0) {
        std::cout << "  \033[31m(" << testsFailed << " failed)\033[0m";
    }
    std::cout << "\n================================================\n";

    return testsFailed > 0 ? 1 : 0;
}
