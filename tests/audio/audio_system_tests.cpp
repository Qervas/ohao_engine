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
#include <string_view>

#include "audio/audio_bank.hpp"
#include "audio/audio_system.hpp"

#include "common/harness.hpp"   // the JSON summary line (ohao::test::finish)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define OHAO_GETPID _getpid
#else
#include <unistd.h>
#define OHAO_GETPID getpid
#endif

using namespace ohao;

// =============================================================================
// TEST FRAMEWORK
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;
static int testsSkipped = 0;
static std::string g_currentTest;            // the TEST_BEGIN name, for the failure list
static std::vector<std::string> g_failures;  // reported in the JSON summary line

#define TEST_BEGIN(name) \
    do { \
        testsRun++; \
        g_currentTest = (name); \
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
        g_failures.push_back(g_currentTest + ": " + std::string(msg)); \
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
    TEST_BEGIN("Sound handle constants + category helpers");

    EXPECT_EQ(INVALID_SOUND_HANDLE, 0u);
    EXPECT_FALSE(isValidSoundHandle(INVALID_SOUND_HANDLE));
    EXPECT_TRUE(isValidSoundHandle(1u));
    EXPECT_TRUE(isValidSoundCategory(SoundCategory::SFX));
    EXPECT_FALSE(isValidSoundCategory(SoundCategory::Count));
    EXPECT_EQ(soundCategoryName(SoundCategory::Music), std::string_view("music"));
    EXPECT_EQ(soundCategoryIndex(SoundCategory::Ambient), 2);

    TEST_PASS();
    return true;
}

bool testVolumeClamp() {
    TEST_BEGIN("Master/category volume clamp to [0,1]");

    AudioSystem audio;
    bool ok = audio.initialize();
    if (!ok) { TEST_SKIP("No audio device"); return true; }

    audio.setMasterVolume(2.5f);
    EXPECT_NEAR(audio.getMasterVolume(), 1.0f, 0.001f);
    audio.setMasterVolume(-1.0f);
    EXPECT_NEAR(audio.getMasterVolume(), 0.0f, 0.001f);
    audio.setCategoryVolume(SoundCategory::SFX, 5.0f);
    EXPECT_NEAR(audio.getCategoryVolume(SoundCategory::SFX), 1.0f, 0.001f);
    EXPECT_EQ(audio.activeSoundCount(), 0u);

    audio.shutdown();
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
// TEST 26: THE MIXER (sub-project 11 step 1, docs/specs/2026-10-28-audio-design.md)
//
// Everything renders through the deviceless engine (ma_engine_read_pcm_frames), 48 kHz stereo,
// from synthetic sounds written as float WAVs into a per-process folder.
// =============================================================================

namespace t26 {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kStep = kRate / 60;   // one 60 Hz step: 800 frames

namespace fs = std::filesystem;

void writeWav(const fs::path& path, const std::vector<float>& mono) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(mono.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3 /* IEEE float */); u16(1); u32(kRate); u32(kRate * 4); u16(4); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(mono.data()), bytes);
}

// The synthetic sounds, written once per process: dc (0.5 for 3 s), click (one 1.0 sample, then
// silence), tone440 and tone100 (0.5 for 2 s), noise (white, 0.3, a fixed generator, 2 s), and the
// local-first pair: local/var_1, base/var_1, base/var_2.
const fs::path& soundRoot() {
    static const fs::path root = [] {
        const fs::path r = fs::temp_directory_path() / ("ohao_audio_tests_" + std::to_string(OHAO_GETPID()));
        fs::create_directories(r / "base");
        fs::create_directories(r / "local");
        const auto n = [](float seconds) { return static_cast<std::size_t>(seconds * kRate); };
        std::vector<float> dc(n(3.0f), 0.5f), click(4800, 0.0f), t440(n(2.0f)), t100(n(2.0f)), noise(n(2.0f));
        click[0] = 1.0f;
        for (std::size_t i = 0; i < t440.size(); ++i) {
            t440[i] = 0.5f * std::sin(6.283185307f * 440.0f * float(i) / kRate);
            t100[i] = 0.5f * std::sin(6.283185307f * 100.0f * float(i) / kRate);
        }
        uint32_t s = 12345u;
        for (float& x : noise) { s = s * 1664525u + 1013904223u; x = 0.3f * (float(s >> 8) / 8388608.0f - 1.0f); }
        writeWav(r / "base" / "dc.wav", dc);
        writeWav(r / "base" / "click.wav", click);
        writeWav(r / "base" / "tone440.wav", t440);
        writeWav(r / "base" / "tone100.wav", t100);
        writeWav(r / "base" / "noise.wav", noise);
        writeWav(r / "base" / "var_1.wav", click);
        writeWav(r / "base" / "var_2.wav", click);
        writeWav(r / "local" / "var_1.wav", dc);
        return r;
    }();
    return root;
}

// A test bank: master <- world <- {weapons, foley}, master <- ui, master <- reverb (a return).
constexpr const char* kBank = R"({
  "folders": ["local", "base"],
  "buses": {
    "master": {}, "world": {"parent": "master"},
    "weapons": {"parent": "world", "cap": 12}, "foley": {"parent": "world", "cap": 10},
    "ui": {"parent": "master", "cap": 4}, "reverb": {"parent": "master", "return": true}
  },
  "voices": 32,
  "shapes": {"near": {"min": 0.5, "max": 20, "rolloff": 1.5}},
  "sounds": {
    "dc":      {"files": "dc", "bus": "foley", "priority": 100, "shape": "ear", "holdMs": 3000},
    "click":   {"files": "click", "bus": "weapons", "priority": 200, "shape": "ear", "holdMs": 100},
    "tone440": {"files": "tone440", "bus": "weapons", "priority": 200, "shape": "ear", "holdMs": 2000},
    "tone100": {"files": "tone100", "bus": "ui", "priority": 50, "shape": "ear", "holdMs": 2000},
    "noise":   {"files": "noise", "bus": "weapons", "priority": 200, "shape": "near", "holdMs": 2000},
    "var":     {"files": "var_{n}", "bus": "foley", "priority": 100, "shape": "ear", "holdMs": 100}
  }
})";

AudioBank testBank() {
    auto r = parseAudioBank(kBank);
    if (!r) {
        for (const auto& e : r.error()) std::cout << "\n      bank error " << e.path << ": " << e.message;
        return {};
    }
    return std::move(r).value();
}

// A deviceless mixer with the test bank loaded; false if either fails.
bool mixer(AudioSystem& a) {
    return a.initialize(true) && a.loadBank(testBank(), soundRoot());
}

// Renders `frames` stereo frames; returns the left channel.
std::vector<float> renderLeft(AudioSystem& a, uint32_t frames) {
    std::vector<float> st(std::size_t(frames) * 2), left(frames);
    a.render(st, frames);
    for (uint32_t i = 0; i < frames; ++i) left[i] = st[std::size_t(i) * 2];
    return left;
}

double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) s += double(x[i]) * x[i];
    return to > from ? std::sqrt(s / double(to - from)) : 0.0;
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

// The power of `x` at `hz` (Goertzel), normalised by length.
double goertzel(const std::vector<float>& x, double hz, std::size_t from, std::size_t to) {
    const double w = 2.0 * 3.14159265358979 * hz / kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double n = double(to - from);
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (n * n);
}

// The mean power over bins from lo to hi Hz, every `step` Hz.
double bandPower(const std::vector<float>& x, double lo, double hi, std::size_t from, std::size_t to,
                 double step = 250.0) {
    double sum = 0.0;
    int bins = 0;
    for (double f = lo; f <= hi; f += step, ++bins) sum += goertzel(x, f, from, to);
    return bins ? sum / bins : 0.0;
}

VoicePlacement atEar(float volume = 1.0f) {
    VoicePlacement p;
    p.volume = volume;
    return p;
}

} // namespace t26

bool t26_bank_parse_errors() {
    TEST_BEGIN("t26_bank_parse_errors");
    struct Case { const char* json; const char* path; };
    const Case cases[] = {
        {R"({"buses": {"master": {}}, "frobnicate": 1})", "/frobnicate"},
        {R"({"buses": {"master": {}, "world": {"parent": "nope"}}})", "/buses/world/parent"},
        {R"({"buses": {"master": {}, "a": {"parent": "b"}, "b": {"parent": "a"}}})", "/buses/a/parent"},
        {R"({"buses": {"master": {}, "y": {"parent": "master", "cap": 0}}})", "/buses/y/cap"},
        {R"({"buses": {"master": {}, "r": {"parent": "master", "return": true}}, "sounds": {"x": {"files": "x", "bus": "r"}}})", "/sounds/x/bus"},
        {R"({"buses": {"master": {}}, "sounds": {"x": {"files": "x", "bus": "master", "shape": "wobbly"}}})", "/sounds/x/shape"},
        {R"({"buses": {"master": {}}, "sounds": {"x": {"files": "x", "bus": "master", "priority": 300}}})", "/sounds/x/priority"},
        {R"({"buses": {"master": {}}, "sounds": {"x": {"files": "x", "bus": "master", "holdMs": 0}}})", "/sounds/x/holdMs"},
        {R"({"buses": {"master": {}}, "sounds": {"x": {"bus": "master"}}})", "/sounds/x/files"},
        {R"({"buses": {"master": {}}, "ducks": [{"on": "nope", "bus": "master", "db": -6}]})", "/ducks/0/on"},
        {R"({"buses": {"master": {}}, "sounds": {"x": {"files": "x", "bus": "master"}}, "ducks": [{"on": "x", "bus": "nope", "db": -6}]})", "/ducks/0/bus"},
        {R"({"buses": {"master": {}}, "snapshots": {"dead": {"buses": {"nope": {"db": -9}}}}})", "/snapshots/dead/buses/nope"},
        {R"({"buses": {"master": {}, "other": {}}})", "/buses"},
        {R"({"buses": {"master": {}}, "voices": 0})", "/voices"},
        {R"({"buses": {"master": {}}, "defaultZone": "cave"})", "/defaultZone"},
    };
    for (const Case& c : cases) {
        const auto r = parseAudioBank(c.json);
        bool found = false;
        if (!r)
            for (const auto& e : r.error()) found = found || e.path == c.path;
        if (!found) {
            TEST_FAIL(std::string("no error at ") + c.path);
            return false;
        }
    }
    EXPECT_TRUE(static_cast<bool>(parseAudioBank(R"({"buses": {"master": {}}})")));
    EXPECT_TRUE(static_cast<bool>(parseAudioBank(t26::kBank)));
    TEST_PASS();
    return true;
}

// The committed slice bank parses, every sound resolves to at least one file, and Feel's values
// hold: the gunfire and near curves (Feel's "Audio quick wins").
bool t26_slice_bank() {
    TEST_BEGIN("t26_slice_bank");
    const std::filesystem::path root = OHAO_SOURCE_DIR;
    std::ifstream f(root / "assets/audio/slice.audio.json", std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(f));
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = parseAudioBank(json);
    if (!r) {
        for (const auto& e : r.error()) std::cout << "\n      " << e.path << ": " << e.message;
        TEST_FAIL("the slice bank does not parse");
        return false;
    }
    const AudioBank& bank = r.value();
    const ShapeDesc* gun = bank.shape("gunfire");
    const ShapeDesc* near = bank.shape("near");
    EXPECT_TRUE(gun && near);
    EXPECT_NEAR(gun->minDistance, 1.0f, 0.0f);
    EXPECT_NEAR(gun->maxDistance, 60.0f, 0.0f);
    EXPECT_NEAR(gun->rolloff, 1.0f, 0.0f);
    EXPECT_NEAR(near->minDistance, 0.5f, 0.0f);
    EXPECT_NEAR(near->maxDistance, 20.0f, 0.0f);
    EXPECT_NEAR(near->rolloff, 1.5f, 0.0f);
    EXPECT_EQ(bank.voices, 32u);
    AudioSystem a;
    EXPECT_TRUE(a.initialize(true));
    EXPECT_TRUE(a.loadBank(bank, root));
    for (std::size_t s = 0; s < bank.sounds.size(); ++s)
        if (a.variantCount(static_cast<uint16_t>(s)) == 0) {
            TEST_FAIL("no file for " + bank.sounds[s].name);
            return false;
        }
    TEST_PASS();
    return true;
}

// The default bank is Feel's mixer: master over sfx (24 voices), music and ambient; the category
// API plays into those buses.
bool t26_default_bank_categories() {
    TEST_BEGIN("t26_default_bank_categories");
    const AudioBank d = defaultBank();
    EXPECT_TRUE(d.bus("master") >= 0 && d.bus("sfx") >= 0 && d.bus("music") >= 0 && d.bus("ambient") >= 0);
    EXPECT_EQ(d.buses[static_cast<std::size_t>(d.bus("sfx"))].cap, 24u);
    AudioSystem a;
    EXPECT_TRUE(a.initialize(true));
    const std::string tone = (t26::soundRoot() / "base" / "tone440.wav").string();
    EXPECT_TRUE(isValidSoundHandle(a.playSound(tone, SoundCategory::SFX)));
    const double on = t26::rms(t26::renderLeft(a, 4800), 960);
    EXPECT_TRUE(on > 0.1);
    a.setCategoryVolume(SoundCategory::SFX, 0.0f);
    const double off = t26::rms(t26::renderLeft(a, 4800), 960);
    EXPECT_TRUE(off < 1e-4);
    a.setCategoryVolume(SoundCategory::SFX, 1.0f);
    a.setCategoryVolume(SoundCategory::Music, 0.0f);   // another bus: the tone plays on
    const double back = t26::rms(t26::renderLeft(a, 4800), 960);
    EXPECT_NEAR(back, on, on * 1e-3);
    TEST_PASS();
    return true;
}

// A bus's gain scales only its subtree's output.
bool t26_bus_tree() {
    TEST_BEGIN("t26_bus_tree");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const uint16_t tone = static_cast<uint16_t>(a.bank().sound("tone440"));
    EXPECT_TRUE(isValidSoundHandle(a.play(tone, 0, t26::atEar())));
    const double r0 = t26::rms(t26::renderLeft(a, 9600), 960);
    a.setBus(static_cast<uint8_t>(a.bank().bus("world")), -6.0f, 0.0, 0);
    const double r1 = t26::rms(t26::renderLeft(a, 9600), 960);
    a.setBus(static_cast<uint8_t>(a.bank().bus("ui")), -20.0f, 0.0, 0);   // not the tone's
    const double r2 = t26::rms(t26::renderLeft(a, 9600), 960);
    std::cout << "\n      world -6 dB: " << t26::db(r1 / r0) << " dB; ui -20 dB: " << t26::db(r2 / r1) << " dB ";
    EXPECT_NEAR(t26::db(r1 / r0), -6.0, 0.1);
    EXPECT_NEAR(t26::db(r2 / r1), 0.0, 0.01);
    TEST_PASS();
    return true;
}

// Mute silences a bus's subtree; solo leaves only the soloed bus, its ancestors and descendants.
bool t26_mute_solo() {
    TEST_BEGIN("t26_mute_solo");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const auto& bank = a.bank();
    EXPECT_TRUE(isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("tone440")), 0, t26::atEar())));   // weapons
    EXPECT_TRUE(isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("tone100")), 0, t26::atEar())));   // ui
    const auto both = [&](double& p440, double& p100) {
        const auto x = t26::renderLeft(a, 9600);
        p440 = t26::goertzel(x, 440.0, 2400, 9600);
        p100 = t26::goertzel(x, 100.0, 2400, 9600);
    };
    double w0, u0, w1, u1, w2, u2, w3, u3;
    both(w0, u0);
    EXPECT_TRUE(w0 > 1e-3 && u0 > 1e-3);
    const uint8_t world = static_cast<uint8_t>(bank.bus("world")), weapons = static_cast<uint8_t>(bank.bus("weapons"));
    a.setMute(world, true);
    both(w1, u1);
    EXPECT_TRUE(w1 < w0 * 1e-6 && std::abs(u1 - u0) < u0 * 1e-3);
    a.setMute(world, false);
    a.setSolo(weapons, true);
    both(w2, u2);
    EXPECT_TRUE(std::abs(w2 - w0) < w0 * 1e-3 && u2 < u0 * 1e-6);
    a.setSolo(weapons, false);
    both(w3, u3);
    EXPECT_TRUE(std::abs(w3 - w0) < w0 * 1e-3 && std::abs(u3 - u0) < u0 * 1e-3);
    TEST_PASS();
    return true;
}

// Two voices scheduled 800 frames apart start 800 frames apart, to the frame.
bool t26_start_time_sample_accurate() {
    TEST_BEGIN("t26_start_time_sample_accurate");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const uint16_t click = static_cast<uint16_t>(a.bank().sound("click"));
    const uint64_t f0 = a.engineFrame();
    EXPECT_TRUE(isValidSoundHandle(a.play(click, 0, t26::atEar(), f0 + 1000)));
    EXPECT_TRUE(isValidSoundHandle(a.play(click, 0, t26::atEar(), f0 + 1000 + t26::kStep)));
    const auto x = t26::renderLeft(a, 4000);
    std::vector<std::size_t> onsets;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > 0.3f) onsets.push_back(i);
    std::cout << "\n      onsets:";
    for (std::size_t o : onsets) std::cout << ' ' << o;
    std::cout << ' ';
    EXPECT_EQ(onsets.size(), 2u);
    EXPECT_EQ(onsets[0], 1000u);
    EXPECT_EQ(onsets[1], 1000u + t26::kStep);
    TEST_PASS();
    return true;
}

// A duck's gain follows attack and release, linear in dB, within 0.2 dB at every step boundary.
bool t26_duck_envelope() {
    TEST_BEGIN("t26_duck_envelope");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const auto& bank = a.bank();
    EXPECT_TRUE(isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("dc")), 0, t26::atEar())));
    const auto pre = t26::renderLeft(a, 960);
    const double unit = pre.back();   // the dc's level at the output before any duck
    EXPECT_TRUE(unit > 0.1);
    const uint8_t foley = static_cast<uint8_t>(bank.bus("foley"));
    a.setBus(foley, -6.0f, 0.0, t26::kStep);   // attack: one step
    const auto attack = t26::renderLeft(a, t26::kStep);
    const auto hold = t26::renderLeft(a, 2 * t26::kStep);
    a.setBus(foley, 0.0f, 0.0, 15 * t26::kStep);   // release: 15 steps
    const auto release = t26::renderLeft(a, 15 * t26::kStep);
    const auto gainDb = [&](float v) { return t26::db(double(v) / unit); };
    EXPECT_NEAR(gainDb(attack[t26::kStep / 2]), -3.0, 0.2);
    EXPECT_NEAR(gainDb(attack.back()), -6.0, 0.2);
    EXPECT_NEAR(gainDb(hold.back()), -6.0, 0.05);
    for (uint32_t k = 1; k <= 15; ++k)
        if (std::abs(gainDb(release[k * t26::kStep - 1]) - (-6.0 + 6.0 * k / 15.0)) > 0.2) {
            TEST_FAIL("release off at step " + std::to_string(k) + ": " + std::to_string(gainDb(release[k * t26::kStep - 1])));
            return false;
        }
    TEST_PASS();
    return true;
}

// `dead`'s bus change: -9 dB and a 900 Hz low-pass over 18 steps. The gain is linear in dB over the
// blend; afterwards a low band is down 9 dB and the band above 2 kHz at least 12 dB.
bool t26_snapshot_blend() {
    TEST_BEGIN("t26_snapshot_blend");
    const uint32_t blend = 18 * t26::kStep;
    {   // the gain over the blend, on dc (which the low-pass passes)
        AudioSystem a;
        EXPECT_TRUE(t26::mixer(a));
        const auto& bank = a.bank();
        EXPECT_TRUE(isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("dc")), 0, t26::atEar())));
        const double unit = t26::renderLeft(a, 960).back();
        a.setBus(static_cast<uint8_t>(bank.bus("world")), -9.0f, 900.0, blend);
        const auto x = t26::renderLeft(a, blend + 960);
        const auto gainDb = [&](std::size_t i) { return t26::db(double(x[i]) / unit); };
        EXPECT_NEAR(gainDb(blend / 2), -4.5, 0.2);
        EXPECT_NEAR(gainDb(blend + 900), -9.0, 0.1);
    }
    {   // the spectrum after the blend, on noise: the same frames of it with and without `dead`
        const auto render = [&](bool dead) {
            AudioSystem a;
            if (!t26::mixer(a)) return std::vector<float>{};
            const auto& bank = a.bank();
            if (!isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("noise")), 0, t26::atEar()))) return std::vector<float>{};
            if (dead) a.setBus(static_cast<uint8_t>(bank.bus("world")), -9.0f, 900.0, blend);
            (void)t26::renderLeft(a, blend + 960);
            return t26::renderLeft(a, 9600);
        };
        const auto before = render(false), after = render(true);
        EXPECT_TRUE(before.size() == 9600 && after.size() == 9600);
        const double low = t26::db(std::sqrt(t26::bandPower(after, 100.0, 300.0, 0, 9600, 25.0) /
                                             t26::bandPower(before, 100.0, 300.0, 0, 9600, 25.0)));
        const double high = t26::db(std::sqrt(t26::bandPower(after, 2000.0, 10000.0, 0, 9600) /
                                              t26::bandPower(before, 2000.0, 10000.0, 0, 9600)));
        std::cout << "\n      low band " << low << " dB, above 2 kHz " << high << " dB ";
        EXPECT_NEAR(low, -9.0, 1.0);
        EXPECT_TRUE(high <= -12.0);
    }
    TEST_PASS();
    return true;
}

// A stopped voice fades over 2 ms (96 frames): no sample step over 0.02, silent after the fade.
bool t26_steal_fade() {
    TEST_BEGIN("t26_steal_fade");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const SoundHandle v = a.play(static_cast<uint16_t>(a.bank().sound("dc")), 0, t26::atEar());
    EXPECT_TRUE(isValidSoundHandle(v));
    const auto pre = t26::renderLeft(a, 960);
    EXPECT_TRUE(pre.back() > 0.1f);
    a.stop(v, 96);
    const auto x = t26::renderLeft(a, 960);
    float step = std::abs(x[0] - pre.back());
    for (std::size_t i = 1; i < x.size(); ++i) step = std::max(step, std::abs(x[i] - x[i - 1]));
    std::cout << "\n      largest sample step " << step << ' ';
    EXPECT_TRUE(step < 0.02f);
    EXPECT_TRUE(std::abs(x[200]) < 1e-6f && std::abs(x.back()) < 1e-6f);
    a.update();   // the faded voice is freed
    EXPECT_EQ(a.activeSoundCount(), 0u);
    TEST_PASS();
    return true;
}

// The same plays and bus changes rendered by two mixers: bit for bit.
bool t26_render_repeatable() {
    TEST_BEGIN("t26_render_repeatable");
    const auto run = [](std::vector<float>& out) {
        AudioSystem a;
        if (!t26::mixer(a)) return false;
        const auto& bank = a.bank();
        a.setListenerPosition({0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0});
        VoicePlacement far;
        far.spatial = true;
        far.position = {3.0f, 1.0f, -6.0f};
        far.shape = SoundShape{0.5f, 20.0f, 1.5f, true};   // occluded: through the low-pass
        far.volume = 0.7f;
        if (!isValidSoundHandle(a.play(static_cast<uint16_t>(bank.sound("noise")), 0, far))) return false;
        const uint64_t f0 = a.engineFrame();
        (void)a.play(static_cast<uint16_t>(bank.sound("click")), 0, t26::atEar(), f0 + 333);
        (void)a.play(static_cast<uint16_t>(bank.sound("tone440")), 0, t26::atEar(0.5f), f0 + 4000);
        a.setBus(static_cast<uint8_t>(bank.bus("foley")), -6.0f, 0.0, t26::kStep);
        a.setBus(static_cast<uint8_t>(bank.bus("world")), -9.0f, 900.0, 18 * t26::kStep);
        out.resize(std::size_t(t26::kRate) * 2);
        a.render(out, t26::kRate);
        return true;
    };
    std::vector<float> x, y;
    EXPECT_TRUE(run(x));
    EXPECT_TRUE(run(y));
    EXPECT_TRUE(x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
    TEST_PASS();
    return true;
}

// An occluded sound's low-pass feeds its bus, not the endpoint: muting the bus silences it.
bool t26_occluded_through_bus() {
    TEST_BEGIN("t26_occluded_through_bus");
    AudioSystem a;
    EXPECT_TRUE(a.initialize(true));
    a.setListenerPosition({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const std::string noise = (t26::soundRoot() / "base" / "noise.wav").string();
    EXPECT_TRUE(isValidSoundHandle(a.playSoundAt(noise, {0, 0, -2}, SoundCategory::SFX, false, 1.0f,
                                                 SoundShape{1.0f, 50.0f, 1.0f, true})));
    EXPECT_TRUE(t26::rms(t26::renderLeft(a, 4800), 960) > 1e-3);
    a.setMute(static_cast<uint8_t>(a.bank().bus("sfx")), true);
    EXPECT_TRUE(t26::rms(t26::renderLeft(a, 4800), 960) < 1e-6);
    TEST_PASS();
    return true;
}

// Files resolve local-folder-first: the first folder holding any variant supplies all of them.
bool t26_local_first() {
    TEST_BEGIN("t26_local_first");
    AudioSystem a;
    EXPECT_TRUE(t26::mixer(a));
    const int var = a.bank().sound("var");
    EXPECT_TRUE(var >= 0);
    EXPECT_EQ(a.variantCount(static_cast<uint16_t>(var)), 1u);   // local/var_1 only, not base's two
    // ... and it is local's file: dc, not a click.
    EXPECT_TRUE(isValidSoundHandle(a.play(static_cast<uint16_t>(var), 0, t26::atEar())));
    const auto x = t26::renderLeft(a, 960);
    EXPECT_TRUE(x[500] > 0.1f);
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
    testVolumeClamp();

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

    std::cout << "\n\033[1m--- Test 26: the mixer (deviceless) ---\033[0m" << std::endl;
    t26_bank_parse_errors();
    t26_slice_bank();
    t26_default_bank_categories();
    t26_bus_tree();
    t26_mute_solo();
    t26_start_time_sample_accurate();
    t26_duck_envelope();
    t26_snapshot_blend();
    t26_steal_fade();
    t26_render_repeatable();
    t26_occluded_through_bus();
    t26_local_first();
    std::error_code ec;
    std::filesystem::remove_all(t26::soundRoot(), ec);

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
    std::cout << std::flush;
    return ohao::test::finish("audio_system_tests", testsPassed, testsFailed, g_failures);
}
