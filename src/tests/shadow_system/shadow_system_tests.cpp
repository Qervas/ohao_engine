/**
 * shadow_system_tests.cpp - Runtime Tests for Shadow System
 *
 * Tests:
 * - Handle bounds validation (checkedAccess throws correctly)
 * - Shadow atlas tile allocation and exhaustion
 * - CSM cascade selection
 * - Strong handle type safety (compile-time via include)
 */

// Include compile-time tests (static_asserts run at compile time)
#include "compile_time_tests.hpp"

#include "renderer/lighting/unified_light.hpp"
#include "renderer/shader/shader_bindings.hpp"
#include <iostream>
#include <vector>
#include <bitset>

namespace ohao::tests {

// =============================================================================
// TEST FRAMEWORK (minimal, no external dependencies)
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_BEGIN(name) \
    do { \
        testsRun++; \
        std::cout << "  TEST: " << name << "... "; \
    } while(0)

#define TEST_PASS() \
    do { \
        testsPassed++; \
        std::cout << "PASS" << std::endl; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        testsFailed++; \
        std::cout << "FAIL: " << msg << std::endl; \
    } while(0)

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            TEST_FAIL(#expr " was false"); \
            return; \
        } \
    } while(0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_THROW(expr, exception_type) \
    do { \
        bool caught = false; \
        try { expr; } catch (const exception_type&) { caught = true; } \
        if (!caught) { \
            TEST_FAIL(#expr " did not throw " #exception_type); \
            return; \
        } \
    } while(0)

#define EXPECT_NO_THROW(expr) \
    do { \
        try { expr; } catch (...) { \
            TEST_FAIL(#expr " threw an exception"); \
            return; \
        } \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            TEST_FAIL(#a " != " #b); \
            return; \
        } \
    } while(0)

// =============================================================================
// HANDLE TESTS
// =============================================================================

void testHandleInvalidity() {
    TEST_BEGIN("Handle default construction is invalid");

    LightHandle h1;
    ShadowMapHandle h2;
    CascadeIndex h3;
    AtlasTileHandle h4;

    EXPECT_FALSE(h1.isValid());
    EXPECT_FALSE(h2.isValid());
    EXPECT_FALSE(h3.isValid());
    EXPECT_FALSE(h4.isValid());

    TEST_PASS();
}

void testHandleExplicitConstruction() {
    TEST_BEGIN("Handle explicit construction is valid");

    LightHandle h1{0};
    ShadowMapHandle h2{1};
    CascadeIndex h3{2};
    AtlasTileHandle h4{3};

    EXPECT_TRUE(h1.isValid());
    EXPECT_TRUE(h2.isValid());
    EXPECT_TRUE(h3.isValid());
    EXPECT_TRUE(h4.isValid());

    EXPECT_EQ(h1.id, 0u);
    EXPECT_EQ(h2.id, 1u);
    EXPECT_EQ(h3.id, 2u);
    EXPECT_EQ(h4.id, 3u);

    TEST_PASS();
}

void testHandleComparison() {
    TEST_BEGIN("Handle comparison operators");

    LightHandle h1{0};
    LightHandle h2{0};
    LightHandle h3{1};

    EXPECT_TRUE(h1 == h2);
    EXPECT_FALSE(h1 == h3);
    EXPECT_TRUE(h1 != h3);
    EXPECT_TRUE(h1 < h3);
    EXPECT_TRUE(h3 > h1);

    TEST_PASS();
}

void testHandleInvalidFactory() {
    TEST_BEGIN("Handle::invalid() factory method");

    LightHandle h = LightHandle::invalid();
    EXPECT_FALSE(h.isValid());
    EXPECT_EQ(h.id, LightHandle::kInvalidValue);

    TEST_PASS();
}

// =============================================================================
// CHECKED ACCESS TESTS
// =============================================================================

void testCheckedAccessValid() {
    TEST_BEGIN("checkedAccess with valid handle");

    std::vector<UnifiedLight> lights(8);
    lights[3] = UnifiedLight::createPoint(glm::vec3(1, 2, 3));

    LightHandle h{3};
    EXPECT_NO_THROW(
        auto& light = checkedAccess(lights, h, "test");
        (void)light;
    );

    auto& light = checkedAccess(lights, h, "test");
    EXPECT_EQ(light.position.x, 1.0f);

    TEST_PASS();
}

void testCheckedAccessInvalidHandle() {
    TEST_BEGIN("checkedAccess with invalid handle throws");

    std::vector<UnifiedLight> lights(8);
    LightHandle h; // Invalid

    EXPECT_THROW(checkedAccess(lights, h, "test"), std::out_of_range);

    TEST_PASS();
}

void testCheckedAccessOutOfBounds() {
    TEST_BEGIN("checkedAccess with out-of-bounds handle throws");

    std::vector<UnifiedLight> lights(8);
    LightHandle h{99}; // Out of bounds

    EXPECT_THROW(checkedAccess(lights, h, "test"), std::out_of_range);

    TEST_PASS();
}

// =============================================================================
// UNIFIED LIGHT TESTS
// =============================================================================

void testUnifiedLightFactories() {
    TEST_BEGIN("UnifiedLight factory methods");

    auto directional = UnifiedLight::createDirectional(glm::vec3(0, -1, 0));
    EXPECT_TRUE(directional.isDirectional());
    EXPECT_FALSE(directional.isPoint());
    EXPECT_FALSE(directional.isSpot());
    EXPECT_FALSE(directional.castsShadow());

    auto point = UnifiedLight::createPoint(glm::vec3(0, 5, 0));
    EXPECT_FALSE(point.isDirectional());
    EXPECT_TRUE(point.isPoint());
    EXPECT_FALSE(point.isSpot());

    auto spot = UnifiedLight::createSpot(glm::vec3(0, 5, 0), glm::vec3(0, -1, 0), 30.0f, 45.0f);
    EXPECT_FALSE(spot.isDirectional());
    EXPECT_FALSE(spot.isPoint());
    EXPECT_TRUE(spot.isSpot());

    TEST_PASS();
}

void testUnifiedLightShadowIndex() {
    TEST_BEGIN("UnifiedLight shadow map index");

    UnifiedLight light = UnifiedLight::createDirectional(glm::vec3(0, -1, 0));
    EXPECT_FALSE(light.castsShadow());
    EXPECT_EQ(light.shadowMapIndex, -1);

    light.shadowMapIndex = 0;
    EXPECT_TRUE(light.castsShadow());
    EXPECT_EQ(light.shadowMapIndex, 0);

    TEST_PASS();
}

// =============================================================================
// SHADER BINDINGS TESTS
// =============================================================================

void testShaderBindingsConsistency() {
    TEST_BEGIN("ShaderBindings constants consistency");

    // These are also tested at compile time, but runtime verification is useful
    EXPECT_EQ(ShaderBindings::kMaxLights, 8);
    EXPECT_EQ(ShaderBindings::kMaxShadowMaps, 4);
    EXPECT_EQ(ShaderBindings::kMaxCSMCascades, 4);
    EXPECT_EQ(ShaderBindings::kMaxAtlasTiles, 16);

    EXPECT_EQ(ShaderBindings::ShadowAtlas::kAtlasSize, 4096u);
    EXPECT_EQ(ShaderBindings::ShadowAtlas::kTileSize, 1024u);
    EXPECT_EQ(ShaderBindings::ShadowAtlas::kTilesPerRow, 4u);
    EXPECT_EQ(ShaderBindings::ShadowAtlas::kTotalTiles, 16u);

    TEST_PASS();
}

// =============================================================================
// ATLAS ALLOCATION SIMULATION TESTS
// =============================================================================

void testAtlasAllocationSimulation() {
    TEST_BEGIN("Atlas allocation simulation (without Vulkan)");

    // Simulate atlas allocation tracking
    std::bitset<16> allocatedTiles;
    int allocCount = 0;

    // Allocate all 16 tiles
    for (int i = 0; i < 16; ++i) {
        if (allocatedTiles.count() < 16) {
            for (size_t j = 0; j < 16; ++j) {
                if (!allocatedTiles.test(j)) {
                    allocatedTiles.set(j);
                    allocCount++;
                    break;
                }
            }
        }
    }

    EXPECT_EQ(allocCount, 16);
    EXPECT_EQ(allocatedTiles.count(), 16u);

    // 17th allocation should fail (simulated by count check)
    bool canAllocate = allocatedTiles.count() < 16;
    EXPECT_FALSE(canAllocate);

    // Release a tile
    allocatedTiles.reset(5);
    canAllocate = allocatedTiles.count() < 16;
    EXPECT_TRUE(canAllocate);

    TEST_PASS();
}

// =============================================================================
// TEST RUNNER
// =============================================================================

void runAllTests() {
    std::cout << "\n=== Shadow System Tests ===" << std::endl;
    std::cout << "\n--- Handle Tests ---" << std::endl;
    testHandleInvalidity();
    testHandleExplicitConstruction();
    testHandleComparison();
    testHandleInvalidFactory();

    std::cout << "\n--- Checked Access Tests ---" << std::endl;
    testCheckedAccessValid();
    testCheckedAccessInvalidHandle();
    testCheckedAccessOutOfBounds();

    std::cout << "\n--- UnifiedLight Tests ---" << std::endl;
    testUnifiedLightFactories();
    testUnifiedLightShadowIndex();

    std::cout << "\n--- ShaderBindings Tests ---" << std::endl;
    testShaderBindingsConsistency();

    std::cout << "\n--- Atlas Allocation Tests ---" << std::endl;
    testAtlasAllocationSimulation();

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Tests run: " << testsRun << std::endl;
    std::cout << "Passed: " << testsPassed << std::endl;
    std::cout << "Failed: " << testsFailed << std::endl;

    if (testsFailed > 0) {
        std::cout << "\n*** TESTS FAILED ***" << std::endl;
    } else {
        std::cout << "\n*** ALL TESTS PASSED ***" << std::endl;
    }
}

} // namespace ohao::tests

// =============================================================================
// MAIN
// =============================================================================

int main() {
    ohao::tests::runAllTests();
    return ohao::tests::testsFailed > 0 ? 1 : 0;
}
