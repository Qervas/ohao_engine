/**
 * audio_system_tests.cpp - Tests for the AudioSystem (miniaudio backend)
 *
 * Tests initialization, category/master volume control, handle management,
 * and graceful shutdown.
 *
 * Note: Actual sound playback tests are limited since CI may lack audio devices.
 * We test the API surface and state management, not audible output.
 */

#include <iostream>
#include <cmath>

#include "audio/audio_system.hpp"

using namespace ohao;

// =============================================================================
// TEST FRAMEWORK
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

#define EXPECT_NEAR(a, b, eps) \
    do { \
        if (std::abs((a) - (b)) > (eps)) { \
            TEST_FAIL(#a " not near " #b); \
            return false; \
        } \
    } while(0)

// =============================================================================
// TESTS
// =============================================================================

bool testAudioSystemInit() {
    TEST_BEGIN("AudioSystem initialization");

    AudioSystem audio;
    EXPECT_FALSE(audio.isInitialized());

    bool ok = audio.initialize();
    if (!ok) {
        TEST_SKIP("No audio device available");
        return true;
    }

    EXPECT_TRUE(audio.isInitialized());
    audio.shutdown();
    EXPECT_FALSE(audio.isInitialized());

    TEST_PASS();
    return true;
}

bool testAudioDoubleInit() {
    TEST_BEGIN("AudioSystem double init/shutdown safety");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    // Shutdown twice — should not crash
    audio.shutdown();
    audio.shutdown();
    EXPECT_FALSE(audio.isInitialized());

    TEST_PASS();
    return true;
}

bool testMasterVolume() {
    TEST_BEGIN("Master volume get/set");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    EXPECT_NEAR(audio.getMasterVolume(), 1.0f, 0.001f);

    audio.setMasterVolume(0.5f);
    EXPECT_NEAR(audio.getMasterVolume(), 0.5f, 0.001f);

    audio.setMasterVolume(0.0f);
    EXPECT_NEAR(audio.getMasterVolume(), 0.0f, 0.001f);

    audio.setMasterVolume(1.0f);
    EXPECT_NEAR(audio.getMasterVolume(), 1.0f, 0.001f);

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testCategoryVolume() {
    TEST_BEGIN("Category volume get/set");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    // All categories should default to 1.0
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::SFX), 1.0f, 0.001f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::Music), 1.0f, 0.001f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::Ambient), 1.0f, 0.001f);

    audio.setCategoryVolume(SoundCategory::SFX, 0.3f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::SFX), 0.3f, 0.001f);

    audio.setCategoryVolume(SoundCategory::Music, 0.7f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::Music), 0.7f, 0.001f);

    audio.setCategoryVolume(SoundCategory::Ambient, 0.0f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::Ambient), 0.0f, 0.001f);

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testSoundHandleConstants() {
    TEST_BEGIN("Sound handle constants");

    EXPECT_EQ(INVALID_SOUND_HANDLE, 0u);

    TEST_PASS();
    return true;
}

bool testPlayInvalidPath() {
    TEST_BEGIN("Play nonexistent file returns INVALID_SOUND_HANDLE");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    auto handle = audio.playSound("/nonexistent/path/foo.wav");
    EXPECT_EQ(handle, INVALID_SOUND_HANDLE);

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testStopInvalidHandle() {
    TEST_BEGIN("Stop/pause/resume invalid handle (no crash)");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    // These should all be no-ops on invalid handles
    audio.stopSound(INVALID_SOUND_HANDLE);
    audio.pauseSound(INVALID_SOUND_HANDLE);
    audio.resumeSound(INVALID_SOUND_HANDLE);
    audio.setSoundVolume(INVALID_SOUND_HANDLE, 0.5f);
    audio.setSoundPosition(INVALID_SOUND_HANDLE, glm::vec3(1, 2, 3));

    // Also test with a made-up handle
    audio.stopSound(999);
    audio.pauseSound(999);
    audio.resumeSound(999);

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testStopAllSounds() {
    TEST_BEGIN("Stop all sounds");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    audio.stopAll();

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testCategoryControl() {
    TEST_BEGIN("Category-wide stop/pause/resume (no crash)");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    audio.stopCategory(SoundCategory::SFX);
    audio.pauseCategory(SoundCategory::Music);
    audio.resumeCategory(SoundCategory::Ambient);

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testListenerPosition() {
    TEST_BEGIN("Listener position set (no crash)");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    audio.setListenerPosition(
        glm::vec3(0, 1.7f, 0),     // position
        glm::vec3(0, 0, -1),       // forward
        glm::vec3(0, 1, 0)         // up
    );

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testUpdateCleanup() {
    TEST_BEGIN("Update (cleanup finished sounds)");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    // Calling update with no active sounds should be fine
    audio.update();
    audio.update();
    audio.update();

    audio.shutdown();
    TEST_PASS();
    return true;
}

bool testSoundCategoryEnum() {
    TEST_BEGIN("SoundCategory enum values");

    EXPECT_EQ(static_cast<int>(SoundCategory::SFX), 0);
    EXPECT_EQ(static_cast<int>(SoundCategory::Music), 1);
    EXPECT_EQ(static_cast<int>(SoundCategory::Ambient), 2);

    TEST_PASS();
    return true;
}

// =============================================================================
// TEST RUNNER
// =============================================================================

void runAllTests() {
    std::cout << "\n\033[1m=== OHAO Audio System Tests ===\033[0m" << std::endl;

    std::cout << "\n\033[1m--- Initialization ---\033[0m" << std::endl;
    testAudioSystemInit();
    testAudioDoubleInit();

    std::cout << "\n\033[1m--- Volume Control ---\033[0m" << std::endl;
    testMasterVolume();
    testCategoryVolume();

    std::cout << "\n\033[1m--- Handle Safety ---\033[0m" << std::endl;
    testSoundHandleConstants();
    testPlayInvalidPath();
    testStopInvalidHandle();

    std::cout << "\n\033[1m--- Category & Global Control ---\033[0m" << std::endl;
    testStopAllSounds();
    testCategoryControl();

    std::cout << "\n\033[1m--- Listener & Update ---\033[0m" << std::endl;
    testListenerPosition();
    testUpdateCleanup();

    std::cout << "\n\033[1m--- Enum Values ---\033[0m" << std::endl;
    testSoundCategoryEnum();

    // Results
    std::cout << "\n\033[1m=== Audio Test Results ===\033[0m" << std::endl;
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
