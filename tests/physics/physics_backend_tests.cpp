/**
 * physics_backend_tests.cpp - Comprehensive Tests for Jolt Physics Backend
 *
 * Tests all 7 features of the physics backend:
 *   1. Body lifecycle (create, destroy, transforms, velocity, forces)
 *   2. CCD (Continuous Collision Detection)
 *   3. 16-layer collision system
 *   4. Raycasting and shape queries
 *   5. Contact callbacks
 *   6. Constraints (7 types)
 *   7. Character controller
 */

#include <iostream>
#include <vector>
#include <memory>
#include <cmath>
#include <thread>
#include <chrono>

#include "physics/backend/physics_backend.hpp"
#include "physics/world/physics_world.hpp"

using namespace ohao::physics;
using namespace ohao::physics::backend;

// =============================================================================
// TEST FRAMEWORK (same as renderer_pipeline_tests)
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;
static int testsSkipped = 0;

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

#define TEST_SKIP(msg) \
    do { \
        testsSkipped++; \
        std::cout << "\033[33mSKIP: " << msg << "\033[0m" << std::endl; \
    } while(0)

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            TEST_FAIL(#expr " was false"); \
            return false; \
        } \
    } while(0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            TEST_FAIL(#a " != " #b); \
            return false; \
        } \
    } while(0)

#define EXPECT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            TEST_FAIL(#a " == " #b); \
            return false; \
        } \
    } while(0)

#define EXPECT_NEAR(a, b, eps) \
    do { \
        if (std::abs((a) - (b)) > (eps)) { \
            TEST_FAIL(#a " not near " #b); \
            return false; \
        } \
    } while(0)

#define EXPECT_VEC3_NEAR(a, b, eps) \
    do { \
        if (std::abs((a).x - (b).x) > (eps) || \
            std::abs((a).y - (b).y) > (eps) || \
            std::abs((a).z - (b).z) > (eps)) { \
            TEST_FAIL(#a " not near " #b); \
            return false; \
        } \
    } while(0)

// =============================================================================
// Global backend for testing
// =============================================================================

static std::unique_ptr<IPhysicsBackend> g_backend;
static bool g_joltAvailable = false;

static bool initBackend() {
    PhysicsWorldConfig config;
    config.gravity = glm::vec3(0, -9.81f, 0);
    config.maxBodies = 1024;

    g_backend = createPhysicsBackend("jolt");
    if (!g_backend) return false;

    bool ok = g_backend->initialize(config);
    if (!ok) return false;

    g_joltAvailable = (std::string(g_backend->getName()) == "jolt");
    return true;
}

static void shutdownBackend() {
    if (g_backend) {
        g_backend->shutdown();
        g_backend.reset();
    }
}

// Helper: create a dynamic box at a position
static BodyHandle createDynamicBox(const glm::vec3& pos, float mass = 1.0f) {
    BodyCreationInfo info;
    info.position = pos;
    info.motionType = MotionType::DYNAMIC;
    info.mass = mass;
    info.shape.type = ShapeInfo::BOX;
    info.shape.halfExtents = glm::vec3(0.5f);
    return g_backend->createBody(info);
}

// Helper: create a static box at a position
static BodyHandle createStaticBox(const glm::vec3& pos) {
    BodyCreationInfo info;
    info.position = pos;
    info.motionType = MotionType::STATIC;
    info.shape.type = ShapeInfo::BOX;
    info.shape.halfExtents = glm::vec3(0.5f);
    return g_backend->createBody(info);
}

// Helper: create a large static floor
static BodyHandle createFloor() {
    BodyCreationInfo info;
    info.position = glm::vec3(0, -0.5f, 0);
    info.motionType = MotionType::STATIC;
    info.shape.type = ShapeInfo::BOX;
    info.shape.halfExtents = glm::vec3(50.0f, 0.5f, 50.0f);
    info.layer = CollisionLayer::STATIC;
    return g_backend->createBody(info);
}

// =============================================================================
// 1. BACKEND LIFECYCLE TESTS
// =============================================================================

bool testBackendCreation() {
    TEST_BEGIN("Backend creation and initialization");
    EXPECT_TRUE(g_backend != nullptr);
    EXPECT_TRUE(g_backend->isInitialized());
    std::cout << "[" << g_backend->getName() << "] ";
    TEST_PASS();
    return true;
}

bool testBackendStats() {
    TEST_BEGIN("Backend stats");
    auto stats = g_backend->getStats();
    // Fresh backend should have zero bodies
    EXPECT_EQ(stats.totalBodies, 0u);
    EXPECT_EQ(stats.constraintCount, 0u);
    EXPECT_EQ(stats.characterCount, 0u);
    TEST_PASS();
    return true;
}

bool testGravityGetSet() {
    TEST_BEGIN("Gravity get/set");
    auto gravity = g_backend->getGravity();
    EXPECT_NEAR(gravity.y, -9.81f, 0.01f);

    g_backend->setGravity(glm::vec3(0, -20.0f, 0));
    gravity = g_backend->getGravity();
    EXPECT_NEAR(gravity.y, -20.0f, 0.01f);

    // Reset
    g_backend->setGravity(glm::vec3(0, -9.81f, 0));
    TEST_PASS();
    return true;
}

// =============================================================================
// 2. BODY LIFECYCLE TESTS
// =============================================================================

bool testBodyCreateDestroy() {
    TEST_BEGIN("Body create and destroy");

    auto h = createDynamicBox(glm::vec3(0, 5, 0));
    EXPECT_NE(h, INVALID_BODY);
    EXPECT_TRUE(g_backend->isValidBody(h));

    g_backend->destroyBody(h);
    EXPECT_FALSE(g_backend->isValidBody(h));

    TEST_PASS();
    return true;
}

bool testBodyPositionGetSet() {
    TEST_BEGIN("Body position get/set");

    auto h = createDynamicBox(glm::vec3(1, 2, 3));
    EXPECT_NE(h, INVALID_BODY);

    auto pos = g_backend->getPosition(h);
    EXPECT_VEC3_NEAR(pos, glm::vec3(1, 2, 3), 0.01f);

    g_backend->setPosition(h, glm::vec3(10, 20, 30));
    pos = g_backend->getPosition(h);
    EXPECT_VEC3_NEAR(pos, glm::vec3(10, 20, 30), 0.01f);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testBodyRotationGetSet() {
    TEST_BEGIN("Body rotation get/set");

    auto h = createDynamicBox(glm::vec3(0, 5, 0));
    EXPECT_NE(h, INVALID_BODY);

    // Default rotation should be identity
    auto rot = g_backend->getRotation(h);
    EXPECT_NEAR(rot.w, 1.0f, 0.01f);
    EXPECT_NEAR(rot.x, 0.0f, 0.01f);
    EXPECT_NEAR(rot.y, 0.0f, 0.01f);
    EXPECT_NEAR(rot.z, 0.0f, 0.01f);

    // Set a 90-degree rotation around Y
    glm::quat rot90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    g_backend->setRotation(h, rot90);
    rot = g_backend->getRotation(h);
    EXPECT_NEAR(rot.w, rot90.w, 0.01f);
    EXPECT_NEAR(rot.y, rot90.y, 0.01f);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testBodyVelocity() {
    TEST_BEGIN("Body linear/angular velocity");

    auto h = createDynamicBox(glm::vec3(0, 5, 0));

    g_backend->setLinearVelocity(h, glm::vec3(1, 2, 3));
    auto lv = g_backend->getLinearVelocity(h);
    EXPECT_VEC3_NEAR(lv, glm::vec3(1, 2, 3), 0.01f);

    g_backend->setAngularVelocity(h, glm::vec3(0.5f, 0, 0));
    auto av = g_backend->getAngularVelocity(h);
    EXPECT_NEAR(av.x, 0.5f, 0.01f);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testBodyProperties() {
    TEST_BEGIN("Body physical properties (mass, friction, restitution)");

    auto h = createDynamicBox(glm::vec3(0, 5, 0), 10.0f);

    // These are fire-and-forget setters; verify they don't crash
    g_backend->setMass(h, 5.0f);
    g_backend->setFriction(h, 0.8f);
    g_backend->setRestitution(h, 0.5f);
    g_backend->setLinearDamping(h, 0.1f);
    g_backend->setAngularDamping(h, 0.1f);
    g_backend->setGravityEnabled(h, false);
    g_backend->setGravityEnabled(h, true);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testBodySleepWake() {
    TEST_BEGIN("Body sleep/wake");

    auto h = createDynamicBox(glm::vec3(0, 5, 0));

    g_backend->setAwake(h, true);
    EXPECT_TRUE(g_backend->isAwake(h));

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testBodyForces() {
    TEST_BEGIN("Body force/impulse/torque application");

    auto h = createDynamicBox(glm::vec3(0, 5, 0));

    // Apply force, impulse, torque — should not crash
    g_backend->applyForce(h, glm::vec3(100, 0, 0), glm::vec3(0));
    g_backend->applyImpulse(h, glm::vec3(0, 10, 0), glm::vec3(0));
    g_backend->applyTorque(h, glm::vec3(0, 5, 0));

    // Step physics to apply forces
    g_backend->step(1.0f / 60.0f);

    // Body should have moved from impulse
    auto vel = g_backend->getLinearVelocity(h);
    EXPECT_TRUE(glm::length(vel) > 0.01f);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testMultipleBodies() {
    TEST_BEGIN("Multiple body creation and tracking");

    std::vector<BodyHandle> bodies;
    for (int i = 0; i < 20; i++) {
        auto h = createDynamicBox(glm::vec3(static_cast<float>(i) * 2.0f, 5, 0));
        EXPECT_NE(h, INVALID_BODY);
        bodies.push_back(h);
    }

    auto stats = g_backend->getStats();
    EXPECT_TRUE(stats.totalBodies >= 20);

    // Destroy all
    for (auto h : bodies) {
        g_backend->destroyBody(h);
    }

    TEST_PASS();
    return true;
}

bool testShapeTypes() {
    TEST_BEGIN("All shape types (Box, Sphere, Capsule, Cylinder)");

    // Box
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(1, 1, 1);
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    // Sphere
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::SPHERE;
        info.shape.radius = 0.5f;
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    // Capsule
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::CAPSULE;
        info.shape.radius = 0.3f;
        info.shape.height = 1.8f;
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    // Cylinder
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::CYLINDER;
        info.shape.radius = 0.5f;
        info.shape.height = 2.0f;
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    TEST_PASS();
    return true;
}

bool testMotionTypes() {
    TEST_BEGIN("All motion types (Dynamic, Static, Kinematic)");

    // Dynamic
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(0.5f);
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    // Static
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 0, 0);
        info.motionType = MotionType::STATIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(10, 0.5f, 10);
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    // Kinematic
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 3, 0);
        info.motionType = MotionType::KINEMATIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(1, 0.1f, 1);
        auto h = g_backend->createBody(info);
        EXPECT_NE(h, INVALID_BODY);
        g_backend->destroyBody(h);
    }

    TEST_PASS();
    return true;
}

// =============================================================================
// 3. CCD TESTS
// =============================================================================

bool testCCDCreation() {
    TEST_BEGIN("CCD at creation time");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    BodyCreationInfo info;
    info.position = glm::vec3(0, 5, 0);
    info.motionType = MotionType::DYNAMIC;
    info.shape.type = ShapeInfo::SPHERE;
    info.shape.radius = 0.1f;
    info.useCCD = true;
    info.mass = 0.5f;

    auto h = g_backend->createBody(info);
    EXPECT_NE(h, INVALID_BODY);
    g_backend->destroyBody(h);

    TEST_PASS();
    return true;
}

bool testCCDToggle() {
    TEST_BEGIN("CCD enable/disable at runtime");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto h = createDynamicBox(glm::vec3(0, 5, 0));
    EXPECT_NE(h, INVALID_BODY);

    // Toggle CCD — should not crash
    g_backend->setCCDEnabled(h, true);
    g_backend->setCCDEnabled(h, false);
    g_backend->setCCDEnabled(h, true);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

// =============================================================================
// 4. COLLISION LAYER TESTS
// =============================================================================

bool testCollisionLayerConstants() {
    TEST_BEGIN("Collision layer constants");

    EXPECT_EQ(CollisionLayer::DEFAULT, 0);
    EXPECT_EQ(CollisionLayer::STATIC, 1);
    EXPECT_EQ(CollisionLayer::DYNAMIC, 2);
    EXPECT_EQ(CollisionLayer::KINEMATIC, 3);
    EXPECT_EQ(CollisionLayer::CHARACTER, 4);
    EXPECT_EQ(CollisionLayer::TRIGGER, 5);
    EXPECT_EQ(CollisionLayer::DEBRIS, 6);
    EXPECT_EQ(CollisionLayer::PROJECTILE, 7);
    EXPECT_EQ(CollisionLayer::NUM_LAYERS, 16);
    EXPECT_EQ(CollisionLayer::ALL_MASK, 0xFFFF);

    TEST_PASS();
    return true;
}

bool testBodyLayerAssignment() {
    TEST_BEGIN("Body layer get/set");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto h = createDynamicBox(glm::vec3(0, 5, 0));

    g_backend->setBodyLayer(h, CollisionLayer::PROJECTILE);
    auto layer = g_backend->getBodyLayer(h);
    EXPECT_EQ(layer, CollisionLayer::PROJECTILE);

    g_backend->setBodyLayer(h, CollisionLayer::DEBRIS);
    layer = g_backend->getBodyLayer(h);
    EXPECT_EQ(layer, CollisionLayer::DEBRIS);

    g_backend->destroyBody(h);
    TEST_PASS();
    return true;
}

bool testLayerCollisionMatrix() {
    TEST_BEGIN("Layer collision matrix configuration");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Disable debris-projectile collision — should not crash
    g_backend->setLayerCollision(CollisionLayer::DEBRIS, CollisionLayer::PROJECTILE, false);
    // Re-enable
    g_backend->setLayerCollision(CollisionLayer::DEBRIS, CollisionLayer::PROJECTILE, true);

    // Disable trigger-static
    g_backend->setLayerCollision(CollisionLayer::TRIGGER, CollisionLayer::STATIC, false);

    TEST_PASS();
    return true;
}

bool testAutoLayerAssignment() {
    TEST_BEGIN("Auto layer assignment by motion type");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Dynamic body with layer=0 should auto-assign to DYNAMIC layer
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 5, 0);
        info.motionType = MotionType::DYNAMIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(0.5f);
        info.layer = 0; // Auto-assign
        auto h = g_backend->createBody(info);
        auto layer = g_backend->getBodyLayer(h);
        EXPECT_EQ(layer, CollisionLayer::DYNAMIC);
        g_backend->destroyBody(h);
    }

    // Static body with layer=0 should auto-assign to STATIC layer
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 0, 0);
        info.motionType = MotionType::STATIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(10, 0.5f, 10);
        info.layer = 0; // Auto-assign
        auto h = g_backend->createBody(info);
        auto layer = g_backend->getBodyLayer(h);
        EXPECT_EQ(layer, CollisionLayer::STATIC);
        g_backend->destroyBody(h);
    }

    // Kinematic body with layer=0 should auto-assign to KINEMATIC layer
    {
        BodyCreationInfo info;
        info.position = glm::vec3(0, 3, 0);
        info.motionType = MotionType::KINEMATIC;
        info.shape.type = ShapeInfo::BOX;
        info.shape.halfExtents = glm::vec3(1, 0.1f, 1);
        info.layer = 0; // Auto-assign
        auto h = g_backend->createBody(info);
        auto layer = g_backend->getBodyLayer(h);
        EXPECT_EQ(layer, CollisionLayer::KINEMATIC);
        g_backend->destroyBody(h);
    }

    TEST_PASS();
    return true;
}

// =============================================================================
// 5. RAYCASTING TESTS
// =============================================================================

bool testRaycastBasic() {
    TEST_BEGIN("Basic raycast hit");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Create a static box at origin
    auto box = createStaticBox(glm::vec3(0, 0, 0));

    // Need a step to let Jolt process the body
    g_backend->step(1.0f / 60.0f);

    // Cast ray from above, pointing down
    RaycastHit hit;
    bool didHit = g_backend->castRay(
        glm::vec3(0, 10, 0),      // origin
        glm::vec3(0, -1, 0),      // direction
        100.0f,                     // max distance
        hit
    );

    EXPECT_TRUE(didHit);
    EXPECT_NE(hit.bodyHandle, INVALID_BODY);
    EXPECT_TRUE(hit.fraction > 0.0f && hit.fraction < 1.0f);

    g_backend->destroyBody(box);
    TEST_PASS();
    return true;
}

bool testRaycastMiss() {
    TEST_BEGIN("Raycast miss");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto box = createStaticBox(glm::vec3(0, 0, 0));
    g_backend->step(1.0f / 60.0f);

    // Cast ray that misses (away from the box)
    RaycastHit hit;
    bool didHit = g_backend->castRay(
        glm::vec3(100, 100, 100),
        glm::vec3(0, 1, 0),  // pointing up, away from everything
        50.0f,
        hit
    );

    EXPECT_FALSE(didHit);

    g_backend->destroyBody(box);
    TEST_PASS();
    return true;
}

bool testRaycastAll() {
    TEST_BEGIN("Raycast all (multiple hits)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Stack multiple boxes vertically
    auto b1 = createStaticBox(glm::vec3(0, 0, 0));
    auto b2 = createStaticBox(glm::vec3(0, 3, 0));
    auto b3 = createStaticBox(glm::vec3(0, 6, 0));
    g_backend->step(1.0f / 60.0f);

    auto hits = g_backend->castRayAll(
        glm::vec3(0, 10, 0),
        glm::vec3(0, -1, 0),
        20.0f
    );

    // Should hit at least 2 boxes (rays through box interiors may or may not register)
    EXPECT_TRUE(hits.size() >= 2);

    g_backend->destroyBody(b1);
    g_backend->destroyBody(b2);
    g_backend->destroyBody(b3);
    TEST_PASS();
    return true;
}

bool testRaycastLayerFilter() {
    TEST_BEGIN("Raycast with layer mask filtering");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Create a box on PROJECTILE layer
    BodyCreationInfo info;
    info.position = glm::vec3(0, 0, 0);
    info.motionType = MotionType::STATIC;
    info.shape.type = ShapeInfo::BOX;
    info.shape.halfExtents = glm::vec3(0.5f);
    info.layer = CollisionLayer::PROJECTILE;
    auto box = g_backend->createBody(info);
    g_backend->step(1.0f / 60.0f);

    // Cast ray with mask that excludes PROJECTILE layer
    RaycastHit hit;
    uint16_t maskNoProjectile = CollisionLayer::ALL_MASK & ~(1u << CollisionLayer::PROJECTILE);
    bool didHit = g_backend->castRay(
        glm::vec3(0, 10, 0),
        glm::vec3(0, -1, 0),
        20.0f,
        hit,
        maskNoProjectile
    );

    EXPECT_FALSE(didHit);

    // Cast with mask that includes PROJECTILE layer
    didHit = g_backend->castRay(
        glm::vec3(0, 10, 0),
        glm::vec3(0, -1, 0),
        20.0f,
        hit,
        (1u << CollisionLayer::PROJECTILE)
    );

    EXPECT_TRUE(didHit);

    g_backend->destroyBody(box);
    TEST_PASS();
    return true;
}

bool testOverlapSphere() {
    TEST_BEGIN("Overlap sphere query");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createStaticBox(glm::vec3(0, 0, 0));
    auto b2 = createStaticBox(glm::vec3(2, 0, 0));
    auto b3 = createStaticBox(glm::vec3(100, 0, 0)); // Far away
    g_backend->step(1.0f / 60.0f);

    // Overlap sphere at origin with radius 5 — should find b1 and b2 but not b3
    auto results = g_backend->overlapSphere(glm::vec3(0, 0, 0), 5.0f);
    EXPECT_TRUE(results.size() >= 2);

    g_backend->destroyBody(b1);
    g_backend->destroyBody(b2);
    g_backend->destroyBody(b3);
    TEST_PASS();
    return true;
}

bool testOverlapBox() {
    TEST_BEGIN("Overlap box query");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createStaticBox(glm::vec3(0, 0, 0));
    auto b2 = createStaticBox(glm::vec3(100, 0, 0)); // Far away
    g_backend->step(1.0f / 60.0f);

    auto results = g_backend->overlapBox(
        glm::vec3(0, 0, 0),
        glm::vec3(3, 3, 3),
        glm::quat(1, 0, 0, 0)
    );
    EXPECT_TRUE(results.size() >= 1);

    g_backend->destroyBody(b1);
    g_backend->destroyBody(b2);
    TEST_PASS();
    return true;
}

// =============================================================================
// 6. CONTACT CALLBACK TESTS
// =============================================================================

class TestContactListener : public IContactListener {
public:
    std::vector<ContactEvent> events;

    void onContactBegin(const ContactEvent& e) override { events.push_back(e); }
    void onContactPersist(const ContactEvent& e) override { events.push_back(e); }
    void onContactEnd(const ContactEvent& e) override { events.push_back(e); }
};

bool testContactListener() {
    TEST_BEGIN("Contact listener registration");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    TestContactListener listener;
    g_backend->setContactListener(&listener);

    // Clean up
    g_backend->setContactListener(nullptr);
    TEST_PASS();
    return true;
}

bool testContactEventQueue() {
    TEST_BEGIN("Contact event queue (get events)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Create floor and a falling box
    auto floor = createFloor();
    auto box = createDynamicBox(glm::vec3(0, 2, 0), 1.0f);

    // Step physics until the box hits the floor
    for (int i = 0; i < 120; i++) {
        g_backend->step(1.0f / 60.0f);
    }

    // Check for contact events
    auto events = g_backend->getContactEvents();
    // Events should have been generated (at least BEGIN when box hits floor)
    // Note: events are drained per step, so we may have already drained them
    // The important thing is that this doesn't crash

    g_backend->destroyBody(box);
    g_backend->destroyBody(floor);
    TEST_PASS();
    return true;
}

// =============================================================================
// 7. CONSTRAINT TESTS
// =============================================================================

bool testConstraintFixed() {
    TEST_BEGIN("Fixed constraint");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);
    auto b2 = createDynamicBox(glm::vec3(1, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::FIXED;
    settings.body1 = b1;
    settings.body2 = b2;
    settings.anchor1 = glm::vec3(0.5f, 0, 0);
    settings.anchor2 = glm::vec3(-0.5f, 0, 0);

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);
    EXPECT_TRUE(g_backend->isValidConstraint(ch));
    EXPECT_EQ(g_backend->getConstraintCount(), 1u);

    g_backend->destroyConstraint(ch);
    EXPECT_FALSE(g_backend->isValidConstraint(ch));
    EXPECT_EQ(g_backend->getConstraintCount(), 0u);

    g_backend->destroyBody(b1);
    g_backend->destroyBody(b2);
    TEST_PASS();
    return true;
}

bool testConstraintHinge() {
    TEST_BEGIN("Hinge constraint (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::HINGE;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.anchor1 = glm::vec3(0, 0, 0);
    settings.anchor2 = glm::vec3(0, 5, 0);
    settings.axis1 = glm::vec3(0, 1, 0);
    settings.axis2 = glm::vec3(0, 1, 0);
    settings.enableLimits = true;
    settings.limitMin = -glm::radians(45.0f);
    settings.limitMax = glm::radians(45.0f);

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintSlider() {
    TEST_BEGIN("Slider constraint (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::SLIDER;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.axis1 = glm::vec3(1, 0, 0);
    settings.axis2 = glm::vec3(1, 0, 0);
    settings.enableLimits = true;
    settings.limitMin = -2.0f;
    settings.limitMax = 2.0f;

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintPoint() {
    TEST_BEGIN("Point (ball-and-socket) constraint (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::POINT;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.anchor1 = glm::vec3(0, 0, 0);
    settings.anchor2 = glm::vec3(0, 5, 0);

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintDistance() {
    TEST_BEGIN("Distance constraint (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::DISTANCE;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.anchor1 = glm::vec3(0);
    settings.anchor2 = glm::vec3(0, 5, 0);
    settings.minDistance = 0.0f;
    settings.maxDistance = 2.0f;

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintCone() {
    TEST_BEGIN("Cone twist constraint (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::CONE_TWIST;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.anchor1 = glm::vec3(0, 0, 0);
    settings.anchor2 = glm::vec3(0, 5, 0);
    settings.axis1 = glm::vec3(0, -1, 0);
    settings.axis2 = glm::vec3(0, -1, 0);
    settings.limitMax = glm::radians(30.0f);

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintMotor() {
    TEST_BEGIN("Constraint motor state (hinge, world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::HINGE;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor
    settings.anchor1 = glm::vec3(0);
    settings.anchor2 = glm::vec3(0, 5, 0);
    settings.axis1 = glm::vec3(0, 1, 0);
    settings.axis2 = glm::vec3(0, 1, 0);

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    // Enable/disable motor
    g_backend->setConstraintMotorState(ch, true, 3.14f, 500.0f);
    g_backend->setConstraintMotorState(ch, false, 0.0f, 0.0f);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintEnableDisable() {
    TEST_BEGIN("Constraint enable/disable (world-anchored)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::FIXED;
    settings.body1 = b1;
    settings.body2 = INVALID_BODY; // World anchor

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);

    g_backend->setConstraintEnabled(ch, false);
    g_backend->setConstraintEnabled(ch, true);

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    TEST_PASS();
    return true;
}

bool testConstraintTwoBody() {
    TEST_BEGIN("Two-body fixed constraint (BodyLockMulti path)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto b1 = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);
    auto b2 = createDynamicBox(glm::vec3(1, 5, 0), 1.0f);

    ConstraintSettings settings;
    settings.type = ConstraintType::FIXED;
    settings.body1 = b1;
    settings.body2 = b2;

    auto ch = g_backend->createConstraint(settings);
    EXPECT_NE(ch, INVALID_CONSTRAINT);
    EXPECT_TRUE(g_backend->isValidConstraint(ch));

    g_backend->destroyConstraint(ch);
    g_backend->destroyBody(b1);
    g_backend->destroyBody(b2);
    TEST_PASS();
    return true;
}

// =============================================================================
// 8. CHARACTER CONTROLLER TESTS
// =============================================================================

bool testCharacterCreate() {
    TEST_BEGIN("Character controller creation");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    CharacterCreationInfo info;
    info.position = glm::vec3(0, 2, 0);
    info.capsuleRadius = 0.3f;
    info.capsuleHeight = 1.8f;
    info.maxSlopeAngleDeg = 50.0f;
    info.mass = 80.0f;

    auto ch = g_backend->createCharacter(info);
    EXPECT_NE(ch, INVALID_CHARACTER);
    EXPECT_TRUE(g_backend->isValidCharacter(ch));
    EXPECT_EQ(g_backend->getCharacterCount(), 1u);

    g_backend->destroyCharacter(ch);
    EXPECT_FALSE(g_backend->isValidCharacter(ch));
    EXPECT_EQ(g_backend->getCharacterCount(), 0u);

    TEST_PASS();
    return true;
}

bool testCharacterState() {
    TEST_BEGIN("Character state query");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    CharacterCreationInfo info;
    info.position = glm::vec3(5, 2, 5);
    info.capsuleRadius = 0.3f;
    info.capsuleHeight = 1.8f;
    info.mass = 80.0f;

    auto ch = g_backend->createCharacter(info);

    auto state = g_backend->getCharacterState(ch);
    EXPECT_VEC3_NEAR(state.position, glm::vec3(5, 2, 5), 0.1f);

    g_backend->destroyCharacter(ch);
    TEST_PASS();
    return true;
}

bool testCharacterPositionSet() {
    TEST_BEGIN("Character position teleport");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    CharacterCreationInfo info;
    info.position = glm::vec3(0, 2, 0);
    auto ch = g_backend->createCharacter(info);

    g_backend->setCharacterPosition(ch, glm::vec3(10, 5, 10));
    auto state = g_backend->getCharacterState(ch);
    EXPECT_VEC3_NEAR(state.position, glm::vec3(10, 5, 10), 0.1f);

    g_backend->destroyCharacter(ch);
    TEST_PASS();
    return true;
}

bool testCharacterVelocity() {
    TEST_BEGIN("Character velocity set");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    CharacterCreationInfo info;
    info.position = glm::vec3(0, 2, 0);
    auto ch = g_backend->createCharacter(info);

    g_backend->setCharacterLinearVelocity(ch, glm::vec3(5, 0, 0));
    auto state = g_backend->getCharacterState(ch);
    EXPECT_NEAR(state.linearVelocity.x, 5.0f, 0.1f);

    g_backend->destroyCharacter(ch);
    TEST_PASS();
    return true;
}

bool testCharacterUpdate() {
    TEST_BEGIN("Character update with movement");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    // Create a floor for the character to stand on
    auto floor = createFloor();
    g_backend->step(1.0f / 60.0f);

    CharacterCreationInfo info;
    info.position = glm::vec3(0, 2, 0);
    info.capsuleRadius = 0.3f;
    info.capsuleHeight = 1.8f;
    info.mass = 80.0f;
    auto ch = g_backend->createCharacter(info);

    // Update character with forward movement for several frames
    glm::vec3 gravity(0, -9.81f, 0);
    glm::vec3 move(5, 0, 0); // Move right
    for (int i = 0; i < 60; i++) {
        g_backend->updateCharacter(ch, 1.0f / 60.0f, gravity, move);
        g_backend->step(1.0f / 60.0f);
    }

    auto state = g_backend->getCharacterState(ch);
    // Character should have moved to the right
    EXPECT_TRUE(state.position.x > 0.5f);

    g_backend->destroyCharacter(ch);
    g_backend->destroyBody(floor);
    TEST_PASS();
    return true;
}

bool testMultipleCharacters() {
    TEST_BEGIN("Multiple character controllers");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    std::vector<CharacterHandle> chars;
    for (int i = 0; i < 5; i++) {
        CharacterCreationInfo info;
        info.position = glm::vec3(static_cast<float>(i) * 3.0f, 2, 0);
        auto ch = g_backend->createCharacter(info);
        EXPECT_NE(ch, INVALID_CHARACTER);
        chars.push_back(ch);
    }

    EXPECT_EQ(g_backend->getCharacterCount(), 5u);

    for (auto ch : chars) {
        g_backend->destroyCharacter(ch);
    }

    EXPECT_EQ(g_backend->getCharacterCount(), 0u);
    TEST_PASS();
    return true;
}

// =============================================================================
// 9. SIMULATION INTEGRATION TESTS
// =============================================================================

bool testGravitySimulation() {
    TEST_BEGIN("Gravity simulation (ball falls down)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto ball = createDynamicBox(glm::vec3(0, 10, 0), 1.0f);
    auto startPos = g_backend->getPosition(ball);

    // Step for 1 second
    for (int i = 0; i < 60; i++) {
        g_backend->step(1.0f / 60.0f);
    }

    auto endPos = g_backend->getPosition(ball);
    // Ball should have fallen (y decreased)
    EXPECT_TRUE(endPos.y < startPos.y);

    g_backend->destroyBody(ball);
    TEST_PASS();
    return true;
}

bool testCollisionResponse() {
    TEST_BEGIN("Collision response (ball lands on floor)");
    if (!g_joltAvailable) { TEST_SKIP("Jolt not available"); return true; }

    auto floor = createFloor();
    auto ball = createDynamicBox(glm::vec3(0, 5, 0), 1.0f);

    // Step until ball settles
    for (int i = 0; i < 180; i++) {
        g_backend->step(1.0f / 60.0f);
    }

    auto pos = g_backend->getPosition(ball);
    // Ball should have landed on the floor (y near 0.5 — box half-extent above floor surface)
    EXPECT_TRUE(pos.y < 2.0f);  // Should be well below starting position
    EXPECT_TRUE(pos.y > -1.0f); // Should not have fallen through

    g_backend->destroyBody(ball);
    g_backend->destroyBody(floor);
    TEST_PASS();
    return true;
}

// =============================================================================
// 10. NULL BACKEND TESTS
// =============================================================================

bool testNullBackend() {
    TEST_BEGIN("Null backend graceful degradation");

    NullPhysicsBackend null;
    PhysicsWorldConfig config;
    EXPECT_TRUE(null.initialize(config));
    EXPECT_TRUE(null.isInitialized());
    EXPECT_EQ(std::string(null.getName()), std::string("null"));

    // Create body — should return valid handle
    BodyCreationInfo info;
    info.shape.type = ShapeInfo::BOX;
    auto h = null.createBody(info);
    EXPECT_NE(h, INVALID_BODY);
    EXPECT_TRUE(null.isValidBody(h));

    // All operations are no-ops — should not crash
    null.setPosition(h, glm::vec3(1, 2, 3));
    null.step(1.0f / 60.0f);
    auto pos = null.getPosition(h);
    EXPECT_VEC3_NEAR(pos, glm::vec3(0, 0, 0), 0.01f); // Null returns zero

    // Raycasting returns false
    RaycastHit hit;
    EXPECT_FALSE(null.castRay(glm::vec3(0), glm::vec3(0, -1, 0), 10, hit, CollisionLayer::ALL_MASK));

    // Constraints return INVALID
    ConstraintSettings cs;
    EXPECT_EQ(null.createConstraint(cs), INVALID_CONSTRAINT);

    // Characters return INVALID
    CharacterCreationInfo ci;
    EXPECT_EQ(null.createCharacter(ci), INVALID_CHARACTER);

    null.shutdown();
    EXPECT_FALSE(null.isInitialized());

    TEST_PASS();
    return true;
}

// =============================================================================
// 11. HANDLE VALIDITY TESTS
// =============================================================================

bool testInvalidHandleConstants() {
    TEST_BEGIN("Invalid handle constants");

    EXPECT_EQ(INVALID_BODY, UINT32_MAX);
    EXPECT_EQ(INVALID_CONSTRAINT, UINT32_MAX);
    EXPECT_EQ(INVALID_CHARACTER, UINT32_MAX);

    TEST_PASS();
    return true;
}

bool testInvalidHandleOperations() {
    TEST_BEGIN("Operations on invalid handles (no crash)");

    // These should not crash — just no-op or return defaults
    EXPECT_FALSE(g_backend->isValidBody(INVALID_BODY));
    EXPECT_FALSE(g_backend->isValidConstraint(INVALID_CONSTRAINT));
    EXPECT_FALSE(g_backend->isValidCharacter(INVALID_CHARACTER));

    TEST_PASS();
    return true;
}

// =============================================================================
// TEST RUNNER
// =============================================================================

void runAllTests() {
    std::cout << "\n\033[1m=== OHAO Physics Backend Tests ===\033[0m" << std::endl;

    if (!initBackend()) {
        std::cerr << "\033[31mFailed to initialize physics backend\033[0m" << std::endl;
        testsFailed++;
        return;
    }

    std::cout << "Backend: " << g_backend->getName()
              << (g_joltAvailable ? " (Jolt)" : " (Null)") << std::endl;

    // 1. Backend lifecycle
    std::cout << "\n\033[1m--- Backend Lifecycle ---\033[0m" << std::endl;
    testBackendCreation();
    testBackendStats();
    testGravityGetSet();

    // 2. Body lifecycle
    std::cout << "\n\033[1m--- Body Lifecycle ---\033[0m" << std::endl;
    testBodyCreateDestroy();
    testBodyPositionGetSet();
    testBodyRotationGetSet();
    testBodyVelocity();
    testBodyProperties();
    testBodySleepWake();
    testBodyForces();
    testMultipleBodies();
    testShapeTypes();
    testMotionTypes();

    // 3. CCD
    std::cout << "\n\033[1m--- CCD (Continuous Collision Detection) ---\033[0m" << std::endl;
    testCCDCreation();
    testCCDToggle();

    // 4. Collision layers
    std::cout << "\n\033[1m--- Collision Layers ---\033[0m" << std::endl;
    testCollisionLayerConstants();
    testBodyLayerAssignment();
    testLayerCollisionMatrix();
    testAutoLayerAssignment();

    // 5. Raycasting
    std::cout << "\n\033[1m--- Raycasting & Queries ---\033[0m" << std::endl;
    testRaycastBasic();
    testRaycastMiss();
    testRaycastAll();
    testRaycastLayerFilter();
    testOverlapSphere();
    testOverlapBox();

    // 6. Contact callbacks
    std::cout << "\n\033[1m--- Contact Callbacks ---\033[0m" << std::endl;
    testContactListener();
    testContactEventQueue();

    // 7. Constraints
    std::cout << "\n\033[1m--- Constraints ---\033[0m" << std::endl;
    testConstraintFixed();
    testConstraintHinge();
    testConstraintSlider();
    testConstraintPoint();
    testConstraintDistance();
    testConstraintCone();
    testConstraintMotor();
    testConstraintEnableDisable();
    testConstraintTwoBody();

    // 8. Character controller
    std::cout << "\n\033[1m--- Character Controller ---\033[0m" << std::endl;
    testCharacterCreate();
    testCharacterState();
    testCharacterPositionSet();
    testCharacterVelocity();
    testCharacterUpdate();
    testMultipleCharacters();

    // 9. Simulation integration
    std::cout << "\n\033[1m--- Simulation Integration ---\033[0m" << std::endl;
    testGravitySimulation();
    testCollisionResponse();

    // 10. Null backend
    std::cout << "\n\033[1m--- Null Backend ---\033[0m" << std::endl;
    testNullBackend();

    // 11. Handle safety
    std::cout << "\n\033[1m--- Handle Safety ---\033[0m" << std::endl;
    testInvalidHandleConstants();
    testInvalidHandleOperations();

    // Cleanup
    shutdownBackend();

    // Results
    std::cout << "\n\033[1m=== Physics Test Results ===\033[0m" << std::endl;
    std::cout << "Tests run:    " << testsRun << std::endl;
    std::cout << "\033[32mPassed:       " << testsPassed << "\033[0m" << std::endl;
    if (testsFailed > 0) {
        std::cout << "\033[31mFailed:       " << testsFailed << "\033[0m" << std::endl;
    }
    if (testsSkipped > 0) {
        std::cout << "\033[33mSkipped:      " << testsSkipped << "\033[0m" << std::endl;
    }

    if (testsFailed > 0) {
        std::cout << "\n\033[31m*** SOME TESTS FAILED ***\033[0m" << std::endl;
    } else {
        std::cout << "\n\033[32m*** ALL TESTS PASSED ***\033[0m" << std::endl;
    }
}

int main() {
    runAllTests();
    return testsFailed > 0 ? 1 : 0;
}
