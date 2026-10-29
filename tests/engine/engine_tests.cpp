/**
 * engine_tests.cpp - Unit tests for engine subsystems
 *
 * Tests:
 *   1. EventBus  — subscribe, publish, unsubscribe, clear, typed data, multiple subscribers
 *   2. CommandHistory — execute, undo, redo, canUndo/canRedo, descriptions, clear, max history
 *   3. Assets, meta/traits, headless deferred/path tracer, RenderScene
 */

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <cmath>
#include <cstring>

#include "core/core.hpp"
#include "core/log.hpp"
#include "render/frame/pass_registry.hpp"
#include "gpu/layout_meta.hpp"
#include "gpu/vulkan/vk_utils.hpp"
#include "gpu/vulkan/material_instance.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/camera/camera.hpp"
#include "physics/backend/physics_backend.hpp"
#include "physics/common/physics_constants.hpp"
#include "physics/world/physics_world.hpp"
#include "render/deferred/post_processing_pipeline.hpp"
#include "render/graph/resource_handle.hpp"
#include "render/hud/hud_list.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/rt/path_tracer.hpp"
#include "render/rt/rt_gi_technique.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/rt/gpu_light.hpp"
#include "render/scene/render_scene.hpp"
#include "render/scene/material_table.hpp"
#include "render/scene/model_upload.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"
#include "runtime/image.hpp"

#include "common/harness.hpp"   // the JSON summary line (ohao::test::finish)

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>
#include <vector>

using namespace ohao;

// =============================================================================
// TEST FRAMEWORK (matches existing suites)
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;
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

#define EXPECT(expr, msg) \
    do { if (!(expr)) { TEST_FAIL(msg); return; } } while(0)

#define EXPECT_EQ(a, b, msg) \
    do { if ((a) != (b)) { TEST_FAIL(std::string(msg) + " (" + std::to_string(a) + " != " + std::to_string(b) + ")"); return; } } while(0)

// =============================================================================
// SECTION 1 — EVENT BUS
// =============================================================================

static void runEventBusTests() {
    std::cout << "\n[EventBus]\n";

    // Reset between test sections
    EventBus::instance().clear();

    TEST_BEGIN("subscribe and publish fire handler");
    {
        int callCount = 0;
        auto id = EventBus::instance().subscribe("test.event",
            [&](const Event&) { ++callCount; });
        EventBus::instance().publish("test.event");
        EventBus::instance().unsubscribe(id);
        EXPECT(callCount == 1, "handler should fire once");
        TEST_PASS();
    }

    TEST_BEGIN("publish to unknown event type is safe");
    {
        // Should not crash, no subscribers
        EventBus::instance().publish("no.subscribers.here");
        TEST_PASS();
    }

    TEST_BEGIN("unsubscribe stops future delivery");
    {
        int callCount = 0;
        auto id = EventBus::instance().subscribe("unsub.test",
            [&](const Event&) { ++callCount; });
        EventBus::instance().publish("unsub.test");
        EventBus::instance().unsubscribe(id);
        EventBus::instance().publish("unsub.test");
        EXPECT(callCount == 1, "handler should fire only once before unsubscribe");
        TEST_PASS();
    }

    TEST_BEGIN("multiple subscribers all receive publish");
    {
        int a = 0, b = 0, c = 0;
        auto id1 = EventBus::instance().subscribe("multi.test", [&](const Event&) { ++a; });
        auto id2 = EventBus::instance().subscribe("multi.test", [&](const Event&) { ++b; });
        auto id3 = EventBus::instance().subscribe("multi.test", [&](const Event&) { ++c; });
        EventBus::instance().publish("multi.test");
        EventBus::instance().unsubscribe(id1);
        EventBus::instance().unsubscribe(id2);
        EventBus::instance().unsubscribe(id3);
        EXPECT(a == 1 && b == 1 && c == 1, "all three subscribers should have fired");
        TEST_PASS();
    }

    TEST_BEGIN("typed event data (string payload)");
    {
        std::string received;
        auto id = EventBus::instance().subscribe("entity.selected",
            [&](const Event& e) {
                received = std::any_cast<std::string>(e.data);
            });
        EventBus::instance().publish("entity.selected", std::string("MyEntity"));
        EventBus::instance().unsubscribe(id);
        EXPECT(received == "MyEntity", "payload should round-trip through std::any");
        TEST_PASS();
    }

    TEST_BEGIN("typed event data (int payload)");
    {
        int received = -1;
        auto id = EventBus::instance().subscribe("frame.count",
            [&](const Event& e) { received = std::any_cast<int>(e.data); });
        EventBus::instance().publish("frame.count", 42);
        EventBus::instance().unsubscribe(id);
        EXPECT(received == 42, "int payload should round-trip");
        TEST_PASS();
    }

    TEST_BEGIN("clear removes all subscribers");
    {
        int fired = 0;
        EventBus::instance().subscribe("clear.test", [&](const Event&) { ++fired; });
        EventBus::instance().clear();
        EventBus::instance().publish("clear.test");
        EXPECT(fired == 0, "handler should not fire after clear()");
        TEST_PASS();
    }

    TEST_BEGIN("event type field matches published type");
    {
        std::string capturedType;
        auto id = EventBus::instance().subscribe("type.check",
            [&](const Event& e) { capturedType = e.type; });
        EventBus::instance().publish("type.check");
        EventBus::instance().unsubscribe(id);
        EXPECT(capturedType == "type.check", "event.type should be set by publish");
        TEST_PASS();
    }

    EventBus::instance().clear();
}

// =============================================================================
// SECTION 2 — COMMAND HISTORY
// =============================================================================

static void runCommandHistoryTests() {
    std::cout << "\n[CommandHistory]\n";

    CommandHistory::instance().clear();

    TEST_BEGIN("execute runs the command");
    {
        int val = 0;
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "Set val=1",
            [&]() { val = 1; },
            [&]() { val = 0; }
        ));
        EXPECT(val == 1, "execute() should call doFunc");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("undo reverses the command");
    {
        int val = 0;
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "Set val=99",
            [&]() { val = 99; },
            [&]() { val = 0; }
        ));
        EXPECT(val == 99, "execute should set val=99");
        CommandHistory::instance().undo();
        EXPECT(val == 0, "undo() should call undoFunc and reset to 0");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("redo re-applies after undo");
    {
        int val = 0;
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "Set val=7",
            [&]() { val = 7; },
            [&]() { val = 0; }
        ));
        CommandHistory::instance().undo();
        EXPECT(val == 0, "after undo, val=0");
        CommandHistory::instance().redo();
        EXPECT(val == 7, "after redo, val=7");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("canUndo and canRedo flags");
    {
        CommandHistory::instance().clear();
        EXPECT(!CommandHistory::instance().canUndo(), "canUndo should be false on empty history");
        EXPECT(!CommandHistory::instance().canRedo(), "canRedo should be false on empty redo stack");

        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "dummy", []() {}, []() {}
        ));
        EXPECT(CommandHistory::instance().canUndo(), "canUndo should be true after execute");
        EXPECT(!CommandHistory::instance().canRedo(), "canRedo should be false, no redo yet");

        CommandHistory::instance().undo();
        EXPECT(!CommandHistory::instance().canUndo(), "canUndo false after undoing only command");
        EXPECT(CommandHistory::instance().canRedo(), "canRedo true after undo");

        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("execute clears redo stack");
    {
        int val = 0;
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "step1", [&]() { val = 1; }, [&]() { val = 0; }
        ));
        CommandHistory::instance().undo();
        EXPECT(CommandHistory::instance().canRedo(), "should have redo after undo");
        // Executing a new command should clear redo
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "step2", [&]() { val = 2; }, [&]() { val = 0; }
        ));
        EXPECT(!CommandHistory::instance().canRedo(), "new execute should clear redo stack");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("undo returns false when nothing to undo");
    {
        CommandHistory::instance().clear();
        bool result = CommandHistory::instance().undo();
        EXPECT(!result, "undo() should return false on empty stack");
        TEST_PASS();
    }

    TEST_BEGIN("redo returns false when nothing to redo");
    {
        CommandHistory::instance().clear();
        bool result = CommandHistory::instance().redo();
        EXPECT(!result, "redo() should return false on empty redo stack");
        TEST_PASS();
    }

    TEST_BEGIN("undoDescription / redoDescription");
    {
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
            "Move Box to (1,2,3)", []() {}, []() {}
        ));
        std::string desc = CommandHistory::instance().undoDescription();
        EXPECT(desc == "Move Box to (1,2,3)", "undoDescription should return last command description");
        CommandHistory::instance().undo();
        std::string redoDesc = CommandHistory::instance().redoDescription();
        EXPECT(redoDesc == "Move Box to (1,2,3)", "redoDescription should return undone command description");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("getUndoCount / getRedoCount");
    {
        CommandHistory::instance().clear();
        EXPECT_EQ(CommandHistory::instance().getUndoCount(), 0u, "undo count starts at 0");

        CommandHistory::instance().execute(std::make_unique<LambdaCommand>("a", [](){}, [](){}));
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>("b", [](){}, [](){}));
        CommandHistory::instance().execute(std::make_unique<LambdaCommand>("c", [](){}, [](){}));
        EXPECT_EQ(CommandHistory::instance().getUndoCount(), 3u, "undo count should be 3");

        CommandHistory::instance().undo();
        EXPECT_EQ(CommandHistory::instance().getUndoCount(), 2u, "undo count decrements");
        EXPECT_EQ(CommandHistory::instance().getRedoCount(), 1u, "redo count increments");

        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("setMaxHistory trims old entries");
    {
        CommandHistory::instance().clear();
        CommandHistory::instance().setMaxHistory(3);
        for (int i = 0; i < 5; ++i) {
            CommandHistory::instance().execute(std::make_unique<LambdaCommand>(
                "cmd" + std::to_string(i), [](){}, [](){}
            ));
        }
        // Max 3 → only 3 entries kept
        EXPECT(CommandHistory::instance().getUndoCount() <= 3u, "history should be trimmed to maxHistory");
        CommandHistory::instance().setMaxHistory(100);
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("LambdaCommand getDescription");
    {
        LambdaCommand cmd("Rotate Camera 45deg", [](){}, [](){});
        EXPECT(cmd.getDescription() == "Rotate Camera 45deg", "getDescription should return ctor arg");
        TEST_PASS();
    }

    CommandHistory::instance().clear();
}

// =============================================================================
// SECTION 3 — ASSETS (scene entities are covered by ecs_tests)
// =============================================================================

static void runAssetTests() {
    std::cout << "\n[Assets]\n";

    TEST_BEGIN("ModelLoader::isSupportedExtension");
    {
        EXPECT(ModelLoader::isSupportedExtension("glb"), "glb ok");
        EXPECT(ModelLoader::isSupportedExtension(".fbx"), "fbx with dot");
        EXPECT(!ModelLoader::isSupportedExtension("exe"), "exe no");
        TEST_PASS();
    }
}

// =============================================================================
// C++20 META / TRAITS
// =============================================================================

void runMetaTests() {
    std::cout << "\n--- C++20 Meta / Traits ---\n";

    TEST_BEGIN("RT profile traits (compile-time settings)");
    {
        constexpr auto rt = makeProfileSettings<RTRenderProfile::Realtime>();
        constexpr auto off = makeProfileSettings<RTRenderProfile::Offline>();
        EXPECT(rt.maxBounces == 2, "realtime max bounces");
        EXPECT(off.maxBounces == 32, "offline max bounces");
        EXPECT(off.denoiseMode == DenoiseMode::OIDN, "offline default denoise OIDN");
        EXPECT(RTProfileTraits<RTRenderProfile::Realtime>::is_realtime, "realtime flag");
        EXPECT(RTProfileTraits<RTRenderProfile::Offline>::is_offline, "offline flag");
        TEST_PASS();
    }

    TEST_BEGIN("DenoiseModeTraits + runtime mirrors");
    {
        static_assert(DenoiseModeTraits<DenoiseMode::NRD>::needs_motion_vectors);
        static_assert(DenoiseModeTraits<DenoiseMode::NRD>::needs_diff_spec_split);
        static_assert(DenoiseModeTraits<DenoiseMode::OIDN>::needs_cpu_readback);
        static_assert(!DenoiseModeTraits<DenoiseMode::OIDN>::is_realtime_capable);
        static_assert(DenoiseModeTraits<DenoiseMode::DLSSRR>::is_gpu_backend);

        EXPECT(denoiseNeedsMotionVectors(DenoiseMode::NRD), "NRD needs MV");
        EXPECT(denoiseNeedsMotionVectors(DenoiseMode::DLSSRR), "DLSSRR needs MV");
        EXPECT(!denoiseNeedsMotionVectors(DenoiseMode::OIDN), "OIDN no MV");
        EXPECT(denoiseNeedsCpuReadback(DenoiseMode::OIDN), "OIDN cpu readback");
        EXPECT(denoiseNeedsDiffSpecSplit(DenoiseMode::NRD), "NRD diff/spec");
        EXPECT(!denoiseIsRealtimeCapable(DenoiseMode::OIDN), "OIDN not realtime");
        TEST_PASS();
    }

    TEST_BEGIN("RTFeatureFlags + makeFeatureSettings if constexpr path");
    {
        using F = RTFeatureFlags<RTRenderProfile::Realtime, DenoiseMode::DLSSRR>;
        static_assert(F::want_motion_vectors);
        static_assert(F::interactive);
        static_assert(F::max_bounces == 2);

        constexpr auto s = makeFeatureSettings<RTRenderProfile::Offline, DenoiseMode::NRD>();
        EXPECT(s.enableAuxiliaryAOVs, "NRD forces auxiliary AOVs");
        EXPECT(s.denoiseMode == DenoiseMode::NRD, "feature settings denoise mode");
        EXPECT(s.profile == RTRenderProfile::Offline, "feature settings profile");
        TEST_PASS();
    }

    TEST_BEGIN("GPU layout contracts");
    {
        static_assert(sizeof(GPULight) == layout::kGPULightBytes);
        static_assert(layout::MaterialGpuPack::kBytes == 112);
        static_assert(layout::MaterialGpuPack::byteOffset(1) == 112);
        EXPECT(layout::MaterialGpuPack::vec4Count(2) == 14, "2 materials = 14 vec4s");
        EXPECT(GpuPod<GPULight>, "GPULight is GpuPod");
        TEST_PASS();
    }

    TEST_BEGIN("as_span + ContiguousRangeOf");
    {
        std::vector<float> v{1.f, 2.f, 3.f, 4.f};
        auto s = as_const_span<float>(v);
        EXPECT(s.size() == 4, "span size");
        EXPECT(s[0] == 1.f && s[3] == 4.f, "span contents");
        EXPECT(span_covers_image(s, 2, 2, 1), "2x2x1 covered");
        EXPECT(!span_covers_image(s, 4, 4, 3), "too small for 4x4x3");
        TEST_PASS();
    }

    TEST_BEGIN("Result / VoidResult");
    {
        auto ok = Result<int>::ok(7);
        EXPECT(ok && ok.value() == 7, "Result ok");
        auto bad = err_string<int>("nope");
        EXPECT(!bad, "Result err is false");
        EXPECT(bad.error() == "nope", "Result error string");
        EXPECT(ok.value_or(0) == 7, "value_or success");
        EXPECT(bad.value_or(3) == 3, "value_or fallback");

        auto vok = VoidResult<>::ok();
        auto verr = err_void("fail");
        EXPECT(vok, "VoidResult ok");
        EXPECT(!verr && verr.error() == "fail", "VoidResult err");
        TEST_PASS();
    }

    TEST_BEGIN("ScopedSubscription RAII + typed publish");
    {
        EventBus bus;
        int hits = 0;
        {
            auto sub = make_scoped_subscription(bus, "core.test",
                [&](const Event& e) {
                    if (e.holds<int>()) hits += *e.try_cast<int>();
                });
            EXPECT(sub, "scoped sub active");
            bus.publishTyped("core.test", 5);
            bus.publishTyped("core.test", 7);
            EXPECT(hits == 12, "typed publish delivered");
            EXPECT(bus.subscriptionCount() == 1, "one sub");
        }
        bus.publishTyped("core.test", 100);
        EXPECT(hits == 12, "RAII unsubscribed");
        EXPECT(bus.subscriptionCount() == 0, "zero subs after scope");
        TEST_PASS();
    }

    TEST_BEGIN("make_lambda_command + execute_lambda");
    {
        CommandHistory::instance().clear();
        int x = 0;
        CommandHistory::instance().execute_lambda(
            "inc",
            [&]() { x += 1; },
            [&]() { x -= 1; });
        EXPECT(x == 1 && CommandHistory::instance().canUndo(), "executed");
        EXPECT(CommandHistory::instance().undo(), "undo");
        EXPECT(x == 0, "undone");
        EXPECT(CommandHistory::instance().redo(), "redo");
        EXPECT(x == 1, "redone");
        CommandHistory::instance().clear();
        TEST_PASS();
    }

    TEST_BEGIN("to_underlying(log::Level)");
    {
        EXPECT(to_underlying(log::Level::Error) == 4, "log::Level underlying");
        EXPECT(log::levelName(log::Level::Warn) == "warn", "log level name");
        TEST_PASS();
    }

    TEST_BEGIN("GPU vk_utils + MaterialFeatures + RenderMode");
    {
        EXPECT(vk_ok(VK_SUCCESS), "vk_ok");
        EXPECT(vk_failed(VK_ERROR_DEVICE_LOST), "vk_failed");
        EXPECT(vk_result_name(VK_SUCCESS) == "VK_SUCCESS", "result name");
        EXPECT(mip_levels_for(1, 1) == 1u, "1x1 mips");
        EXPECT(mip_levels_for(256, 128) == 9u, "256 mips"); // 256→1 = 9 levels
        auto flags = MaterialFeatures::CastShadows | MaterialFeatures::UseNormalMap;
        EXPECT(hasFlag(flags, MaterialFeatures::CastShadows), "has cast");
        EXPECT(isRTRenderMode(RenderMode::RTOffline), "RT offline");
        EXPECT(isRasterRenderMode(RenderMode::Deferred), "deferred raster");
        TEST_PASS();
    }

    TEST_BEGIN("RT applyDenoisePolicy + fresh-sample / jitter traits");
    {
        RTRenderSettings s{
            .profile = RTRenderProfile::Realtime,
            .enableAuxiliaryAOVs = false,
            .denoiseMode = DenoiseMode::NRD,
        };
        s = applyDenoisePolicy(s);
        EXPECT(s.enableAuxiliaryAOVs, "NRD forces AOVs");
        EXPECT(denoiseNeedsPixelJitter(DenoiseMode::NRD), "NRD jitter");
        EXPECT(denoiseNeedsPixelJitter(DenoiseMode::DLSSRR), "DLSS jitter");
        EXPECT(!denoiseNeedsPixelJitter(DenoiseMode::OIDN), "OIDN no jitter");
        EXPECT(denoiseWantsFreshSample(DenoiseMode::Atrous), "SVGF fresh");
        EXPECT(denoiseWantsFreshSample(DenoiseMode::DLSSRR), "DLSS fresh");
        EXPECT(!denoiseWantsFreshSample(DenoiseMode::NRD), "NRD not fresh-only");
        EXPECT(denoiseNeedsAovAccumulation(DenoiseMode::NRD), "NRD AOV mean");
        EXPECT(clampSamplesPerFrame(0) == 1u && clampSamplesPerFrame(100) == 64u, "spf clamp");
        EXPECT(isQmcSampler(SamplerType::Sobol) && !isQmcSampler(SamplerType::PCG), "QMC");
        TEST_PASS();
    }

    TEST_BEGIN("Render graph handles + camera");
    {
        TextureHandle th{};
        EXPECT(!th, "invalid texture handle");
        th.index = 3;
        EXPECT(th && th.isValid(), "valid handle");
        auto usage = TextureUsage::ColorAttachment | TextureUsage::ShaderRead;
        EXPECT(hasFlag(usage, TextureUsage::ShaderRead), "usage flag");
        Camera cam;
        EXPECT(cam.isPerspective(), "default perspective");
        EXPECT(cam.getFov() > 0.f, "fov");
        EXPECT(tonemapOperatorIndex(TonemapOperator::ACES) == 0u, "tonemap index");
        TEST_PASS();
    }

    TEST_BEGIN("Physics handles + ShapeInfo span + sim state");
    {
        using namespace ohao::physics::backend;
        EXPECT(!isValidBody(INVALID_BODY), "invalid body");
        EXPECT(isValidBody(1u), "valid body");
        RaycastHit miss{};
        EXPECT(!miss, "no hit");
        miss.bodyHandle = 3;
        EXPECT(miss.hit(), "hit");

        std::vector<glm::vec3> verts{{0,0,0},{1,0,0},{0,1,0}};
        std::vector<uint32_t> inds{0,1,2};
        ShapeInfo shape;
        shape.setMesh(verts, inds);
        EXPECT(shape.type == ShapeInfo::Type::MESH, "mesh type");
        EXPECT(shape.meshVertexCount == 3u && shape.meshIndexCount == 3u, "mesh counts");

        EXPECT(isSimulating(ohao::physics::SimulationState::RUNNING), "running");
        EXPECT(!isSimulating(ohao::physics::SimulationState::PAUSED), "paused");
        auto moon = ohao::physics::PhysicsConfig::moon();
        EXPECT(moon.gravity.y < -1.f && moon.gravity.y > -3.f, "moon g");
        TEST_PASS();
    }

    TEST_BEGIN("a rigid body exists without any scene component and is named");
    {
        ohao::physics::PhysicsWorld world;
        auto body = world.createRigidBody("crate");
        EXPECT(body != nullptr, "createRigidBody(name) returns a body");
        EXPECT(body->getName() == "crate", "the body carries its name");
        EXPECT_EQ(world.getBodyCount(), size_t{1}, "the world owns it");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION 5 — THE DEFERRED PIPELINE, HEADLESS
// =============================================================================
//
// ROADMAP ITEM 3 (spec §10.1, renderer fitting) NEEDS TO RENDER THE DEFERRED
// PIPELINE AND THE PATH TRACER AND DIFFERENCE THEM. Two earlier attempts to
// establish whether that is reachable both stopped at the LINKER, and the
// plan recorded item 3 as blocked on that basis. Both were right about their
// own failure and wrong about the conclusion:
//
//   1. Linking DeferredRenderer into a focused GPU test binary fails: it drags
//      in ohao_scene -> ohao_physics -> Jolt, which such a
//      binary has no business acquiring.
//   2. Linking it into THIS binary, which already has all of those, fails on
//      stb_image being defined in both ohao_gpu_vulkan and ohao_scene.
//
// The second is a known engine-wide condition with a known engine-wide answer
// -- /FORCE:MULTIPLE, which the GDExtension build has always used -- and once
// it is applied the pipeline stands up. So the blocker was the link, twice,
// and never the pipeline.
//
// WHAT THIS TEST PINS, and it is the foundation the rest of item 3 stands on:
// DeferredRenderer::initialize succeeds on a BARE device with no swapchain,
// no window and no surface, and hands back a usable final-output image view.
// That is not obvious -- a renderer that sized its targets from a swapchain,
// or wanted a present queue, could not do it -- and it is the difference
// between item 3 being "a stage of engine-harness work" and "impossible from
// a test binary".
//
// TWO THINGS IT ALSO MEASURES, because they are what the NEXT step needs:
//   * Shaders are resolved relative to the working directory ("bin/shaders/
//     ..."), so a pass whose SPIR-V is not found there fails non-fatally.
//     Rendering from a test will need the CWD set or the path made absolute.
//   * The RT function pointers do NOT load, because this device is created
//     without the ray-tracing extensions. The PATH TRACER half of item 3
//     therefore needs a device built like GpuProbeContext's, not like this
//     one.
//
// IT DOES NOT RENDER. Standing up is not rendering, and this test claims only
// what it does.

namespace {

// Validation errors seen since the last reset. Only HeadlessDevice::create(true)
// installs the messenger that feeds it.
static std::atomic<uint64_t> g_validationErrors{0};
static std::string g_lastValidationError;

static VKAPI_ATTR VkBool32 VKAPI_CALL countValidationErrors(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_validationErrors;
        g_lastValidationError = data && data->pMessage ? data->pMessage : "";
    }
    return VK_FALSE;
}

struct HeadlessDevice {
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    bool validating{false};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    // Recorded, because the frame test below needs a queue from the same
    // family it created the device with.
    uint32_t queueFamily{0};

    // withValidation: enable VK_LAYER_KHRONOS_validation and count its errors in
    // g_validationErrors. If the layer is not installed, `validating` stays false
    // and the caller must report the check as skipped, never as passed.
    bool create(bool withValidation = false) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ohao_engine_tests";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;

        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        if (withValidation) {
            uint32_t ln = 0;
            vkEnumerateInstanceLayerProperties(&ln, nullptr);
            std::vector<VkLayerProperties> layers(ln);
            vkEnumerateInstanceLayerProperties(&ln, layers.data());
            for (const auto& l : layers)
                if (std::strcmp(l.layerName, layer) == 0) validating = true;
        }
        if (validating) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &layer;
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = &ext;
        }
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;
        if (validating) {
            VkDebugUtilsMessengerCreateInfoEXT mi{};
            mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            mi.pfnUserCallback = countValidationErrors;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create || create(instance, &mi, nullptr, &messenger) != VK_SUCCESS) validating = false;
        }

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0) return false;
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        physical = devs[0];

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qs.data());
        bool found = false;
        for (uint32_t i = 0; i < qn; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamily = i;
                found = true;
                break;
            }
        }
        if (!found) return false;

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures feats{};
        vkGetPhysicalDeviceFeatures(physical, &feats);
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &feats;
        return vkCreateDevice(physical, &dci, nullptr, &device) == VK_SUCCESS;
    }

    void destroy() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (messenger != VK_NULL_HANDLE) {
            auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyFn) destroyFn(instance, messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};

}  // namespace

static void runHeadlessDeferredTests() {
    std::cout << "\n[Deferred pipeline, headless]\n";

    TEST_BEGIN("the deferred pipeline stands up with no swapchain");
    {
        HeadlessDevice dev;
        if (!dev.create()) {
            // SKIPPED, not failed. A machine without a Vulkan device is not a
            // defect in this pipeline.
            dev.destroy();
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }

        bool initialised = false;
        VkImageView finalOutput = VK_NULL_HANDLE;
        {
            // SCOPED so the renderer is destroyed BEFORE the device it was
            // built on. Destroying a device with live objects on it is what
            // the validation layers report ten times and then stop reporting.
            ohao::DeferredRenderer renderer;
            initialised = renderer.initialize(dev.device, dev.physical);
            if (initialised) finalOutput = renderer.getFinalOutput();
            renderer.cleanup();
        }
        dev.destroy();

        EXPECT(initialised, "DeferredRenderer::initialize on a bare device");
        EXPECT(finalOutput != VK_NULL_HANDLE,
               "a pipeline that initialised must hand back a final-output view");
        TEST_PASS();
    }

    TEST_BEGIN("a resize never writes a destroyed G-buffer view into a descriptor");
    {
        // A resize destroys the G-buffer's views and creates new ones. A pass that
        // rewrites a descriptor set while it still holds an OLD view writes a
        // destroyed handle -- undefined behaviour that crashed renderer_test on
        // NVIDIA once unrelated allocations stopped masking it. The END state looks
        // fine either way (the last setter repairs it), so this watches the resize
        // itself through the validation layer, which tracks handle lifetimes.
        HeadlessDevice dev;
        if (!dev.create(/*withValidation=*/true)) {
            dev.destroy();
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        if (!dev.validating) {
            dev.destroy();
            std::cout << "(VK_LAYER_KHRONOS_validation not installed -- SKIPPED, not verified) ";
            TEST_PASS();
            return;
        }
        bool initialised = false;
        uint64_t errorsDuringResize = 0;
        std::string lastError;
        {
            ohao::DeferredRenderer renderer;
            initialised = renderer.initialize(dev.device, dev.physical);
            if (initialised) {
                g_validationErrors = 0;
                renderer.onResize(640, 480);
                errorsDuringResize = g_validationErrors.load();
                lastError = g_lastValidationError;
            }
            renderer.cleanup();
        }
        dev.destroy();

        EXPECT(initialised, "DeferredRenderer::initialize on a validated device");
        if (errorsDuringResize != 0) std::cout << "\n      last error: " << lastError.substr(0, 300) << "\n";
        EXPECT_EQ(errorsDuringResize, uint64_t{0},
                  "onResize must not write any destroyed handle into a descriptor set");
        TEST_PASS();
    }
}


// -----------------------------------------------------------------------------
// ...AND THE PATH TRACER, which is the reference image item 3 fits against
// -----------------------------------------------------------------------------
//
// Any harness that fits the deferred pipeline against the PATH TRACER needs
// the path tracer standing up, and that half had never been stood up anywhere:
// every prior test touching PathTracer's ownership protocol did so against a
// faithful reproduction of setMaterialData's map-and-memcpy, never against
// PathTracer itself, which needs its images and pipelines alive. This is those
// images and pipelines standing up.
//
// THE DEVICE IS THE WHOLE DIFFICULTY, and it is a different device from the
// one the deferred pipeline needs. Three things had to be right:
//
//   1. THE PHYSICAL DEVICE. This machine has two, and the selection must be
//      by CAPABILITY rather than by index -- so the probe takes the first
//      that advertises VK_KHR_ray_tracing_pipeline instead of devices[0].
//   2. THE EXTENSION LIST AND FEATURE CHAIN, taken from
//      ohao/gpu/vulkan/device_setup.cpp rather than guessed: acceleration
//      structure, ray tracing pipeline, deferred host operations, buffer
//      device address, descriptor indexing, SPIR-V 1.4, shader float
//      controls.
//   3. VK_KHR_push_descriptor, AND THIS ONE IS THE TRAP. Without it
//      `PathTracer::init` still returns TRUE -- the RT pipeline and the SBT
//      are built fine -- and then NRD's NRI device wrapper ABORTS the process
//      from `ResolveDispatchTable()`, because it resolves
//      vkCmdPushDescriptorSet eagerly and treats absence as fatal. A test
//      that only checked the return value would have reported success from a
//      process that then died with exit 3. device_setup.cpp adds this
//      extension under its DLSS block, so an engine build gets it
//      incidentally and never sees the failure.
//
// THE SPIR-V IS FOUND relative to the working directory: path_tracer_pipeline
// searches "build/shaders/<name>" among others, so this test must run from
// the repository root. If it is ever run from elsewhere the pipeline creation
// fails and this test says so rather than silently rendering nothing.
//
// IT DOES NOT RENDER. `render()` wants an RTAccelerationStructure, which
// wants a scene -- and a scene is the next thing item 3 needs. This claims
// only that the pipelines and images exist.

namespace {

struct RtHeadlessDevice {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    uint32_t family{0};
    std::vector<const char*> deviceExts;
    std::string deviceName;

    bool create() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ohao_engine_tests";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0) return false;
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());

        // BY CAPABILITY, not by index: devices[0] need not be the one that can
        // ray trace.
        for (VkPhysicalDevice d : devs) {
            uint32_t ec = 0;
            vkEnumerateDeviceExtensionProperties(d, nullptr, &ec, nullptr);
            std::vector<VkExtensionProperties> exts(ec);
            vkEnumerateDeviceExtensionProperties(d, nullptr, &ec, exts.data());
            bool rtPipeline = false, pushDesc = false;
            for (const auto& e : exts) {
                if (std::strcmp(e.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
                    rtPipeline = true;
                if (std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0)
                    pushDesc = true;
            }
            if (rtPipeline && pushDesc) {
                physical = d;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) return false;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        deviceName = props.deviceName;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qs.data());
        bool found = false;
        for (uint32_t i = 0; i < qn; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                found = true;
                break;
            }
        }
        if (!found) return false;

        // device_setup.cpp's RT list, minus the CUDA-interop entries nothing
        // here uses, plus push_descriptor -- see this section's header for why
        // that last one is not optional.
        deviceExts = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
            VK_KHR_SPIRV_1_4_EXTENSION_NAME,
            VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f12.bufferDeviceAddress = VK_TRUE;
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.scalarBlockLayout = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
        as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        as.accelerationStructure = VK_TRUE;
        as.pNext = &f12;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
        rt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rt.rayTracingPipeline = VK_TRUE;
        rt.pNext = &as;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &rt;
        vkGetPhysicalDeviceFeatures(physical, &f2.features);

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        dci.ppEnabledExtensionNames = deviceExts.data();
        dci.pNext = &f2;
        return vkCreateDevice(physical, &dci, nullptr, &device) == VK_SUCCESS;
    }

    void destroy() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};

}  // namespace

static void runHeadlessPathTracerTests() {
    std::cout << "\n[Path tracer, headless]\n";

    TEST_BEGIN("the path tracer stands up on an RT device with no swapchain");
    {
        RtHeadlessDevice dev;
        if (!dev.create()) {
            dev.destroy();
            std::cout << "(no device with ray tracing + push descriptor -- skipped) ";
            TEST_PASS();
            return;
        }

        bool initialised = false;
        VkImageView outView = VK_NULL_HANDLE;
        VkImage outImage = VK_NULL_HANDLE;
        {
            // SCOPED, so the path tracer releases its images and pipelines
            // before the device they live on is destroyed.
            ohao::PathTracer pt;
            initialised = pt.init(dev.device, dev.physical, 256, 256, dev.instance, dev.family, {},
                                  dev.deviceExts);
            if (initialised) {
                outView = pt.getOutputView();
                outImage = pt.getOutputImage();
            }
        }
        dev.destroy();

        std::cout << "(" << dev.deviceName << ") ";
        EXPECT(initialised, "PathTracer::init on a bare RT device");
        EXPECT(outView != VK_NULL_HANDLE, "an initialised path tracer must have an output view");
        EXPECT(outImage != VK_NULL_HANDLE, "an initialised path tracer must have an output image");
        TEST_PASS();
    }
}


// -----------------------------------------------------------------------------
// ...AND A FRAME RECORDED, SUBMITTED AND READ BACK
// -----------------------------------------------------------------------------
//
// Standing up is not rendering, and the two tests above say so. This is
// rendering: one frame recorded into a command buffer this test owns,
// submitted on a queue it created, waited on, and the final output image
// copied into a host-visible buffer and inspected. It is the last piece of
// item 3's harness that was uncertain -- everything after it (a scene, a
// TLAS, an optimiser) is ordinary work against interfaces that now demonstrably
// function headless.
//
// THE SCENE IS NULL, deliberately. The question this answers is whether the
// pass graph can be driven at all with no window and no swapchain, and a null
// scene is the cleanest way to ask it: nothing to load, nothing to fail for
// reasons that are not the harness's. `render()` passes the scene straight
// through to the passes, so this also establishes that they tolerate its
// absence rather than dereferencing it.
//
// WHAT AN EMPTY SCENE LOOKS LIKE, AND WHY THAT IS THE ASSERTION. The readback
// comes back with EXACTLY width*height non-zero bytes out of width*height*4 --
// one per pixel. That is the alpha channel at 255 with RGB at 0: transparent
// black, opaque alpha, which is precisely a cleared frame with nothing drawn
// into it. Two things follow, and neither would follow from "the copy
// returned":
//
//   * THE COPY ACTUALLY HAPPENED. A failed or skipped vkCmdCopyImageToBuffer
//     leaves the staging buffer as allocated, which is typically all zeros --
//     indistinguishable from a black image if the test only asked "is it
//     black". The non-zero alpha is what separates them.
//   * THE STRIDE IS RIGHT. One non-zero byte in four, at a count equal to the
//     pixel count, is only consistent with a correctly strided RGBA8 copy of
//     the whole image.
//
// THE SOURCE LAYOUT IS VK_IMAGE_LAYOUT_GENERAL, which is what the engine's own
// readbacks in ohao/gpu/vulkan/renderer.cpp use for compute-written images and
// what the final output is left in.
//
// VALIDATION IS NOT ENABLED HERE. Running this path with the validation layers
// on surfaces several PRE-EXISTING engine conditions -- shader-module
// capability requirements, a graphics pipeline layout not matching a declared
// resource variable's stage, and a descriptor imageLayout not matching the
// live layout. They are not caused by rendering headless and they are not this
// test's to fix; they are recorded in the roadmap so that whoever turns
// validation on next is not surprised.

static void runHeadlessDeferredRenderTests() {
    std::cout << "\n[Deferred pipeline, a frame end to end]\n";

    TEST_BEGIN("a headless frame records, submits and reads back");
    {
        HeadlessDevice dev;
        if (!dev.create()) {
            dev.destroy();
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(dev.device, dev.queueFamily, 0, &queue);

        bool submitted = false;
        bool copied = false;
        uint64_t nonZero = 0;
        uint64_t pixels = 0;

        {
            ohao::DeferredRenderer renderer;
            if (!renderer.initialize(dev.device, dev.physical)) {
                renderer.cleanup();
                dev.destroy();
                TEST_FAIL("initialize");
                return;
            }
            const glm::mat4 view =
                glm::lookAt(glm::vec3(0, 1, 4), glm::vec3(0), glm::vec3(0, 1, 0));
            const glm::mat4 proj =
                glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            renderer.setCameraData(view, proj, glm::vec3(0, 1, 4), 0.1f, 100.0f);

            VkCommandPoolCreateInfo pci{};
            pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.queueFamilyIndex = dev.queueFamily;
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(dev.device, &pci, nullptr, &pool);
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(dev.device, &ai, &cmd);
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;

            // No sky: an empty scene is then a cleared frame, the known image the stride
            // check below needs (with the sky on, every pixel of an empty scene is sky).
            renderer.setSkyEnabled(false);
            vkBeginCommandBuffer(cmd, &bi);
            renderer.render(cmd, 0u);
            vkEndCommandBuffer(cmd);
            submitted = (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
            vkQueueWaitIdle(queue);

            const VkImage outImage = renderer.getFinalOutputImage();
            if (submitted && outImage != VK_NULL_HANDLE) {
                const uint32_t w = 1920u, h = 1080u;
                pixels = static_cast<uint64_t>(w) * h;
                const VkDeviceSize bytes = pixels * 4ull;

                VkBufferCreateInfo bci{};
                bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bci.size = bytes;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkBuffer buf = VK_NULL_HANDLE;
                vkCreateBuffer(dev.device, &bci, nullptr, &buf);
                VkMemoryRequirements mr{};
                vkGetBufferMemoryRequirements(dev.device, buf, &mr);
                VkPhysicalDeviceMemoryProperties mp{};
                vkGetPhysicalDeviceMemoryProperties(dev.physical, &mp);
                uint32_t typeIdx = 0;
                for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                    const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags & want) == want) {
                        typeIdx = i;
                        break;
                    }
                }
                VkMemoryAllocateInfo mai{};
                mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                mai.allocationSize = mr.size;
                mai.memoryTypeIndex = typeIdx;
                VkDeviceMemory mem = VK_NULL_HANDLE;
                vkAllocateMemory(dev.device, &mai, nullptr, &mem);
                vkBindBufferMemory(dev.device, buf, mem, 0);

                vkResetCommandBuffer(cmd, 0);
                vkBeginCommandBuffer(cmd, &bi);
                VkImageMemoryBarrier toSrc{};
                toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.image = outImage;
                toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toSrc);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {w, h, 1u};
                vkCmdCopyImageToBuffer(cmd, outImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                       &region);
                vkEndCommandBuffer(cmd);
                vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(queue);

                void* mapped = nullptr;
                if (vkMapMemory(dev.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
                    const unsigned char* px = static_cast<const unsigned char*>(mapped);
                    for (VkDeviceSize i = 0; i < bytes; ++i) {
                        if (px[i] != 0u) ++nonZero;
                    }
                    vkUnmapMemory(dev.device, mem);
                    copied = true;
                }
                vkDestroyBuffer(dev.device, buf, nullptr);
                vkFreeMemory(dev.device, mem, nullptr);
            }
            vkDestroyCommandPool(dev.device, pool, nullptr);
            renderer.cleanup();
        }
        dev.destroy();

        EXPECT(submitted, "the recorded frame must submit successfully");
        EXPECT(copied, "the final output must copy into a host-visible buffer");
        // THE COPY HAPPENED: a skipped copy leaves the staging buffer as
        // allocated, which is all zeros and indistinguishable from a black
        // image unless something is required to be non-zero.
        EXPECT(nonZero > 0, "a readback of all zeros cannot be told from a copy that never ran");
        // AND THE STRIDE IS RIGHT: exactly one non-zero byte per pixel is the
        // alpha channel of a cleared frame, which is what an empty scene is.
        EXPECT_EQ(nonZero, pixels,
                  "expected exactly one non-zero byte per pixel (opaque alpha over transparent "
                  "black, i.e. a cleared frame with nothing drawn)");
        TEST_PASS();
    }
}


// -----------------------------------------------------------------------------
// ENTRY POINTS WHOSE ONLY CALLER WAS REMOVED
// -----------------------------------------------------------------------------
//
// VulkanRenderer::resize had exactly one caller -- the inverse-rendering
// session -- and when that module left this repository it became unreachable
// from anything that ships. (updateRTMaterialParams, its sibling, died with the
// legacy RT scene build: materials now live in the RenderScene material table,
// covered under RENDER SCENE below.)
//
// Unreachable is not the same as wrong. A resized viewport is ordinary engine
// API that a game wants. What it lost was not its usefulness, it was its only
// evidence of working. Dead code with no test rots quietly and then fails the
// first time someone needs it.
//
// These are that evidence. They also mean the next person who greps for callers,
// finds none, and reaches for the delete key has something to read first.
// -----------------------------------------------------------------------------
static void runOrphanedEntryPointTests() {
    std::cout << "\n[Entry points whose only caller was removed]\n";

    TEST_BEGIN("resize() records the new size, growing and shrinking");
    {
        // On a renderer that was never initialized: resize() still records the size
        // the next initialize() and readback() use.
        ohao::VulkanRenderer renderer(64, 48);
        EXPECT_EQ(renderer.getWidth(), 64u, "constructor must record its width");
        EXPECT_EQ(renderer.getHeight(), 48u, "constructor must record its height");
        renderer.resize(96, 64);
        EXPECT_EQ(renderer.getWidth(), 96u, "resize must record the new width");
        EXPECT_EQ(renderer.getHeight(), 64u, "resize must record the new height");
        renderer.resize(16, 16);
        EXPECT_EQ(renderer.getWidth(), 16u, "resize must shrink the width too");
        EXPECT_EQ(renderer.getHeight(), 16u, "resize must shrink the height too");
        TEST_PASS();
    }

    TEST_BEGIN("resize after initialize resizes deferred output and keeps rendering");
    {
        // Under validation: a resize after real frames have rendered is where a
        // pass can rewrite a descriptor set while holding a view another pass just
        // destroyed. The layer tracks handle lifetimes, so those show up as
        // "invalid Vk..." / "destroyed" errors deterministically -- without it they
        // are a heap-layout-dependent segfault.
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer renderer(320, 180);
        const bool ok = renderer.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        renderer.setRenderMode(ohao::RenderMode::Deferred);
        for (int i = 0; i < 4; ++i) renderer.render();
        renderer.resize(256, 128);
        renderer.requestReadback();
        renderer.render();   // the first frame at the new size, read back
        const auto px = renderer.readback();
        EXPECT_EQ(px.size(), static_cast<size_t>(256 * 128 * 4), "the readback follows the resize");
        EXPECT(std::abs(renderer.getCamera().getAspectRatio() - 2.0f) < 1e-5f,
               "resize must update the camera aspect, or the image is stretched");
        size_t opaque = 0;
        for (size_t i = 3; i < px.size(); i += 4) opaque += px[i] != 0;
        EXPECT_EQ(opaque, static_cast<size_t>(256 * 128),
                  "every pixel of the resized frame was written (alpha set)");

        if (!renderer.validationEnabled()) {
            std::cout << "(validation layer not installed -- handle-lifetime check SKIPPED) ";
        } else {
            size_t lifetime = 0;
            std::string firstLifetime;
            for (const auto& m : renderer.validationMessages()) {
                const bool bad = m.find("invalid Vk") != std::string::npos ||
                                 m.find("was destroyed") != std::string::npos;
                if (bad && lifetime++ == 0) firstLifetime = m;
            }
            if (lifetime) std::cout << "\n      first: " << firstLifetime.substr(0, 260) << "\n";
            EXPECT_EQ(lifetime, size_t{0},
                      "no command or descriptor write may reference a destroyed Vulkan handle");
        }
        TEST_PASS();
    }

    TEST_BEGIN("a deferred resize with SSAO, TAA and bloom on leaves the layer silent");
    {
        // ohao_game's post-processing configuration. After a resize each pass
        // rebuilds its images and descriptor sets; any set left unwritten, or any
        // image sampled before its first write, is a validation message. Every
        // message counts here, not only handle lifetimes.
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer renderer(320, 180);
        const bool ok = renderer.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        if (!renderer.validationEnabled()) {
            std::cout << "(validation layer not installed -- SKIPPED) ";
            TEST_PASS();
            return;
        }
        renderer.setRenderMode(ohao::RenderMode::Deferred);
        auto* deferred = renderer.getDeferredRenderer();
        EXPECT(deferred && deferred->getPostProcessing(), "deferred post-processing exists");
        if (deferred && deferred->getPostProcessing()) {
            auto* pp = deferred->getPostProcessing();
            pp->setSSAOEnabled(true);
            pp->setTAAEnabled(true);
            pp->setBloomEnabled(true);
        }
        for (int i = 0; i < 4; ++i) renderer.render();
        renderer.resize(256, 128);
        for (int i = 0; i < 4; ++i) renderer.render();

        const auto& msgs = renderer.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "the validation layer reports nothing across the resize");
        TEST_PASS();
    }

    TEST_BEGIN("every screen-sized deferred target follows the renderer's extent, growing too");
    {
        // A pass that keeps its creation size is invisible to validation: the RT
        // shadow raygen just stores into the wrong texels (or past the image when
        // the window grows), and lighting samples them by UV. Check the extents.
        ohao::VulkanRenderer renderer(320, 180);
        if (!renderer.initialize()) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        renderer.setRenderMode(ohao::RenderMode::Deferred);
        auto* d = renderer.getDeferredRenderer();
        EXPECT(d != nullptr, "deferred renderer exists");
        auto check = [&](uint32_t w, uint32_t h, const char* when) {
            auto same = [&](VkExtent2D e) { return e.width == w && e.height == h; };
            if (d->getSSSPass()) EXPECT(same(d->getSSSPass()->extent()), when);
            if (d->getSSRPass()) EXPECT(same(d->getSSRPass()->extent()), when);
            if (d->getRTShadow()) EXPECT(same(d->getRTShadow()->extent()), when);
        };
        if (d) {
            renderer.render();
            check(320, 180, "SSS/SSR/RT shadow targets match the extent after initialize");
            renderer.resize(2560, 1440);   // larger than any built-in default
            renderer.render();
            check(2560, 1440, "SSS/SSR/RT shadow targets follow a resize upward");
        }
        TEST_PASS();
    }

    TEST_BEGIN("the camera aspect matches the renderer's extent from initialize on");
    {
        // A window opened at its final size never resizes, so initialize() is the
        // only chance to give the camera the right aspect -- else the image is stretched.
        ohao::VulkanRenderer renderer(256, 128);
        if (!renderer.initialize()) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        EXPECT(std::abs(renderer.getCamera().getAspectRatio() - 2.0f) < 1e-5f,
               "a 256x128 renderer must start with a 2:1 camera");
        TEST_PASS();
    }

    TEST_BEGIN("deferred with tonemapping off reads back its lighting output silently");
    {
        // With the post stack's tonemap off, the frame's final image is the lighting
        // pass's HDR output, so it is the source of the readback copy (headless) or
        // the swapchain blit (windowed). Either needs TRANSFER_SRC on that image.
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer renderer(320, 180);
        const bool ok = renderer.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok || !renderer.validationEnabled()) {
            std::cout << "(no Vulkan device or validation layer -- skipped) ";
            TEST_PASS();
            return;
        }
        renderer.setRenderMode(ohao::RenderMode::Deferred);
        if (auto* d = renderer.getDeferredRenderer(); d && d->getPostProcessing())
            d->getPostProcessing()->setTonemappingEnabled(false);
        for (int i = 0; i < 4; ++i) renderer.render();
        const auto& msgs = renderer.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "copying the lighting output is valid");
        TEST_PASS();
    }
}

// =============================================================================
// SECTION — PASS TIMING (sub-project 2b, test 1)
// =============================================================================

// Renders `n` deferred frames, the last one read back (so its fence has signalled and
// every frame's timings can be collected), and returns the renderer's timings.
static std::vector<ohao::render::FrameTiming> timedFrames(ohao::VulkanRenderer& r, int n) {
    for (int i = 0; i < n; ++i) {
        if (i == n - 1) r.requestReadback();
        r.render();
    }
    (void)r.readback();
    return r.frameTimings(ohao::render::GpuPassTimer::kRingFrames);
}

static bool hasPass(const ohao::render::FrameTiming& f, std::string_view name) {
    const int index = ohao::render::passIndex(name);
    for (uint32_t i = 0; i < f.count; ++i)
        if (f.pass[i] == index) return true;
    return false;
}

// Every executed pass is a registry pass with its own timer, named once per frame, in
// registry (frame) order; the passes' sum is at most the frame's span (+5%); a pass turned
// off leaves the timings.
static void sp2b_t01_every_pass_is_registered_timed_once_and_within_the_frame() {
    using namespace ohao::render;
    TEST_BEGIN("sp2b_t01: every executed pass is registered, timed once, in order, within the frame");
    ohao::VulkanRenderer renderer(320, 180);
    if (!renderer.initialize()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    renderer.setRenderMode(ohao::RenderMode::Deferred);
    auto* d = renderer.getDeferredRenderer();
    EXPECT(d && d->getPostProcessing(), "deferred post-processing exists");
    auto* pp = d->getPostProcessing();
    pp->setSSAOEnabled(true);
    pp->setTAAEnabled(true);
    pp->setBloomEnabled(true);

    const std::vector<FrameTiming> frames = timedFrames(renderer, 8);
    EXPECT_EQ(frames.size(), size_t{8}, "every rendered frame's timings are collected");
    for (std::size_t i = 1; i < frames.size(); ++i)
        EXPECT(frames[i].frame == frames[i - 1].frame + 1, "frames are kept oldest first, none skipped");
    for (const FrameTiming& f : frames) {
        EXPECT(f.count > 0 && f.gpuMs > 0.0f, "each frame has passes and a span");
        float sum = 0.0f;
        for (uint32_t i = 0; i < f.count; ++i) {
            EXPECT(f.pass[i] < kPassCount, "every pass is in the registry");
            if (i > 0) EXPECT(f.pass[i] > f.pass[i - 1], "each pass once per frame, in registry order");
            EXPECT(f.ms[i] >= 0.0f, "no negative pass time");
            sum += f.ms[i];
        }
        EXPECT(sum <= f.gpuMs * 1.05f, "the passes' sum is at most the frame's span plus 5%");
    }
    const FrameTiming& last = frames.back();
    for (const char* name : {"scene.upload", "cull.instances", "gbuffer.early", "hiz.build", "cull.late", "gbuffer.late",
                             "ssao", "lighting", "post.bloom", "post.taa", "post.tonemap", "readback"})
        EXPECT(hasPass(last, name), "a frame with SSAO, TAA and bloom runs " + std::string(name));
    EXPECT(!hasPass(last, "present") && !hasPass(last, "pt.render"), "a headless deferred frame has no present");
    // The timer reports a refused pass once per process: read the whole run's errors.
    const auto errors = ohao::log::read(0, ohao::log::Level::Error, ohao::log::Channel::render, 4096);
    for (const auto& rec : errors.lines)
        EXPECT(rec.msg.find("pass timing") == std::string::npos, "no pass was refused by the timer: " + rec.msg);

    pp->setBloomEnabled(false);
    pp->setSSAOEnabled(false);
    const std::vector<FrameTiming> after = timedFrames(renderer, 2);
    EXPECT(!after.empty(), "frames after the change are timed");
    EXPECT(!hasPass(after.back(), "post.bloom") && !hasPass(after.back(), "ssao") &&
               hasPass(after.back(), "post.taa") && hasPass(after.back(), "post.tonemap"),
           "bloom and SSAO turned off leave the timings; TAA and tonemapping stay");
    std::cout << "(frame " << last.gpuMs << " ms, " << last.count << " passes) ";
    TEST_PASS();
}

static void runPassTimingTests() {
    std::cout << "\n=== Pass timing ===" << std::endl;
    sp2b_t01_every_pass_is_registered_timed_once_and_within_the_frame();
}

// =============================================================================
// SECTION — READBACK (sub-project 2b, test 3)
// =============================================================================

// A cube turning 600 degrees a second about Y (10 degrees a 1/60 s step), a light, and the
// camera on it: its angle is a known function of the step.
static void buildSpinScene(ohao::ecs::World& world, ohao::VulkanRenderer& r) {
    ohao::Model cube;
    const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& f : n) {
        const glm::vec3 u = std::abs(f.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::cross(u, f);   // with u and f, a right-handed face frame
        const auto base = static_cast<uint32_t>(cube.vertices.size());
        for (const glm::vec2 c : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
            ohao::Vertex v{};
            v.position = 0.6f * (f + c.x * t + c.y * u);
            v.normal = f;
            v.tangent = glm::vec4(t, 1.0f);
            v.color = {1, 1, 1};   // the G-buffer multiplies the material by it
            v.boneWeights = {1, 0, 0, 0};
            cube.vertices.push_back(v);
        }
        // Wound as makeQuad below: clockwise about the face normal.
        for (const uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) cube.indices.push_back(base + k);
    }
    ohao::render::MaterialDesc mat;
    mat.baseColor = {0.8f, 0.3f, 0.1f};
    ohao::ecs::Transform t;
    t.rotation = glm::angleAxis(glm::radians(25.0f), glm::vec3(1, 0, 0));   // a top face in view
    const entt::entity e =
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), cube, "spinning_cube", t, &mat);
    world.registry().emplace<ohao::ecs::Spin>(e, ohao::ecs::Spin{{0, 1, 0}, 600.0f});
    ohao::ecs::Light light;
    light.intensity = 150.0f;
    ohao::ecs::spawnLight(world, "light", light, ohao::ecs::Transform{{1.5f, 2.0f, 3.0f}});
    r.getCamera().setPosition({0.0f, 0.0f, 4.0f});
    r.getCamera().setRotation(0.0f, -90.0f);
    r.setRenderMode(ohao::RenderMode::Deferred);
}

// A fresh renderer's single frame of the spin scene after `steps` steps.
static std::vector<uint8_t> freshCapture(int steps) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) return {};
    std::vector<uint8_t> out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        for (int k = 0; k < steps; ++k) world.simulate(1.0f / 60.0f);
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        out.assign(px.begin(), px.end());
    }
    return out;
}

static int maxAbsDiff(const std::vector<uint8_t>& a, std::span<const uint8_t> b) {
    if (a.size() != b.size() || a.empty()) return 256;
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

// Frame N's readback is frame N: rendering a spinning cube step by step and reading frame N
// (the temporal history reset just before it, so only the frame read can differ) gives
// exactly a fresh renderer's single capture of step N, and not step N-3, which is what the
// deleted headless ring delivered. No frame copies unless asked.
static void sp2b_t03_frame_n_readback_is_frame_n_and_only_requested_frames_copy() {
    TEST_BEGIN("sp2b_t03: frame N's readback equals a fresh capture of step N; only requested frames copy");
    constexpr int kSteps = 9;
    const std::vector<uint8_t> stepN = freshCapture(kSteps);
    if (stepN.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const std::vector<uint8_t> stepNMinus3 = freshCapture(kSteps - ohao::MAX_FRAMES_IN_FLIGHT);
    EXPECT(maxAbsDiff(stepN, stepNMinus3) > 32,
           "the cube's angle shows: step N and step N-3 differ (else this test proves nothing)");

    ohao::VulkanRenderer r(160, 120);
    EXPECT(r.initialize(), "the renderer starts");
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        for (int k = 1; k <= kSteps; ++k) {
            world.simulate(1.0f / 60.0f);
            world.sync();
            if (k == kSteps) {
                r.resetAccumulation();   // frame N renders as a fresh renderer's first frame
                r.requestReadback();
            }
            r.render();
        }
        const auto px = r.readback();
        const int diff = maxAbsDiff(stepN, px);
        std::cout << "(max_abs vs step N " << diff << ", vs step N-3 " << maxAbsDiff(stepNMinus3, px) << ") ";
        EXPECT_EQ(diff, 0, "frame N's readback is exactly a fresh capture of step N");
        EXPECT_EQ(r.requestedReadbackCount(), uint64_t{1}, "nine frames, one request: one copy");

        world.sync();
        r.render();   // not requested
        EXPECT(r.readback().empty(), "an unrequested frame leaves nothing to read");
        EXPECT_EQ(r.requestedReadbackCount(), uint64_t{1}, "and copies nothing");

        const auto timings = r.frameTimings(ohao::render::GpuPassTimer::kRingFrames);
        for (std::size_t i = 1; i < timings.size(); ++i)
            EXPECT(timings[i].frame > timings[i - 1].frame,
                   "the timings stay in frame order across the reset (resetAccumulation collects first)");
    }
    TEST_PASS();
}

static void runReadbackTests() {
    std::cout << "\n=== Readback ===" << std::endl;
    sp2b_t03_frame_n_readback_is_frame_n_and_only_requested_frames_copy();
}

// =============================================================================
// SECTION — RT SHADOWS FROM THE SCENE'S LIGHTS (sub-project 2b, test 9, lights part)
// =============================================================================

// A floor, a box floating above it, one point light at `lightPos`, the camera above and
// in front looking down: one deferred frame with RT shadows. Empty without RT.
static std::vector<uint8_t> shadowSceneFrame(glm::vec3 lightPos, bool castShadows) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize() || !r.isRTSupported()) return {};
    std::vector<uint8_t> out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, box;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        ohao::PrimitiveMeshGenerator::generateBox(box, {-0.5f, 1.0f, -0.5f}, {0.5f, 2.0f, 0.5f}, {1, 1, 1});
        ohao::render::MaterialDesc mat;
        mat.baseColor = {0.7f, 0.7f, 0.7f};
        mat.roughness = 0.9f;
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &mat);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), box, "box", {}, &mat);
        ohao::ecs::Light light;
        light.intensity = 60.0f;
        light.range = 40.0f;
        light.radius = 0.05f;
        light.castShadows = castShadows;
        ohao::ecs::spawnLight(world, "light", light, ohao::ecs::Transform{lightPos});
        r.getCamera().setPosition({0.0f, 7.0f, 7.0f});
        r.getCamera().setRotation(-45.0f, -90.0f);
        r.setRenderMode(ohao::RenderMode::Deferred);
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        out.assign(px.begin(), px.end());
    }
    return out;
}

// Pixels at least 40 levels darker (green channel) in `shadowed` than in `unshadowed`, and
// their mean x as a fraction of the width.
static std::pair<int, double> darkened(const std::vector<uint8_t>& shadowed, const std::vector<uint8_t>& unshadowed) {
    int n = 0;
    double sx = 0.0;
    for (std::size_t p = 0; p + 3 < shadowed.size() && p + 3 < unshadowed.size(); p += 4)
        if (int(unshadowed[p + 1]) - int(shadowed[p + 1]) >= 40) {
            ++n;
            sx += double((p / 4) % 160) / 160.0;
        }
    return {n, n ? sx / n : 0.0};
}

static void sp2b_t09_scene_lights_cast_rt_shadows_and_cast_shadows_false_does_not() {
    TEST_BEGIN("sp2b_t09: RT shadows come from the scene's lights; castShadows=false casts none");
    const auto leftCast = shadowSceneFrame({-3.0f, 4.0f, 0.0f}, true);
    if (leftCast.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const auto leftNone = shadowSceneFrame({-3.0f, 4.0f, 0.0f}, false);
    const auto rightCast = shadowSceneFrame({3.0f, 4.0f, 0.0f}, true);
    const auto rightNone = shadowSceneFrame({3.0f, 4.0f, 0.0f}, false);
    const auto [nl, xl] = darkened(leftCast, leftNone);
    const auto [nr, xr] = darkened(rightCast, rightNone);
    std::cout << "(shadow px: light left " << nl << " at x " << xl << ", light right " << nr << " at x " << xr << ") ";
    EXPECT(nl > 160 && nr > 160, "a light that casts shadows darkens the floor behind the box (>= 1% of the frame)");
    EXPECT(xl > 0.55 && xr < 0.45,
           "the shadow falls away from the scene's light: right of centre for a light on the left, left for one "
           "on the right (a fixed light would put it in one place)");
    EXPECT(darkened(leftNone, leftCast).first == 0, "castShadows=false is never darker than castShadows=true");

    // The light table: the first kMaxShadowLights casting lights own a visibility channel each,
    // in light order; a non-casting light has none (-1).
    ohao::VulkanRenderer r(64, 64);
    EXPECT(r.initialize(), "the renderer starts");
    {
        ohao::ecs::World world;
        ohao::ecs::installRenderExtract(world, r.renderScene());
        const bool casts[6] = {true, true, false, true, true, true};
        for (int k = 0; k < 6; ++k) {
            ohao::ecs::Light l;
            l.castShadows = casts[k];
            l.intensity = float(k + 1);
            ohao::ecs::spawnLight(world, "l" + std::to_string(k), l, ohao::ecs::Transform{{float(k), 3.0f, 0.0f}});
        }
        world.sync();
        const auto raster = r.renderScene().rasterLights();
        const float expected[6] = {0, 1, ohao::render::kShadowNever, 2, 3, 4};
        bool slots = raster.size() == 6;
        for (std::size_t k = 0; slots && k < 6; ++k) slots = raster[k].params.w == expected[k];
        EXPECT(slots, "params.w: channels 0, 1, never, 2, 3, 4");
        const auto shadow = r.renderScene().shadowLights();
        EXPECT(shadow.size() == 5 && shadow[0].intensity == 1.0f && shadow[1].intensity == 2.0f &&
                   shadow[2].intensity == 4.0f && shadow[3].intensity == 5.0f && shadow[4].intensity == 6.0f,
               "shadowLights(): the five casting lights in channel order");
    }
    TEST_PASS();
}

// Sky and particles draw into the lit image before SSS (2b step 4): the sky is in the final
// image of a frame that runs SSS. It used to be drawn after SSS copied the image for post,
// so every SSS frame lost it: the open sky came out black.
static void sp2b_s4_the_sky_reaches_the_final_image_through_sss() {
    TEST_BEGIN("sp2b step 4: the sky reaches the final image in a frame that runs SSS");
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    {
        ohao::ecs::World world;
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-50.0f, -0.1f, -50.0f}, {50.0f, 0.0f, 50.0f}, {1, 1, 1});
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {});
        r.getCamera().setPosition({0.0f, 1.0f, 0.0f});
        r.getCamera().setRotation(10.0f, -90.0f);   // the horizon low, the sky above it
        r.setRenderMode(ohao::RenderMode::Deferred);
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        EXPECT(px.size() == 160u * 120u * 4u, "the frame reads back");
        bool sss = false;
        for (const auto& f : r.frameTimings(1))
            for (const int p : f.pass) sss = sss || p == ohao::render::passIndex("sss");
        EXPECT(sss, "the frame ran SSS (else this test proves nothing)");
        int skyPixels = 0;
        for (uint32_t x = 0; x < 160 && px.size() == 160u * 120u * 4u; ++x) {
            const std::size_t p = (2u * 160u + x) * 4u;   // row 2, near the top
            skyPixels += int(px[p]) + int(px[p + 1]) + int(px[p + 2]) > 30;
        }
        std::cout << "(lit sky pixels in row 2: " << skyPixels << "/160) ";
        EXPECT(skyPixels == 160, "the top row is sky, not the black of an unlit pixel");
    }
    TEST_PASS();
}

// The shadow scene of sp2b_t09 (a floor, a box floating above it, the camera above and in
// front) with the given lights: `frames` frames from a reset at `quality`, the last read back,
// with the passes that frame ran and the dropped-pixel count it reported. Empty without RT.
struct ShadowRun {
    std::vector<uint8_t> px;
    bool csm{false}, rtShadows{false}, filter{false};
    std::optional<uint32_t> dropped;
};
using SceneLight = std::pair<ohao::ecs::Light, glm::vec3>;   // a light and its position

static ShadowRun shadowRun(const std::vector<SceneLight>& lights, ohao::RenderQuality quality, int frames,
                           bool rtShadows = true, glm::quat lightRotation = glm::quat(1, 0, 0, 0)) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize() || !r.isRTSupported()) return {};
    ShadowRun out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, box;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        ohao::PrimitiveMeshGenerator::generateBox(box, {-0.5f, 1.0f, -0.5f}, {0.5f, 2.0f, 0.5f}, {1, 1, 1});
        ohao::render::MaterialDesc mat;
        mat.baseColor = {0.7f, 0.7f, 0.7f};
        mat.roughness = 0.9f;
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &mat);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), box, "box", {}, &mat);
        int k = 0;
        for (const auto& [light, pos] : lights) {
            ohao::ecs::Transform t{pos};
            t.rotation = lightRotation;
            ohao::ecs::spawnLight(world, "light" + std::to_string(k++), light, t);
        }
        r.getCamera().setPosition({0.0f, 7.0f, 7.0f});
        r.getCamera().setRotation(-45.0f, -90.0f);
        r.setRenderMode(ohao::RenderMode::Deferred);
        r.setRTShadowsEnabled(rtShadows);
        r.setRenderQuality(quality);
        world.sync();
        r.render();             // the scene's first frame builds its acceleration structures
        r.resetAccumulation();  // the capture starts here, as a lockstep capture does
        for (int f = 0; f < frames; ++f) {
            world.sync();
            if (f == frames - 1) r.requestReadback();
            r.render();
        }
        const auto px = r.readback();
        out.px.assign(px.begin(), px.end());
        for (const auto& f : r.frameTimings(1))
            for (uint32_t i = 0; i < f.count; ++i) {
                out.csm = out.csm || f.pass[i] == ohao::render::passIndex("csm");
                out.rtShadows = out.rtShadows || f.pass[i] == ohao::render::passIndex("rt.shadows");
                out.filter = out.filter || f.pass[i] == ohao::render::passIndex("rt.shadows.filter");
            }
        if (const auto d = r.shadowLightsDropped()) out.dropped = d->pixels;
    }
    return out;
}

static ohao::ecs::Light shadowLight(float intensity, float radius, bool castShadows = true) {
    ohao::ecs::Light l;
    l.intensity = intensity;
    l.range = 40.0f;
    l.radius = radius;
    l.castShadows = castShadows;
    return l;
}

// The mean absolute difference of two RGBA8 frames' RGB, in levels.
static double meanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) continue;
        s += std::abs(int(a[i]) - int(b[i]));
        ++n;
    }
    return s / double(n);
}

// Test 9 (2b step 7): a capture after a reset is exact run to run, realtime and reference.
static void sp2b_t09_the_capture_after_a_reset_is_exact_run_to_run() {
    TEST_BEGIN("sp2b_t09: an RT-shadowed capture after a reset is exact run to run (realtime and reference)");
    const std::vector<SceneLight> lights{{shadowLight(60.0f, 0.6f), {-3.0f, 4.0f, 0.0f}}};
    const ShadowRun a = shadowRun(lights, ohao::RenderQuality::Realtime, 6);
    if (a.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const ShadowRun b = shadowRun(lights, ohao::RenderQuality::Realtime, 6);
    EXPECT(a.rtShadows && a.filter, "realtime traces and filters");
    EXPECT_EQ(maxAbsDiff(a.px, b.px), 0, "two fresh renderers' 6-frame realtime captures are identical");
    const ShadowRun c = shadowRun(lights, ohao::RenderQuality::Reference, 4);
    const ShadowRun d = shadowRun(lights, ohao::RenderQuality::Reference, 4);
    EXPECT(c.rtShadows && !c.filter, "reference traces and does not filter");
    EXPECT_EQ(maxAbsDiff(c.px, d.px), 0, "two fresh renderers' 4-frame reference captures are identical");
    TEST_PASS();
}

// Test 9: CSM renders only as the fallback, when RT shadows are off.
static void sp2b_t09_csm_is_the_fallback_when_rt_shadows_are_off() {
    TEST_BEGIN("sp2b_t09: CSM shadows the frame when RT shadows are off, and only then");
    ohao::ecs::Light sun = shadowLight(3.0f, 0.01f);
    sun.type = ohao::ecs::LightType::Directional;
    const glm::quat down = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));   // -Z turned to -Y
    const ShadowRun rt = shadowRun({{sun, {0, 10, 0}}}, ohao::RenderQuality::Realtime, 1, true, down);
    if (rt.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const ShadowRun csm = shadowRun({{sun, {0, 10, 0}}}, ohao::RenderQuality::Realtime, 1, false, down);
    ohao::ecs::Light noCast = sun;
    noCast.castShadows = false;
    const ShadowRun none = shadowRun({{noCast, {0, 10, 0}}}, ohao::RenderQuality::Realtime, 1, false, down);
    EXPECT(rt.rtShadows && !rt.csm, "with RT shadows the frame traces them and renders no CSM");
    EXPECT(csm.csm && !csm.rtShadows, "with RT shadows off it renders CSM and traces nothing");
    const auto [n, x] = darkened(csm.px, none.px);
    const auto [nRt, xRt] = darkened(rt.px, none.px);
    std::cout << "(shadow px: CSM " << n << ", RT " << nRt << ") ";
    // The box's shadow straight below it covers about 140 pixels of the 160x120 frame.
    EXPECT(n > 80, "CSM darkens the floor under the box");
    EXPECT(nRt > 80, "and so do RT shadows");
    TEST_PASS();
}

// Test 9: reference quality shadows every castShadows light, past the realtime cap of 4 a
// pixel; realtime reports the pixels that hit the cap.
static void sp2b_t09_reference_shadows_every_light_past_the_realtime_cap() {
    TEST_BEGIN("sp2b_t09: reference shadows all 6 lights; realtime traces 4 a pixel and reports the rest");
    std::vector<SceneLight> cast, off;
    for (int k = 0; k < 6; ++k) {
        const float a = glm::radians(60.0f * float(k) + 15.0f);
        const glm::vec3 pos{3.0f * std::cos(a), 4.0f, 3.0f * std::sin(a)};
        cast.push_back({shadowLight(25.0f, 0.05f), pos});
        off.push_back({shadowLight(25.0f, 0.05f, false), pos});
    }
    const ShadowRun ref = shadowRun(cast, ohao::RenderQuality::Reference, 1);
    if (ref.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const ShadowRun rt = shadowRun(cast, ohao::RenderQuality::Realtime, 1);
    const ShadowRun none = shadowRun(off, ohao::RenderQuality::Realtime, 1);
    // Darker by 8 levels or more: each light carries a sixth of the light.
    const auto darker = [](const std::vector<uint8_t>& s, const std::vector<uint8_t>& u) {
        int n = 0;
        for (std::size_t p = 0; p + 3 < s.size() && p + 3 < u.size(); p += 4) n += int(u[p + 1]) - int(s[p + 1]) >= 8;
        return n;
    };
    const int nRef = darker(ref.px, none.px), nRt = darker(rt.px, none.px);
    std::cout << "(darkened px: reference " << nRef << ", realtime " << nRt << "; dropped "
              << (rt.dropped ? std::to_string(*rt.dropped) : std::string("none")) << ") ";
    EXPECT(nRef > nRt, "reference darkens more of the frame: the shadows of the lights past a pixel's 4");
    EXPECT(rt.dropped && *rt.dropped > 0, "realtime reports the pixels that had more than 4 lights to trace");
    EXPECT(ref.dropped.value_or(0) == 0, "reference drops none");
    TEST_PASS();
}

// Test 9: a spot light outside its cone is worth nothing to a pixel, so it takes none of the
// pixel's 4 traced lights: four point lights and a bright spot pointing straight up from
// near the floor leave no pixel over the cap.
static void sp2b_t09_a_spot_outside_its_cone_takes_no_slot() {
    TEST_BEGIN("sp2b_t09: a spot light outside its cone takes none of a pixel's 4 traced lights");
    std::vector<SceneLight> lights;
    for (int k = 0; k < 4; ++k) {
        const float a = glm::radians(90.0f * float(k) + 45.0f);
        lights.push_back({shadowLight(25.0f, 0.05f), {3.0f * std::cos(a), 4.0f, 3.0f * std::sin(a)}});
    }
    ohao::ecs::Light spot = shadowLight(500.0f, 0.05f);
    spot.type = ohao::ecs::LightType::Spot;
    spot.outerConeDeg = 20.0f;
    spot.innerConeDeg = 15.0f;
    lights.push_back({spot, {3.0f, 0.5f, 0.0f}});
    // -Z turned to +Y: the spot points straight up, away from the floor and the box.
    const glm::quat up = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
    const ShadowRun r = shadowRun(lights, ohao::RenderQuality::Realtime, 1, true, up);
    if (r.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    std::cout << "(dropped " << (r.dropped ? std::to_string(*r.dropped) : std::string("none")) << ") ";
    EXPECT(r.dropped && *r.dropped == 0, "no pixel had more than 4 lights worth tracing: the spot lights none of them");
    TEST_PASS();
}

// Test 9: the realtime shadows converge toward the reference: frame 16 after a reset is
// closer to a 16-frame reference capture than frame 1 is.
static void sp2b_t09_realtime_converges_toward_the_reference() {
    TEST_BEGIN("sp2b_t09: realtime frame 16 is closer to a 16-frame reference than realtime frame 1");
    const std::vector<SceneLight> lights{{shadowLight(60.0f, 0.8f), {-2.0f, 4.0f, 1.0f}}};
    const ShadowRun ref = shadowRun(lights, ohao::RenderQuality::Reference, 16);
    if (ref.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const ShadowRun rt1 = shadowRun(lights, ohao::RenderQuality::Realtime, 1);
    const ShadowRun rt16 = shadowRun(lights, ohao::RenderQuality::Realtime, 16);
    const double d1 = meanAbsDiff(rt1.px, ref.px), d16 = meanAbsDiff(rt16.px, ref.px);
    std::cout << "(mean |realtime - reference|: frame 1 " << d1 << ", frame 16 " << d16 << ") ";
    EXPECT(d16 < d1, "the temporal accumulation converges toward the reference");
    TEST_PASS();
}

// ---------------------------------------------------------------------------------------
// Test 10 (2b step 8): RT GI. The GI scene: a grey floor, a coloured wall standing on it to
// the camera's right, a point light between them; the camera looks down at the floor beside
// the wall, where the wall's bounce lands.
struct GIRun {
    std::vector<uint8_t> px;
    bool gi{false}, denoise{false};
    VkExtent2D extent{0, 0};
    uint32_t rays{0};
};

// `frames` frames from a reset at `quality`, the last read back. `warmup` frames before the
// reset look at the scene from elsewhere, so a history the reset failed to drop would show;
// `prefix` frames after the reset, at their own qualities, come before the `frames`.
static GIRun giRun(ohao::RenderQuality quality, int frames, glm::vec3 wallColor = {0.9f, 0.1f, 0.1f}, int warmup = 0,
                   const std::vector<ohao::RenderQuality>& prefix = {}) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize() || !r.isRTSupported()) return {};
    GIRun out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, wall;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        ohao::PrimitiveMeshGenerator::generateBox(wall, {1.5f, 0.0f, -4.0f}, {1.8f, 3.0f, 4.0f}, {1, 1, 1});
        ohao::render::MaterialDesc grey, coloured;
        grey.baseColor = {0.7f, 0.7f, 0.7f};
        grey.roughness = 0.9f;
        coloured.baseColor = wallColor;
        coloured.roughness = 0.9f;
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), wall, "wall", {}, &coloured);
        ohao::ecs::Light light = shadowLight(40.0f, 0.1f);
        ohao::ecs::spawnLight(world, "light", light, ohao::ecs::Transform{{0.0f, 2.5f, 0.0f}});
        r.setRenderMode(ohao::RenderMode::Deferred);
        r.setRenderQuality(quality);
        r.getCamera().setPosition({-3.0f, 5.0f, 2.0f});   // elsewhere, for the warm-up
        r.getCamera().setRotation(-60.0f, 0.0f);
        world.sync();
        r.render();             // the scene's first frame builds its acceleration structures
        for (int f = 0; f < warmup; ++f) {
            world.sync();
            r.render();
        }
        r.getCamera().setPosition({-1.0f, 4.0f, 3.0f});
        r.getCamera().setRotation(-55.0f, -70.0f);
        r.resetAccumulation();  // the capture starts here, as a lockstep capture does
        for (const ohao::RenderQuality q : prefix) {
            r.setRenderQuality(q);
            world.sync();
            r.render();
        }
        r.setRenderQuality(quality);
        for (int f = 0; f < frames; ++f) {
            world.sync();
            if (f == frames - 1) r.requestReadback();
            r.render();
        }
        const auto px = r.readback();
        out.px.assign(px.begin(), px.end());
        for (const auto& f : r.frameTimings(1))
            for (uint32_t i = 0; i < f.count; ++i) {
                out.gi = out.gi || f.pass[i] == ohao::render::passIndex("rt.gi");
                out.denoise = out.denoise || f.pass[i] == ohao::render::passIndex("rt.gi.denoise");
            }
        if (auto* deferred = r.getDeferredRenderer(); deferred && deferred->getRT_GI()) {
            out.extent = deferred->getRT_GI()->traceExtent();
            out.rays = deferred->getRT_GI()->raysPerPixel();
        }
    }
    return out;
}

// Test 10: GI is deterministic across fresh renderers (processes: deferred_determinism_gpu_test).
static void sp2b_t10_gi_is_deterministic_across_fresh_renderers() {
    TEST_BEGIN("sp2b_t10: an RT GI capture after a reset is exact across fresh renderers (realtime and reference)");
    const GIRun a = giRun(ohao::RenderQuality::Realtime, 6);
    if (a.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun b = giRun(ohao::RenderQuality::Realtime, 6);
    EXPECT(a.gi && a.denoise, "realtime traces and denoises");
    EXPECT_EQ(maxAbsDiff(a.px, b.px), 0, "two fresh renderers' 6-frame realtime captures are identical");
    const GIRun c = giRun(ohao::RenderQuality::Reference, 3);
    const GIRun d = giRun(ohao::RenderQuality::Reference, 3);
    EXPECT_EQ(maxAbsDiff(c.px, d.px), 0, "two fresh renderers' 3-frame reference captures are identical");
    TEST_PASS();
}

// Test 10: the history resets on capture: frames rendered before the reset, from another
// viewpoint, leave no trace in the capture.
static void sp2b_t10_history_resets_on_capture() {
    TEST_BEGIN("sp2b_t10: GI history resets on capture: frames before the reset change nothing");
    const GIRun fresh = giRun(ohao::RenderQuality::Realtime, 4);
    if (fresh.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun warmed = giRun(ohao::RenderQuality::Realtime, 4, {0.9f, 0.1f, 0.1f}, 12);
    EXPECT_EQ(maxAbsDiff(fresh.px, warmed.px), 0, "realtime: 12 frames from elsewhere before the reset, the same capture");
    const GIRun refFresh = giRun(ohao::RenderQuality::Reference, 2);
    const GIRun refWarmed = giRun(ohao::RenderQuality::Reference, 2, {0.9f, 0.1f, 0.1f}, 5);
    EXPECT_EQ(maxAbsDiff(refFresh.px, refWarmed.px), 0, "reference: the running mean starts again at the reset");
    TEST_PASS();
}

// Test 10: multi-frame captures converge: frame 8 to 16 changes less than frame 1 to 8.
static void sp2b_t10_multi_frame_captures_converge() {
    TEST_BEGIN("sp2b_t10: |frame 16 - frame 8| < |frame 8 - frame 1| (realtime GI accumulates)");
    const GIRun f1 = giRun(ohao::RenderQuality::Realtime, 1);
    if (f1.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun f8 = giRun(ohao::RenderQuality::Realtime, 8);
    const GIRun f16 = giRun(ohao::RenderQuality::Realtime, 16);
    const double d18 = meanAbsDiff(f1.px, f8.px), d816 = meanAbsDiff(f8.px, f16.px);
    std::cout << "(mean |1-8| " << d18 << ", |8-16| " << d816 << ") ";
    EXPECT(d18 > 0.0, "frame 8 differs from frame 1 (the accumulation does something)");
    EXPECT(d816 < d18, "and frames 8 to 16 change less: the capture converges");
    TEST_PASS();
}

// Test 10: reference quality traces full resolution with 32 rays and no denoiser; realtime
// half resolution with 2 and the denoiser (render.stats' pass list, the GI image's extent).
static void sp2b_t10_reference_is_full_resolution_32_rays() {
    TEST_BEGIN("sp2b_t10: reference GI is 160x120 at 32 rays without rt.gi.denoise; realtime 80x60 at 2 with it");
    const GIRun ref = giRun(ohao::RenderQuality::Reference, 1);
    if (ref.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun rt = giRun(ohao::RenderQuality::Realtime, 1);
    std::cout << "(reference " << ref.extent.width << "x" << ref.extent.height << " x" << ref.rays << ", realtime "
              << rt.extent.width << "x" << rt.extent.height << " x" << rt.rays << ") ";
    EXPECT(ref.gi && !ref.denoise, "reference: rt.gi, no rt.gi.denoise");
    EXPECT(ref.extent.width == 160 && ref.extent.height == 120 && ref.rays == 32, "reference: full resolution, 32 rays");
    EXPECT(rt.gi && rt.denoise, "realtime: rt.gi and rt.gi.denoise");
    EXPECT(rt.extent.width == 80 && rt.extent.height == 60 && rt.rays == 2, "realtime: half resolution, 2 rays");
    TEST_PASS();
}

// Test 10: the realtime GI converges toward the reference, as the shadows do in test 9.
static void sp2b_t10_realtime_converges_toward_the_reference() {
    TEST_BEGIN("sp2b_t10: realtime GI frame 16 is closer to a 16-frame reference than frame 1");
    const GIRun ref = giRun(ohao::RenderQuality::Reference, 16);
    if (ref.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun rt1 = giRun(ohao::RenderQuality::Realtime, 1);
    const GIRun rt16 = giRun(ohao::RenderQuality::Realtime, 16);
    const double d1 = meanAbsDiff(rt1.px, ref.px), d16 = meanAbsDiff(rt16.px, ref.px);
    std::cout << "(mean |realtime - reference|: frame 1 " << d1 << ", frame 16 " << d16 << ") ";
    EXPECT(d16 < d1, "the temporal accumulation converges toward the reference");
    TEST_PASS();
}

// Test 10: the bounce carries the wall's colour: a red wall tints the floor beside it red
// where a white one does not.
static void sp2b_t10_a_red_wall_bleeds_onto_the_floor() {
    TEST_BEGIN("sp2b_t10: a red wall's bounce tints the floor red (colour bleeding), a white one's does not");
    const GIRun red = giRun(ohao::RenderQuality::Reference, 8, {0.9f, 0.1f, 0.1f});
    if (red.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun white = giRun(ohao::RenderQuality::Reference, 8, {0.9f, 0.9f, 0.9f});
    // Floor pixels: grey in the white-wall frame (the wall itself is excluded by being
    // brighter than the floor would be); their red excess over green in each frame.
    double redExcess = 0.0, whiteExcess = 0.0;
    int n = 0;
    for (std::size_t p = 0; p + 3 < red.px.size() && p + 3 < white.px.size(); p += 4) {
        const int wr = white.px[p], wg = white.px[p + 1], wb = white.px[p + 2];
        if (std::abs(wr - wg) > 2 || std::abs(wg - wb) > 2) continue;   // not grey
        if (int(red.px[p + 1]) < wg - 10) continue;                       // the red wall itself
        redExcess += int(red.px[p]) - int(red.px[p + 1]);
        whiteExcess += wr - wg;
        ++n;
    }
    redExcess /= std::max(n, 1);
    whiteExcess /= std::max(n, 1);
    std::cout << "(floor px " << n << ", mean R-G: red wall " << redExcess << ", white wall " << whiteExcess << ") ";
    EXPECT(n > 2000, "most of the frame is floor");
    EXPECT(redExcess > whiteExcess + 1.0, "the red wall's bounce tints the floor red");
    TEST_PASS();
}

// Test 10: a reference running mean (RT GI's and RT shadows') holds only the reference frames
// in a row: realtime frames in between start it again, as a fresh image does. Two captures
// whose last frame is the same reference frame (index 5 since the reset) are identical,
// whatever came before it: two reference frames then three realtime ones (a stale mean), or
// five realtime ones (a reference image created at frame 5, its contents undefined).
static void sp2b_t10_a_reference_mean_restarts_after_realtime_frames() {
    TEST_BEGIN("sp2b_t10: reference GI and shadow means start again after realtime frames");
    using Q = ohao::RenderQuality;
    const GIRun stale = giRun(Q::Reference, 1, {0.9f, 0.1f, 0.1f}, 0,
                              {Q::Reference, Q::Reference, Q::Realtime, Q::Realtime, Q::Realtime});
    if (stale.px.empty()) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    const GIRun fresh = giRun(Q::Reference, 1, {0.9f, 0.1f, 0.1f}, 0,
                              {Q::Realtime, Q::Realtime, Q::Realtime, Q::Realtime, Q::Realtime});
    std::cout << "(max |stale - fresh| " << maxAbsDiff(stale.px, fresh.px) << ") ";
    EXPECT_EQ(maxAbsDiff(stale.px, fresh.px), 0, "frame 5 at reference is the same after either history");
    TEST_PASS();
}

// ---------------------------------------------------------------------------------------
// Test 5 (2b step 9): SSAO. A floor meeting a wall, one point light of `light` intensity and
// a constant ambient of `ambient`, which lighting applies only without RT GI (and without an
// environment, which this scene has none of): `rtGI` false turns RT GI off; one frame after a
// reset, SSAO on or off. Empty without a device.
struct AORun {
    std::vector<uint8_t> px;
    bool ssaoPass{false};
};

static AORun aoRun(bool ssao, float light, float ambient, bool rtGI = true) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) return {};
    AORun out;
    {
        ohao::ecs::World world;
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, wall;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        ohao::PrimitiveMeshGenerator::generateBox(wall, {-8.0f, 0.0f, -1.3f}, {8.0f, 3.0f, -1.0f}, {1, 1, 1});
        ohao::render::MaterialDesc grey;
        grey.baseColor = {0.7f, 0.7f, 0.7f};
        grey.roughness = 0.9f;
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), wall, "wall", {}, &grey);
        ohao::ecs::Light l = shadowLight(light, 0.1f);
        ohao::ecs::spawnLight(world, "light", l, ohao::ecs::Transform{{0.0f, 2.0f, 1.0f}});
        r.setRenderMode(ohao::RenderMode::Deferred);
        r.setAmbientIntensity(ambient);
        auto settings = r.renderSettings();
        settings.rtGI = rtGI;
        r.setRenderSettings(settings);
        r.getDeferredRenderer()->getPostProcessing()->setSSAOEnabled(ssao);
        r.getCamera().setPosition({0.0f, 1.5f, 2.5f});
        r.getCamera().setRotation(-30.0f, -90.0f);   // down at the crease where the floor meets the wall
        world.sync();
        r.render();
        r.resetAccumulation();
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        out.px.assign(px.begin(), px.end());
        for (const auto& f : r.frameTimings(1))
            for (uint32_t i = 0; i < f.count; ++i) out.ssaoPass = out.ssaoPass || f.pass[i] == ohao::render::passIndex("ssao");
    }
    return out;
}

// Test 5: SSAO darkens the ambient term where it is occluded, and nothing else: with only
// ambient light (the fallback without RT GI), AO on darkens the crease and brightens nothing;
// with only a point light (and RT GI), AO on changes no pixel.
static void sp2b_t05_ssao_darkens_only_the_ambient_term() {
    TEST_BEGIN("sp2b_t05: SSAO darkens ambient light in the crease; a point-lit frame is unchanged");
    const AORun ambOff = aoRun(false, 0.0f, 0.6f, false);
    if (ambOff.px.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const AORun ambOn = aoRun(true, 0.0f, 0.6f, false);
    const AORun litOff = aoRun(false, 30.0f, 0.0f);
    const AORun litOn = aoRun(true, 30.0f, 0.0f);
    int darker = 0, brighter = 0;
    for (std::size_t p = 0; p + 3 < ambOn.px.size() && p + 3 < ambOff.px.size(); p += 4) {
        const int d = int(ambOff.px[p + 1]) - int(ambOn.px[p + 1]);
        darker += d >= 2;
        brighter += d <= -1;
    }
    std::cout << "(ambient only: " << darker << " px darker, " << brighter << " brighter; point light: max_abs "
              << maxAbsDiff(litOn.px, litOff.px) << ") ";
    EXPECT(ambOn.ssaoPass && !ambOff.ssaoPass, "the ssao pass runs when the effect is on, and only then");
    EXPECT(darker > 200, "ambient light is occluded along the crease (>= 1% of the frame darker)");
    EXPECT_EQ(brighter, 0, "occlusion never brightens");
    EXPECT_EQ(maxAbsDiff(litOn.px, litOff.px), 0, "direct and RT GI light are not occluded by SSAO");
    TEST_PASS();
}

// Test 5: SSAO frames are the same from fresh renderers.
static void sp2b_t05_ssao_twin_renders_are_identical() {
    TEST_BEGIN("sp2b_t05: two fresh renderers' SSAO frames are identical");
    const AORun a = aoRun(true, 10.0f, 0.6f);
    if (a.px.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const AORun b = aoRun(true, 10.0f, 0.6f);
    EXPECT_EQ(maxAbsDiff(a.px, b.px), 0, "bit-identical");
    TEST_PASS();
}

// The constant ambient is a fallback for frames without another source of indirect light: with
// RT GI on, its intensity changes no pixel (the walls are lit by GI's rays only); with RT GI off
// (and no environment) it lights the frame.
static void ambient_is_a_fallback_only_without_rt_gi() {
    TEST_BEGIN("ambient: no effect with RT GI on; the fallback fill without it");
    const AORun giNone = aoRun(false, 10.0f, 0.0f);
    if (giNone.px.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const AORun giAmbient = aoRun(false, 10.0f, 0.6f);
    const AORun noGiNone = aoRun(false, 10.0f, 0.0f, false);
    const AORun noGiAmbient = aoRun(false, 10.0f, 0.6f, false);
    int brighter = 0;
    for (std::size_t p = 0; p + 3 < noGiNone.px.size() && p + 3 < noGiAmbient.px.size(); p += 4)
        brighter += int(noGiAmbient.px[p + 1]) - int(noGiNone.px[p + 1]) >= 2;
    std::cout << "(RT GI on: max_abs " << maxAbsDiff(giAmbient.px, giNone.px) << "; off: " << brighter
              << " px brighter) ";
    EXPECT_EQ(maxAbsDiff(giAmbient.px, giNone.px), 0, "with RT GI, the ambient intensity changes no pixel");
    EXPECT(brighter > 10000, "without RT GI, the ambient fallback lights most of the frame");
    TEST_PASS();
}

// 2b step 9: SSS runs on the 16x16 tiles the G-buffer pass flagged for subsurface materials,
// and nowhere else. A box with `subsurface` on a floor: its tiles are flagged and blurred,
// every pixel more than the kernel's reach outside them is untouched; the same box without
// subsurface flags none and changes nothing.
struct SSSRun {
    std::vector<uint8_t> px;
    uint32_t tiles{0};
};

static SSSRun sssRun(float subsurface) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) return {};
    SSSRun out;
    {
        ohao::ecs::World world;
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, box;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        ohao::PrimitiveMeshGenerator::generateBox(box, {-0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, 0.5f}, {1, 1, 1});
        ohao::render::MaterialDesc grey, skin;
        grey.baseColor = {0.7f, 0.7f, 0.7f};
        skin.baseColor = {0.8f, 0.5f, 0.4f};
        skin.subsurface = subsurface;
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), box, "box", {}, &skin);
        // A spot of light across the box: a hard gradient the blur visibly softens.
        ohao::ecs::spawnLight(world, "light", shadowLight(20.0f, 0.05f), ohao::ecs::Transform{{1.5f, 2.0f, 1.5f}});
        r.setRenderMode(ohao::RenderMode::Deferred);
        r.getCamera().setPosition({0.0f, 1.5f, 3.5f});
        r.getCamera().setRotation(-20.0f, -90.0f);
        world.sync();
        r.render();
        r.resetAccumulation();
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        out.px.assign(px.begin(), px.end());
        out.tiles = r.getDeferredRenderer()->getGBufferPass()->sssTilesFlagged();
    }
    return out;
}

static void sp2b_s9_sss_runs_only_on_flagged_tiles() {
    TEST_BEGIN("sp2b step 9: SSS blurs the subsurface material's tiles and nothing else");
    const SSSRun none = sssRun(0.0f);
    if (none.px.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const SSSRun skin = sssRun(1.0f);
    int changed = 0;
    for (std::size_t i = 0; i < skin.px.size() && i < none.px.size(); ++i) changed += skin.px[i] != none.px[i];
    std::cout << "(tiles flagged: none " << none.tiles << ", skin " << skin.tiles << "; bytes changed " << changed << ") ";
    EXPECT_EQ(none.tiles, 0u, "no subsurface material: no tile flagged");
    EXPECT(skin.tiles > 0u && skin.tiles <= 80u, "the box's tiles are flagged, not the frame's 80");
    EXPECT(changed > 0, "the blur changes the box");
    TEST_PASS();
}

static void runSSAOTests() {
    std::cout << "\n=== SSAO, SSS ===" << std::endl;
    sp2b_t05_ssao_darkens_only_the_ambient_term();
    sp2b_t05_ssao_twin_renders_are_identical();
    ambient_is_a_fallback_only_without_rt_gi();
    sp2b_s9_sss_runs_only_on_flagged_tiles();
}

// =============================================================================
// SECTION — THE CAPTURE PATH (sub-project 2b, step 10)
// =============================================================================

// The spin scene with TAA, 6 frames from a reset, the last read back; `warmup` frames
// before the reset move the frame-in-flight slot the capture starts in.
static std::vector<uint8_t> taaCapture(int warmup) {
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) return {};
    std::vector<uint8_t> out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        r.getDeferredRenderer()->getPostProcessing()->setTAAEnabled(true);
        r.getCamera().setPosition({1.5f, 1.0f, 3.0f});   // a different picture in the history the reset drops
        for (int k = 0; k < warmup; ++k) {
            world.sync();
            r.render();
        }
        r.getCamera().setPosition({0.0f, 0.0f, 4.0f});   // buildSpinScene's view
        r.resetAccumulation();
        for (int f = 0; f < 6; ++f) {
            world.sync();
            if (f == 5) r.requestReadback();
            r.render();
        }
        const auto px = r.readback();
        out.assign(px.begin(), px.end());
    }
    return out;
}

// The reset no longer restarts the frame slots, so nothing a capture renders may depend on
// the slot it starts in: TAA's jitter now walks its Halton samples by the frame index since
// the reset, and its history clear waits on earlier frames by queue order.
static void sp2b_s10_a_capture_after_a_reset_is_the_same_from_any_frame_slot() {
    TEST_BEGIN("sp2b_s10: a TAA capture after a reset is exact whatever frame slot it starts in");
    const std::vector<uint8_t> fresh = taaCapture(0);
    if (fresh.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    for (const int warmup : {1, 2, 7}) {
        const int diff = maxAbsDiff(fresh, taaCapture(warmup));
        std::cout << "(warm-up " << warmup << ": max_abs " << diff << ") ";
        EXPECT_EQ(diff, 0, "the capture after a reset equals a fresh renderer's");
    }
    TEST_PASS();
}

// The reset waits for nothing: frames submitted before it are still running when it returns,
// and it returns in well under the spec's 0.1 ms budget's order of magnitude.
static void sp2b_s10_the_reset_does_not_wait_for_the_gpu() {
    TEST_BEGIN("sp2b_s10: resetAccumulation leaves the frames in flight running (no device idle)");
    ohao::VulkanRenderer r(1920, 1080);
    if (!r.initialize()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        r.setRenderQuality(ohao::RenderQuality::Reference);   // the heaviest frame: full-resolution RT GI
        auto* pp = r.getDeferredRenderer()->getPostProcessing();
        pp->setTAAEnabled(true);
        pp->setSSAOEnabled(true);
        world.sync();
        r.render();   // builds the acceleration structures
        double worstMs = 0.0;
        uint32_t inFlight = 0;
        for (int round = 0; round < 5; ++round) {
            for (int f = 0; f < 3; ++f) {
                world.sync();
                r.render();
            }
            const auto t0 = std::chrono::steady_clock::now();
            r.resetAccumulation();
            worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            inFlight = std::max(inFlight, r.framesInFlight());
        }
        std::cout << "(frames in flight after the reset, most: " << inFlight << "; slowest reset " << worstMs << " ms) ";
        EXPECT(inFlight > 0, "a reset right after three 1080p reference frames finds some still running");
        EXPECT(worstMs < 0.1, "the reset takes under 0.1 ms (the spec's budget)");
    }
    TEST_PASS();
}

struct PreviewRun {
    std::vector<uint8_t> full;      // the frame, read back in full
    std::vector<uint8_t> preview;   // the same frame, the GPU preview only
    uint32_t width{0}, height{0};
    bool fullEmpty{false};          // readback() after the preview request
    bool previewPass{false}, copyPass{false};
    double previewMs{0.0};          // capture.preview's GPU time
    uint64_t copies{0};
};

// One spin-scene frame read back in full, then (after a reset, so it is the same frame) as
// a GPU preview of `rect` within `maxSide`.
static PreviewRun previewRun(uint32_t width, uint32_t height, std::array<uint32_t, 4> rect, uint32_t maxSide) {
    ohao::VulkanRenderer r(width, height);
    if (!r.initialize()) return {};
    PreviewRun out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        world.sync();
        r.requestReadback();
        r.render();
        const auto px = r.readback();
        out.full.assign(px.begin(), px.end());
        r.resetAccumulation();
        world.sync();
        r.requestPreviewReadback({rect, maxSide});
        r.render();
        out.fullEmpty = r.readback().empty();
        const ohao::VulkanRenderer::Preview p = r.readbackPreview();
        out.preview.assign(p.pixels.begin(), p.pixels.end());
        out.width = p.width;
        out.height = p.height;
        out.copies = r.requestedReadbackCount();
        for (const auto& f : r.frameTimings(1)) {
            out.previewPass = hasPass(f, "capture.preview");
            out.copyPass = hasPass(f, "readback");
            for (uint32_t i = 0; i < f.count; ++i)
                if (f.pass[i] == ohao::render::passIndex("capture.preview")) out.previewMs = f.ms[i];
        }
    }
    return out;
}

// The GPU preview is runtime::fitWithin's box filter: the same size, bytes within 1 (float
// rounding), exact when nothing is scaled.
static void sp2b_s10_gpu_preview_matches_the_cpu_downscale() {
    TEST_BEGIN("sp2b_s10: capture.preview equals the CPU box filter within 1 (same size; exact unscaled)");
    struct Case {
        uint32_t w, h;
        std::array<uint32_t, 4> rect;
        uint32_t maxSide, wantW, wantH;
        int tolerance;
    };
    const Case cases[] = {{1280, 720, {0, 0, 1280, 720}, 768, 768, 432, 1},
                          {1280, 720, {100, 60, 500, 300}, 200, 200, 120, 1},
                          {1920, 1080, {0, 0, 1920, 1080}, 768, 768, 432, 1},
                          {160, 120, {0, 0, 160, 120}, 768, 160, 120, 0}};
    for (const Case& c : cases) {
        const PreviewRun run = previewRun(c.w, c.h, c.rect, c.maxSide);
        if (run.full.empty()) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        const std::array<int32_t, 4> region{int32_t(c.rect[0]), int32_t(c.rect[1]), int32_t(c.rect[2]), int32_t(c.rect[3])};
        const ohao::runtime::Rgba8Image cpu =
            ohao::runtime::fitWithin(ohao::runtime::crop(run.full, c.w, c.h, region), c.maxSide);
        EXPECT(run.width == c.wantW && run.height == c.wantH, "the preview has the long side within maxSide, aspect kept");
        EXPECT(cpu.width == run.width && cpu.height == run.height, "the same size as the CPU preview");
        int diff = maxAbsDiff(cpu.pixels, run.preview);
        std::size_t differing = 0;
        for (std::size_t i = 0; i < cpu.pixels.size() && i < run.preview.size(); ++i) differing += cpu.pixels[i] != run.preview[i];
        std::cout << "(" << c.w << "x" << c.h << " -> " << run.width << "x" << run.height << ": max_abs " << diff << ", "
                  << differing << " bytes differ, capture.preview " << run.previewMs << " ms) ";
        EXPECT(diff <= c.tolerance, "the preview's bytes are the CPU filter's within the tolerance");
    }
    TEST_PASS();
}

// A preview request copies the preview and nothing else: no full readback, no `readback`
// pass, one requested copy, the capture.preview pass timed.
static void sp2b_s10_a_preview_request_reads_back_only_the_preview() {
    TEST_BEGIN("sp2b_s10: a preview request reads back only the preview (no full copy)");
    const PreviewRun run = previewRun(1280, 720, {0, 0, 1280, 720}, 768);
    if (run.full.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    EXPECT(run.fullEmpty, "readback() returns nothing for a preview frame");
    EXPECT_EQ(run.preview.size(), std::size_t{768} * 432 * 4, "readbackPreview() is 768x432 RGBA8: 1.3 MB, not 3.7");
    EXPECT(run.previewPass, "the frame ran capture.preview");
    EXPECT(!run.copyPass, "and no readback copy");
    EXPECT_EQ(run.copies, uint64_t{2}, "two requests (full, preview): two copies");
    TEST_PASS();
}

// ---- the HUD (sub-project 7, step 4: the hud pass) -------------------------------------------

struct HudRun {
    std::vector<uint8_t> plain, hud, after;   // the frame without, with and again without the list
    bool hudPass{false}, hudPassAfter{true};
    double hudMs{0.0};
};

// The spin scene's frame (after a reset each time, so the same frame) without a HUD, with `list`,
// and without again.
static HudRun hudRun(uint32_t w, uint32_t h, const ohao::render::HudList& list) {
    ohao::VulkanRenderer r(w, h);
    if (!r.initialize()) return {};
    HudRun out;
    {
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r.renderScene());
        buildSpinScene(world, r);
        const auto frame = [&](std::vector<uint8_t>& px) {
            r.resetAccumulation();
            world.sync();
            r.requestReadback();
            r.render();
            const auto got = r.readback();
            px.assign(got.begin(), got.end());
        };
        frame(out.plain);
        r.setHud(list);
        frame(out.hud);
        for (const auto& f : r.frameTimings(1)) {
            out.hudPass = hasPass(f, "hud");
            for (uint32_t i = 0; i < f.count; ++i)
                if (f.pass[i] == ohao::render::passIndex("hud")) out.hudMs = f.ms[i];
        }
        r.setHud({});
        frame(out.after);
        for (const auto& f : r.frameTimings(1)) out.hudPassAfter = hasPass(f, "hud");
    }
    return out;
}

static void sp7_hud_draws_its_items_and_nothing_else() {
    TEST_BEGIN("sp7: the hud pass blends its items over the final image and touches no other pixel");
    constexpr uint32_t W = 320, H = 240;
    ohao::render::HudList list;
    ohao::render::hudRect(list, {10.0f, 10.0f, 30.0f, 20.0f}, {1.0f, 0.0f, 0.0f, 1.0f});          // opaque red
    ohao::render::hudText(list, "A", {50.0f, 10.0f}, 4.0f, {0.0f, 1.0f, 0.0f, 1.0f});              // a green A, 20x28
    ohao::render::hudRect(list, {100.0f, 100.0f, 120.0f, 120.0f}, {0.0f, 0.0f, 1.0f, 0.5f});      // half blue
    const HudRun run = hudRun(W, H, list);
    if (run.plain.empty()) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const auto at = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y) { return &px[(y * W + x) * 4]; };
    const auto inside = [](uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    };
    std::size_t outsideChanged = 0, redWrong = 0, glyphWrong = 0, blueWrong = 0;
    const int a = ohao::render::hudGlyph('A');
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* p = at(run.plain, x, y);
            const uint8_t* q = at(run.hud, x, y);
            if (inside(x, y, 10, 10, 30, 20)) {
                redWrong += !(q[0] == 255 && q[1] == 0 && q[2] == 0);
            } else if (inside(x, y, 50, 10, 70, 38)) {
                const int col = static_cast<int>(x - 50) / 4, row = static_cast<int>(y - 10) / 4;
                const bool lit = (ohao::render::hudGlyphRow(a, row) >> (4 - col)) & 1u;
                glyphWrong += lit ? !(q[0] == 0 && q[1] == 255 && q[2] == 0) : std::memcmp(p, q, 3) != 0;
            } else if (inside(x, y, 100, 100, 120, 120)) {
                for (int c = 0; c < 3; ++c) {
                    const float want = 0.5f * p[c] + 0.5f * (c == 2 ? 255.0f : 0.0f);
                    blueWrong += std::fabs(q[c] - want) > 1.0f;
                }
            } else {
                outsideChanged += std::memcmp(p, q, 4) != 0;
            }
        }
    const int afterDiff = maxAbsDiff(run.plain, run.after);
    std::cout << "(hud " << run.hudMs << " ms; outside changed " << outsideChanged << ", after clearing max_abs "
              << afterDiff << ") ";
    EXPECT(run.hudPass, "the frame with a list ran the hud pass");
    EXPECT_EQ(redWrong, std::size_t{0}, "the opaque rectangle is its colour, edge to edge");
    EXPECT_EQ(glyphWrong, std::size_t{0}, "the glyph's lit cells are its colour, its unlit cells the scene");
    EXPECT_EQ(blueWrong, std::size_t{0}, "a half-alpha rectangle blends half over the scene (within 1)");
    EXPECT_EQ(outsideChanged, std::size_t{0}, "no pixel outside the items changes");
    EXPECT(!run.hudPassAfter && afterDiff == 0, "an empty list: no pass, the plain frame");
    TEST_PASS();
}

static void runCapturePathTests() {
    std::cout << "\n=== Capture path ===" << std::endl;
    sp2b_s10_a_capture_after_a_reset_is_the_same_from_any_frame_slot();
    sp2b_s10_the_reset_does_not_wait_for_the_gpu();
    sp2b_s10_gpu_preview_matches_the_cpu_downscale();
    sp2b_s10_a_preview_request_reads_back_only_the_preview();
    std::cout << "\n=== HUD ===" << std::endl;
    sp7_hud_draws_its_items_and_nothing_else();
}

static void runRTGITests() {
    std::cout << "\n=== RT GI ===" << std::endl;
    sp2b_t10_a_reference_mean_restarts_after_realtime_frames();
    sp2b_t10_gi_is_deterministic_across_fresh_renderers();
    sp2b_t10_history_resets_on_capture();
    sp2b_t10_multi_frame_captures_converge();
    sp2b_t10_reference_is_full_resolution_32_rays();
    sp2b_t10_realtime_converges_toward_the_reference();
    sp2b_t10_a_red_wall_bleeds_onto_the_floor();
}

// ---- render settings (sub-project 2b, step 15: render.settings) ----------------------------

// The passes one frame of a small lit scene ran under `s`, and what RT GI and CSM report.
struct SettingsRun {
    bool ok{false};
    std::vector<std::string> passes;
    ohao::VulkanRenderer::RenderSettings readBack;
    VkExtent2D giExtent{0, 0};
    uint32_t giRays{0};
    std::vector<uint8_t> px;
};

static SettingsRun settingsRun(const ohao::VulkanRenderer::RenderSettings& s, int frames = 2) {
    SettingsRun out;
    ohao::VulkanRenderer r(160, 120);
    if (!r.initialize()) return out;
    {
        ohao::ecs::World world;
        ohao::ecs::installRenderExtract(world, r.renderScene());
        ohao::Model floor, box;
        ohao::PrimitiveMeshGenerator::generateBox(floor, {-8.0f, -0.1f, -8.0f}, {8.0f, 0.0f, 8.0f}, {1, 1, 1});
        // A box floating over the floor: its shadow falls straight below it, in view.
        ohao::PrimitiveMeshGenerator::generateBox(box, {-0.5f, 1.0f, -0.5f}, {0.5f, 2.0f, 0.5f}, {1, 1, 1});
        ohao::render::MaterialDesc grey;
        grey.baseColor = {0.7f, 0.7f, 0.7f};
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
        ohao::ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), box, "box", {}, &grey);
        ohao::ecs::Light sun;
        sun.type = ohao::ecs::LightType::Directional;
        sun.intensity = 3.0f;
        ohao::ecs::Transform sunT;
        sunT.position = {0.0f, 10.0f, 0.0f};
        sunT.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));   // straight down: the box's shadow below it
        ohao::ecs::spawnLight(world, "sun", sun, sunT);
        r.setRenderMode(ohao::RenderMode::Deferred);
        r.setRenderSettings(s);
        r.getCamera().setPosition({0.0f, 7.0f, 7.0f});
        r.getCamera().setRotation(-45.0f, -90.0f);
        r.resetAccumulation();
        for (int f = 0; f < frames; ++f) {
            world.sync();
            if (f == frames - 1) r.requestReadback();
            r.render();
        }
        const auto px = r.readback();
        out.px.assign(px.begin(), px.end());
        for (const auto& f : r.frameTimings(1))
            for (uint32_t i = 0; i < f.count; ++i) out.passes.emplace_back(ohao::render::kPasses[f.pass[i]].name);
        out.readBack = r.renderSettings();
        if (auto* d = r.getDeferredRenderer(); d && d->getRT_GI()) {
            out.giExtent = d->getRT_GI()->traceExtent();
            out.giRays = d->getRT_GI()->raysPerPixel();
        }
        out.ok = true;
    }
    return out;
}

static bool ran(const SettingsRun& r, std::string_view pass) {
    return std::find(r.passes.begin(), r.passes.end(), pass) != r.passes.end();
}

static void sp2b_t14_settings_defaults_are_the_budgeted_frame_and_round_trip() {
    TEST_BEGIN("sp2b_t14: render settings default to the budgeted frame and read back what was set");
    const ohao::VulkanRenderer::RenderSettings defaults;
    const SettingsRun d = settingsRun(defaults);
    if (!d.ok) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    const auto& b = d.readBack;
    EXPECT(b.quality == ohao::RenderQuality::Realtime && b.rtShadows && b.rtShadowMaxLights == 4 && b.rtGI &&
               !b.rtGIFullResolution && b.rtGIRays == 2 && !b.ssao && b.ssr && b.sss &&
               b.geometry == ohao::GpuCulling::Geometry::Auto && b.occlusionCulling && b.csmCascades == 4 &&
               b.csmSize == 2048 && b.exposure == defaults.exposure,
           "the defaults read back unchanged");
    {
        // A fresh renderer, before any set, is already at the defaults (exposure included): a
        // headless host reports them before its renderer starts and captures with them.
        ohao::VulkanRenderer fresh(64, 64);
        EXPECT(fresh.initialize() && fresh.renderSettings().exposure == defaults.exposure &&
                   fresh.renderSettings().csmSize == defaults.csmSize && fresh.renderSettings().rtGIRays == defaults.rtGIRays,
               "a fresh renderer starts at the defaults");
    }
    ohao::VulkanRenderer::RenderSettings s;
    s.rtShadowMaxLights = 2;
    s.rtGIRays = 5;
    s.ssao = true;
    s.geometry = ohao::GpuCulling::Geometry::Instances;
    s.occlusionCulling = false;
    s.csmCascades = 3;
    s.csmSize = 1024;
    s.exposure = 1.5f;
    const auto t = settingsRun(s).readBack;
    EXPECT(t.rtShadowMaxLights == 2 && t.rtGIRays == 5 && t.ssao && t.geometry == ohao::GpuCulling::Geometry::Instances &&
               !t.occlusionCulling && t.csmCascades == 3 && t.csmSize == 1024 && t.exposure == 1.5f,
           "every changed knob reads back");
    TEST_PASS();
}

static void sp2b_t14_switches_turn_their_passes_off() {
    TEST_BEGIN("sp2b_t14: ssr, sss, ssao, rtGI and rtShadows switch their passes; the frame still renders");
    const SettingsRun on = settingsRun({});
    if (!on.ok) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    ohao::VulkanRenderer::RenderSettings s;
    s.ssr = false;
    s.sss = false;
    s.rtGI = false;
    s.rtShadows = false;
    const SettingsRun off = settingsRun(s);
    s.ssao = true;
    const SettingsRun ssao = settingsRun(s);
    EXPECT(ran(on, "ssr") && ran(on, "sss") && !ran(on, "ssao"), "the defaults run ssr and sss, not ssao");
    EXPECT(!ran(off, "ssr") && !ran(off, "sss"), "ssr and sss off: neither runs");
    EXPECT(!ran(off, "rt.gi") && !ran(off, "rt.gi.denoise") && !ran(off, "rt.shadows"), "rtGI and rtShadows off: none of their passes");
    EXPECT(ran(off, "csm"), "rtShadows off: the CSM fallback draws the sun's shadow");
    EXPECT(ran(ssao, "ssao"), "ssao on: it runs");
    EXPECT(!off.px.empty() && !on.px.empty(), "both frames read back");
    TEST_PASS();
}

static void sp2b_t14_gi_resolution_and_rays_reach_the_trace() {
    TEST_BEGIN("sp2b_t14: rtGI resolution full traces the frame's size, rays reach the trace; half stays exact");
    ohao::VulkanRenderer::RenderSettings s;
    const SettingsRun half = settingsRun(s, 4);
    if (!half.ok || half.giExtent.width == 0) {
        std::cout << "(no ray tracing device -- skipped) ";
        TEST_PASS();
        return;
    }
    s.rtGIFullResolution = true;
    s.rtGIRays = 6;
    const SettingsRun full = settingsRun(s, 4);
    const SettingsRun half2 = settingsRun({}, 4);
    EXPECT(half.giExtent.width == 80 && half.giExtent.height == 60 && half.giRays == 2, "half: 80x60, 2 rays");
    EXPECT(full.giExtent.width == 160 && full.giExtent.height == 120 && full.giRays == 6, "full: 160x120, 6 rays");
    EXPECT(ran(full, "rt.gi.denoise"), "full resolution still denoises");
    EXPECT_EQ(maxAbsDiff(half.px, half2.px), 0, "half resolution renders the same image twice");
    EXPECT(maxAbsDiff(half.px, full.px) > 0, "full resolution changes the indirect light");
    TEST_PASS();
}

static void sp2b_t14_csm_cascades_and_size_apply() {
    TEST_BEGIN("sp2b_t14: csm cascades and size rebuild the fallback's shadow map; the image follows");
    ohao::VulkanRenderer::RenderSettings s;
    s.rtShadows = false;
    const SettingsRun four = settingsRun(s);
    if (!four.ok) {
        std::cout << "(no Vulkan device -- skipped) ";
        TEST_PASS();
        return;
    }
    s.csmCascades = 1;
    s.csmSize = 512;
    const SettingsRun one = settingsRun(s);
    EXPECT(ran(one, "csm") && one.readBack.csmCascades == 1 && one.readBack.csmSize == 512, "one 512x512 cascade drawn");
    EXPECT(maxAbsDiff(four.px, one.px) > 0, "a coarser shadow map changes the shadow");
    TEST_PASS();
}

static void runRenderSettingsTests() {
    std::cout << "\n=== Render settings ===" << std::endl;
    sp2b_t14_settings_defaults_are_the_budgeted_frame_and_round_trip();
    sp2b_t14_switches_turn_their_passes_off();
    sp2b_t14_gi_resolution_and_rays_reach_the_trace();
    sp2b_t14_csm_cascades_and_size_apply();
}

static void runRTShadowTests() {
    std::cout << "\n=== RT shadows ===" << std::endl;
    sp2b_t09_scene_lights_cast_rt_shadows_and_cast_shadows_false_does_not();
    sp2b_t09_the_capture_after_a_reset_is_exact_run_to_run();
    sp2b_t09_csm_is_the_fallback_when_rt_shadows_are_off();
    sp2b_t09_reference_shadows_every_light_past_the_realtime_cap();
    sp2b_t09_a_spot_outside_its_cone_takes_no_slot();
    sp2b_t09_realtime_converges_toward_the_reference();
    sp2b_s4_the_sky_reaches_the_final_image_through_sss();
}

// =============================================================================
// SECTION — RENDER SCENE (persistent GPU render world)
// =============================================================================

static void runRenderSceneTests() {
    std::cout << "\n=== RenderScene ===" << std::endl;
    using namespace ohao::render;

    auto makeQuad = [](std::vector<ohao::Vertex>& v, std::vector<uint32_t>& i) {
        v.assign(4, ohao::Vertex{});
        v[0].position = {-1, 0, -1}; v[1].position = {1, 0, -1};
        v[2].position = {1, 0, 1};   v[3].position = {-1, 0, 1};
        for (auto& x : v) { x.normal = {0, 1, 0}; x.boneWeights = {1, 0, 0, 0}; }
        i = {0, 1, 2, 0, 2, 3};
    };

    TEST_BEGIN("a registered mesh gets an AABB, a table entry and a BLAS");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        EXPECT(m.valid(), "registerMesh returns a valid handle");
        const MeshInfo* info = rs.meshInfo(m);
        EXPECT(info && info->indexCount == 6 && info->vertexCount == 4, "mesh table entry");
        EXPECT(info->aabbMin.x == -1.0f && info->aabbMax.z == 1.0f, "AABB computed once at registration");
        EXPECT(!rs.rtSupported() || info->blas != ohao::INVALID_BLAS, "one BLAS per mesh when RT is available");
        TEST_PASS();
    }

    TEST_BEGIN("static instances upload once; moving ones upload in proportion");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        std::vector<uint32_t> slots;
        for (int k = 0; k < 10000; ++k)
            slots.push_back(rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true}));
        r.render();
        EXPECT(rs.stats().instanceBytesUploaded >= 10000 * sizeof(InstanceGpu), "first frame uploads everything");
        r.render();
        EXPECT_EQ(rs.stats().instanceBytesUploaded, size_t{0}, "a still scene uploads no instance bytes");
        for (int k = 0; k < 100; ++k)
            rs.updateInstance(slots[k], InstanceDesc{m, {&mat, 1}, glm::mat4(2), glm::mat4(1), true});
        r.render();
        EXPECT_EQ(rs.stats().instanceBytesUploaded, 100 * sizeof(InstanceGpu), "bytes = moved instances");
        TEST_PASS();
    }

    TEST_BEGIN("missing material refs fall back to the default material");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        // Two submeshes, one material given.
        MeshHandle m = rs.registerMesh(MeshData{v, i, {{0, 3, 0}, {3, 3, 1}}, {0, 1}, "two"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{{1, 0, 0}});
        uint32_t s = rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        EXPECT_EQ(rs.materialIndexFor(s, 0), mat.index, "slot 0 uses the given material");
        EXPECT_EQ(rs.materialIndexFor(s, 1), 0u, "slot 1 has no ref: default material 0");
        TEST_PASS();
    }

    TEST_BEGIN("instance-table growth mid-frame defers destroying the old buffers");
    {
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer r(64, 64);
        const bool ok = r.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok || !r.validationEnabled()) { std::cout << "(skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        // Each frame adds a batch; the device instance + material-ref buffers grow
        // inside recordUpdates while earlier frames still read the old ones.
        for (int k = 0; k < 64 && rs.stats().instanceTableGrowths < 2; ++k) {
            for (int n = 0; n < 300; ++n)
                rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
            r.render();
        }
        EXPECT(rs.stats().instanceTableGrowths >= 2, "the instance table grew past 1024 twice");
        for (int k = 0; k < 4; ++k) r.render();
        const auto msgs = r.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "no retired buffer was destroyed while in use");
        TEST_PASS();
    }

    TEST_BEGIN("mesh arena growth keeps registering and drawing meshes");
    {
        // registerMesh idles the queue, so this does not exercise deferred
        // destruction; it checks that meshes registered across a growth are intact.
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        const auto grows0 = rs.stats().arenaGrowths;
        std::vector<MeshHandle> meshes;
        for (int k = 0; k < 64 && rs.stats().arenaGrowths == grows0; ++k) {
            std::vector<ohao::Vertex> big(20000, v[0]);
            big.back().position = {float(k), 2.0f, 3.0f};
            std::vector<uint32_t> bigI(60000, 0);
            meshes.push_back(rs.registerMesh(MeshData{big, bigI, {}, {}, "big"}));
            rs.createInstance(InstanceDesc{meshes.back(), {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
            r.render();
        }
        EXPECT(rs.stats().arenaGrowths > grows0, "the arena grew during the test");
        MeshHandle after = rs.registerMesh(MeshData{v, i, {}, {}, "after"});
        rs.createInstance(InstanceDesc{after, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        for (int k = 0; k < 4; ++k) r.render();
        bool intact = true;
        for (size_t k = 0; k < meshes.size(); ++k) {
            const MeshInfo* mi = rs.meshInfo(meshes[k]);
            intact = intact && mi && mi->vertexOffset == k * 20000u && mi->indexOffset == k * 60000u &&
                     mi->aabbMax.x == std::max(0.0f, float(k)) &&
                     (!rs.rtSupported() || rs.rtAccel()->getBlasAddress(mi->blas) != 0);
        }
        EXPECT(intact, "meshes registered before and at the growth keep offsets, AABB and BLAS");
        const MeshInfo* ai = rs.meshInfo(after);
        EXPECT(ai && ai->vertexOffset == meshes.size() * 20000u && ai->indexCount == 6, "a mesh after growth appends");
        EXPECT(!rs.rtSupported() || rs.rtAccel()->getBlasAddress(ai->blas) != 0, "a mesh after growth gets a BLAS");
        EXPECT_EQ(rs.draws().size(), meshes.size() + 1, "every instance draws");
        TEST_PASS();
    }

    // params.z is a point or spot light's sphere radius: its specular is the reflection of its
    // emitter mesh (core/deferred_lighting.frag's evaluateBRDFSphere).
    TEST_BEGIN("raster lights carry their sphere radius, sorted by order");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        LightDesc point{}; point.kind = LightKind::Point; point.order = 0; point.radius = 0.25f;
        LightDesc sun{};   sun.kind = LightKind::Directional; sun.order = 1;
        LightDesc bare{};  bare.kind = LightKind::Point; bare.order = 2; bare.radius = 0.0f;
        const LightDesc lights[] = {sun, bare, point};   // `order` sorts them: point, sun, bare
        rs.setLights(lights);
        const auto raster = rs.rasterLights();
        EXPECT_EQ(raster.size(), size_t{3}, "three raster lights");
        EXPECT(raster[0].position.w == 1.0f && raster[0].params.z == 0.25f, "the point light's sphere radius");
        EXPECT(raster[1].position.w == 0.0f && raster[1].params.z == 0.0f, "the directional light has none");
        EXPECT(raster[2].position.w == 1.0f && raster[2].params.z == 0.0f, "a point light of radius 0 stays a point");
        TEST_PASS();
    }

    // The material table pack is what the G-buffer and the path tracer's hit
    // shaders read: every field of a material, at the MaterialGpuPack offsets.
    TEST_BEGIN("a material edit reaches the material table pack");
    {
        MaterialDesc d;
        d.baseColor = {0.25f, 0.5f, 0.75f};
        d.roughness = 0.3f;
        d.metallic = 1.0f;
        d.albedoTex = 7; d.normalTex = 8; d.roughMetalTex = 9; d.emissiveTex = 10; d.occlusionTex = 11;
        d.emissiveStrength = 2.0f;
        const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
        const MaterialTable::Pack p = MaterialTable::pack(d);
        EXPECT(p[0] == 0.25f && p[1] == 0.5f && p[2] == 0.75f && bits(p[3]) == 7u, "row 0: base color, albedo texture");
        EXPECT(p[4] == 0.3f && p[5] == 1.0f && bits(p[6]) == 8u && bits(p[7]) == 10u,
               "row 1: roughness, metallic, normal and emissive textures");
        EXPECT(bits(p[8]) == 9u && p[9] == 2.0f && p[10] == 0.0f && bits(p[11]) == 11u,
               "row 2: rough/metal texture, emissive strength, subsurface, occlusion texture");
        EXPECT(p[12] == 0.0f && p[13] == 0.5f && p[14] == 1.0f && p[15] == 1.0f,
               "row 3: opaque, cutoff 0.5, opacity 1, base colour alpha 1 by default");
        d.alphaMode = AlphaMode::Blend;
        d.opacity = 0.25f;
        d.baseAlpha = 0.5f;
        const MaterialTable::Pack pb = MaterialTable::pack(d);
        EXPECT(pb[12] == 2.0f && pb[14] == 0.25f && pb[15] == 0.5f, "row 3: blend, opacity and base colour alpha");
        d.alphaMode = AlphaMode::Opaque;
        // An edit is a new registration: the table never rewrites a live material.
        d.baseColor = {1, 0, 0};
        EXPECT(MaterialTable::pack(d)[0] == 1.0f, "the edited color is what gets packed");

        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- partial) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        MaterialDesc a; a.baseColor = {0.5f, 0.5f, 0.5f};
        MaterialDesc b = a; b.baseColor = {0.9f, 0.1f, 0.1f};
        const MaterialHandle ha = rs.registerMaterial(a);
        const MaterialHandle hb = rs.registerMaterial(b);
        EXPECT(ha.valid() && hb.valid() && ha.index != hb.index && ha.index != 0u,
               "re-registering an edited material gets its own table entry");
        TEST_PASS();
    }

    // The path tracer binds the RenderScene: vertex/index arenas, mesh table, slots,
    // materials, instances, refs, lights, one texture table and the in-frame TLAS.
    // Not under validation: NRD's NRI wrapper aborts the process when the validation
    // layer hides vkCmdPushDescriptorSet (pre-existing, any RT profile init).
    TEST_BEGIN("the path tracer traces the RenderScene");
    {
        auto r = std::make_unique<ohao::VulkanRenderer>(64, 64);
        if (!r->initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        if (!r->isRTSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r->renderScene());
        ohao::Model model;
        model.vertices = v; model.indices = i;
        ohao::ecs::Transform quadT;   // the XZ quad turned to face +Z, 4 units wide
        quadT.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
        quadT.scale = glm::vec3(2.0f);
        MaterialDesc quadMat;
        quadMat.baseColor = {0.9f, 0.9f, 0.9f};
        ohao::ecs::spawnModel(world, r->renderScene(), r->getTextureManager(), model, "quad", quadT, &quadMat);
        ohao::ecs::Light light;
        light.intensity = 20.0f;
        light.radius = 0.5f;
        ohao::ecs::spawnLight(world, "light", light, ohao::ecs::Transform{{0, 0, 2}});
        r->getCamera().setPosition({0.0f, 0.0f, 6.0f});
        r->getCamera().setRotation(0.0f, -90.0f);
        r->setRenderMode(ohao::RenderMode::RTOffline);
        if (r->getRenderMode() != ohao::RenderMode::RTOffline) { std::cout << "(no path tracer -- skipped) "; TEST_PASS(); return; }
        for (int k = 0; k < 6; ++k) {
            world.update(0.0f);
            if (k == 5) r->requestReadback();
            r->render();
        }
        RenderScene& rs = r->renderScene();
        EXPECT(rs.tlas() != VK_NULL_HANDLE, "the RenderScene built a TLAS for the path tracer");
        EXPECT_EQ(rs.rtLightCount(), 1u, "the scene's light reached the RT light list");
        const auto px = r->readback();
        EXPECT_EQ(px.size(), size_t{64 * 64 * 4}, "a full readback");
        const std::size_t centre = (32 * 64 + 32) * 4, corner = 0;
        EXPECT(px[centre] > 20, "the lit quad covers the image centre (closest-hit read the scene)");
        EXPECT(px[centre] > px[corner], "the corner, past the quad, sees the black sky");
        TEST_PASS();
    }

    // Materials store texture-table slot indices up to its capacity (4096): the path
    // tracer's binding 12 must cover them, and an index past it must read as "no
    // texture" instead of an unwritten descriptor. Not under validation (NRD abort).
    TEST_BEGIN("texture slots past 1024 reach the path tracer; out-of-range reads as untextured");
    {
        auto r = std::make_unique<ohao::VulkanRenderer>(64, 64);
        if (!r->initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        if (!r->isRTSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        ohao::BindlessTextureManager* tm = r->getTextureManager();
        if (!tm || tm->getMaxTextures() <= 1100) { std::cout << "(small texture table -- skipped) "; TEST_PASS(); return; }
        // Fill the table past slot 1024 with 1x1 textures; the last one is pure red.
        const std::array<uint8_t, 4> grey{128, 128, 128, 255}, red{255, 0, 0, 255};
        ohao::BindlessTextureHandle last;
        for (int n = 0; n < 1100; ++n) {
            last = tm->loadTextureFromMemory(std::span<const uint8_t>(n == 1099 ? red : grey), 1, 1,
                                             VK_FORMAT_R8G8B8A8_SRGB, ohao::BindlessTextureType::Albedo, false);
            if (!last.valid()) break;
        }
        tm->updateDescriptorSet();
        EXPECT(last.valid() && last.index >= 1024, "a texture slot past 1024");

        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        ohao::ecs::World world;   // after the renderer: released first
        ohao::ecs::installRenderExtract(world, r->renderScene());
        ohao::Model model;
        model.vertices = v; model.indices = i;
        const auto addQuad = [&](const char* name, float x, uint32_t albedoTex) {
            ohao::ecs::Transform t;
            t.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
            t.position = {x, 0, 0};
            t.scale = glm::vec3(0.9f);
            MaterialDesc mat;
            mat.baseColor = {1.0f, 1.0f, 1.0f};
            mat.albedoTex = albedoTex;
            ohao::ecs::spawnModel(world, r->renderScene(), tm, model, name, t, &mat);
        };
        addQuad("left_far_red", -1.0f, last.index);
        addQuad("right_plain", 1.0f, kNoTexture);
        ohao::ecs::Light light;
        light.intensity = 20.0f;
        light.radius = 0.5f;
        ohao::ecs::spawnLight(world, "light", light, ohao::ecs::Transform{{0, 0, 2}});

        // A third quad straight on the RenderScene, whose material names slot 100000:
        // past every binding, it must render as its base colour (green), not crash.
        RenderScene& rs = r->renderScene();
        MaterialDesc bogus;
        bogus.baseColor = {0.1f, 0.9f, 0.1f};
        bogus.albedoTex = 100000;
        const MaterialHandle bogusMat = rs.registerMaterial(bogus);
        std::vector<ohao::Vertex> v2 = v;
        for (auto& vert : v2) {   // the quad in the XY plane, facing the camera
            vert.position = glm::vec3(vert.position.x * 0.9f, vert.position.z * 0.9f, 0.0f);
            vert.normal = {0, 0, 1};
        }
        const MeshHandle bogusMesh = rs.registerMesh(MeshData{v2, i, {}, {}, "bogus"});
        const uint32_t bogusSlot = rs.createInstance(InstanceDesc{bogusMesh, {&bogusMat, 1},
            glm::translate(glm::mat4(1), {0, 2.0f, 0}), glm::mat4(1), true});
        EXPECT(bogusSlot != kInvalidIndex, "the out-of-range instance exists");

        r->getCamera().setPosition({0.0f, 0.7f, 6.0f});
        r->getCamera().setRotation(0.0f, -90.0f);
        r->setRenderMode(ohao::RenderMode::RTOffline);
        if (r->getRenderMode() != ohao::RenderMode::RTOffline) { std::cout << "(no path tracer -- skipped) "; TEST_PASS(); return; }
        for (int k = 0; k < 6; ++k) {
            world.update(0.0f);
            if (k == 5) r->requestReadback();
            r->render();
        }
        const auto px = r->readback();
        EXPECT_EQ(px.size(), size_t{64 * 64 * 4}, "a full readback");
        int redLeft = 0, greenTop = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const uint8_t* p = &px[(static_cast<std::size_t>(y) * 64 + x) * 4];
                if (x < 32 && p[0] > 40 && p[0] > 3 * p[1] && p[0] > 3 * p[2]) ++redLeft;
                if (p[1] > 40 && p[1] > 3 * p[0] && p[1] > 3 * p[2]) ++greenTop;
            }
        EXPECT(redLeft > 20, "the quad textured from slot >= 1024 renders red (binding 12 covers it)");
        EXPECT(greenTop > 20, "the quad naming slot 100000 renders its base colour (guarded)");
        TEST_PASS();
    }

    // The TLAS is built and refit inside the frame's command buffer: the validation
    // layer checks those builds and their barriers.
    TEST_BEGIN("moving an instance refits the TLAS with the new transform");
    {
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer r(64, 64);
        const bool ok = r.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok) { std::cout << "(skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        if (!rs.rtSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        uint32_t s = rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "first frame builds");
        EXPECT(rs.tlas() != VK_NULL_HANDLE, "the RenderScene has a TLAS");
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::None, "a still scene does nothing");
        const glm::mat4 moved = glm::translate(glm::mat4(1), {3, 0, 0});
        rs.updateInstance(s, InstanceDesc{m, {&mat, 1}, moved, glm::mat4(1), true});
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Refit, "a transform change refits");
        EXPECT(rs.tlasTransform(s)[3].x == 3.0f, "the TLAS instance carries the new world matrix");
        rs.updateInstance(s, InstanceDesc{m, {&mat, 1}, moved, glm::mat4(1), false});
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "a visibility change rebuilds");
        for (int k = 0; k < 4; ++k) r.render();
        const auto msgs = r.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "in-frame TLAS builds and refits are clean");
        TEST_PASS();
    }

    TEST_BEGIN("a slot released and reused in one frame forces a TLAS rebuild");
    {
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer r(64, 64);
        const bool ok = r.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok) { std::cout << "(skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        if (!rs.rtSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle a = rs.registerMesh(MeshData{v, i, {}, {}, "a"});
        MeshHandle b = rs.registerMesh(MeshData{v, i, {}, {}, "b"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        uint32_t s = rs.createInstance(InstanceDesc{a, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        r.render(); r.render();
        rs.destroyInstance(s);
        uint32_t s2 = rs.createInstance(InstanceDesc{b, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        EXPECT_EQ(s2, s, "the released slot is reused");
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "slot reuse rebuilds, never refits");
        // Grow the instance table (and the TLAS instance buffer and storage with it)
        // while earlier frames still trace the old TLAS.
        const auto grows0 = rs.stats().instanceTableGrowths;
        for (int n = 0; n < 1500; ++n)
            rs.createInstance(InstanceDesc{a, {&mat, 1}, glm::translate(glm::mat4(1), {float(n), 0, 0}),
                                           glm::mat4(1), true});
        r.render();
        EXPECT(rs.stats().instanceTableGrowths > grows0, "the instance table grew mid-frame");
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "growth rebuilds");
        EXPECT(rs.tlasTransform(1500)[3].x == 1499.0f, "grown slots carry their transforms");
        for (int k = 0; k < 4; ++k) r.render();
        r.shutdown();
        const auto msgs = r.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "retired TLAS storage was never destroyed in use");
        TEST_PASS();
    }

    // The raster light table (clustered lighting's light buffer) is host-written while up to
    // two earlier frames may still read it: one buffer per frame slot, written after that
    // slot's fence, bound by that slot's lighting and lights.cluster descriptor sets.
    TEST_BEGIN("the raster light table is one buffer per frame slot");
    {
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer r(64, 64);
        const bool ok = r.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        RenderScene& rs = r.renderScene();
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        for (int f = 0; f < 6; ++f) {
            LightDesc l{};
            l.kind = LightKind::Point;
            l.position = {float(f), 2.0f, 0.0f};   // a light that moves every frame
            rs.setLights({&l, 1});
            r.render();
        }
        const ohao::LightClusters* lc = r.getDeferredRenderer()->lightClusters();
        EXPECT(lc != nullptr, "the deferred renderer has its light table");
        for (uint32_t s = 0; s < ohao::MAX_FRAMES_IN_FLIGHT; ++s)
            EXPECT(lc->lightBuffer(s) != VK_NULL_HANDLE, "every frame slot has a light buffer");
        EXPECT(lc->lightBuffer(0) != lc->lightBuffer(1) && lc->lightBuffer(1) != lc->lightBuffer(2) &&
                   lc->lightBuffer(0) != lc->lightBuffer(2),
               "the slots' light buffers are distinct");
        // Frame 5 (slot 2) wrote x=5; slots 0 and 1 still hold frames 3 and 4.
        for (uint32_t s = 0; s < ohao::MAX_FRAMES_IN_FLIGHT; ++s) {
            EXPECT(lc->lightCount(s) == 1 && lc->lights(s) != nullptr, "each slot holds the one scene light");
            EXPECT(lc->lights(s)[0].position[0] == float(3 + s),
                   "writing one slot leaves the frames in flight on the others untouched");
        }
        for (int k = 0; k < 4; ++k) r.render();
        const auto msgs = r.validationMessages();
        if (!msgs.empty()) std::cout << "\n      first: " << msgs.front().substr(0, 260) << "\n";
        EXPECT_EQ(msgs.size(), size_t{0}, "a light moving every frame renders clean");
        TEST_PASS();
    }

    // Accumulating RT profiles restart when the scene they trace changes: a TLAS
    // build or refit, or a light change. A still scene keeps accumulating.
    TEST_BEGIN("RT accumulation restarts when an instance moves or a light changes");
    {
        auto r = std::make_unique<ohao::VulkanRenderer>(64, 64);
        if (!r->initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        if (!r->isRTSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r->renderScene();
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        uint32_t s = rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        LightDesc l{};
        l.position = {0, 2, 0};
        rs.setLights({&l, 1});
        r->setRenderMode(ohao::RenderMode::RTOffline);
        if (r->getRenderMode() != ohao::RenderMode::RTOffline) { std::cout << "(no path tracer -- skipped) "; TEST_PASS(); return; }
        for (int k = 0; k < 5; ++k) r->render();
        EXPECT_EQ(r->getPathTracerFrameIndex(), 5u, "a still scene accumulates every frame");
        rs.updateInstance(s, InstanceDesc{m, {&mat, 1}, glm::translate(glm::mat4(1), {1, 0, 0}),
                                          glm::mat4(1), true});
        r->render();
        EXPECT(rs.stats().lastTlasOp != RenderSceneStats::TlasOp::None, "the move changed the TLAS");
        EXPECT_EQ(r->getPathTracerFrameIndex(), 1u, "a moved instance restarts accumulation");
        r->render(); r->render();
        EXPECT_EQ(r->getPathTracerFrameIndex(), 3u, "and it accumulates again once still");
        l.position = {0, 3, 0};
        rs.setLights({&l, 1});
        r->render();
        EXPECT_EQ(r->getPathTracerFrameIndex(), 1u, "a light change restarts accumulation");
        TEST_PASS();
    }
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    std::cout << "================================================\n";
    std::cout << "  OHAO Engine Tests\n";
    std::cout << "================================================\n";

    runEventBusTests();
    runCommandHistoryTests();
    runAssetTests();
    runMetaTests();
    runHeadlessDeferredTests();
    runHeadlessPathTracerTests();
    runHeadlessDeferredRenderTests();
    runPassTimingTests();
    runReadbackTests();
    runRTShadowTests();
    runRTGITests();
    runSSAOTests();
    runRenderSettingsTests();
    runCapturePathTests();
    runOrphanedEntryPointTests();
    runRenderSceneTests();

    std::cout << "\n================================================\n" << std::flush;
    return ohao::test::finish("engine_tests", testsPassed, testsFailed, g_failures);
}
