/**
 * engine_tests.cpp - Unit tests for engine subsystems
 *
 * Tests:
 *   1. EventBus  — subscribe, publish, unsubscribe, clear, typed data, multiple subscribers
 *   2. CommandHistory — execute, undo, redo, canUndo/canRedo, descriptions, clear, max history
 *   3. Scene — createActor, findActor, removeActor, getAllActors, actor count
 */

#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <cmath>

#include "core/core.hpp"
#include "gpu/layout_meta.hpp"
#include "gpu/vulkan/vk_utils.hpp"
#include "gpu/vulkan/material_instance.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/camera/camera.hpp"
#include "render/culling.hpp"
#include "physics/backend/physics_backend.hpp"
#include "physics/common/physics_constants.hpp"
#include "physics/world/physics_world.hpp"
#include "render/deferred/post_processing_pipeline.hpp"
#include "render/graph/resource_handle.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/rt/gpu_light.hpp"
#include "scene/scene_module.hpp"

#include <span>
#include <vector>

using namespace ohao;

// =============================================================================
// TEST FRAMEWORK (matches existing suites)
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
        auto id = EventBus::instance().subscribe("actor.selected",
            [&](const Event& e) {
                received = std::any_cast<std::string>(e.data);
            });
        EventBus::instance().publish("actor.selected", std::string("MyActor"));
        EventBus::instance().unsubscribe(id);
        EXPECT(received == "MyActor", "payload should round-trip through std::any");
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
// SECTION 3 — SCENE
// =============================================================================

static void runSceneTests() {
    std::cout << "\n[Scene]\n";

    TEST_BEGIN("createActor adds to scene");
    {
        Scene scene("TestScene");
        auto actor = scene.createActor("Box1");
        EXPECT(actor != nullptr, "createActor should return non-null");
        EXPECT(actor->getName() == "Box1", "actor name should match");
        // +1 for the root "World" actor created in Scene ctor
        EXPECT(scene.getAllActors().size() >= 1u, "scene should have at least the new actor");
        TEST_PASS();
    }

    TEST_BEGIN("findActor by name");
    {
        Scene scene("TestScene");
        scene.createActor("Sphere1");
        auto found = scene.findActor("Sphere1");
        EXPECT(found != nullptr, "findActor should find the created actor");
        EXPECT(found->getName() == "Sphere1", "found actor name should match");
        TEST_PASS();
    }

    TEST_BEGIN("findActor returns null for missing actor");
    {
        Scene scene("TestScene");
        auto found = scene.findActor("DoesNotExist");
        EXPECT(found == nullptr, "findActor should return null for unknown name");
        TEST_PASS();
    }

    TEST_BEGIN("removeActor by name");
    {
        Scene scene("TestScene");
        scene.createActor("TempActor");
        EXPECT(scene.findActor("TempActor") != nullptr, "actor should exist before remove");
        scene.removeActor("TempActor");
        EXPECT(scene.findActor("TempActor") == nullptr, "actor should not exist after removeActor");
        TEST_PASS();
    }

    TEST_BEGIN("createActor multiple actors");
    {
        Scene scene("Multi");
        size_t before = scene.getAllActors().size();
        scene.createActor("A");
        scene.createActor("B");
        scene.createActor("C");
        EXPECT(scene.getAllActors().size() == before + 3, "scene should have 3 more actors");
        TEST_PASS();
    }

    TEST_BEGIN("scene name");
    {
        Scene scene("MyGameScene");
        EXPECT(scene.getName() == "MyGameScene", "scene name should match ctor arg");
        TEST_PASS();
    }

    TEST_BEGIN("findActor by id");
    {
        Scene scene("IdTest");
        auto actor = scene.createActor("IdActor");
        uint64_t id = actor->getID();
        auto found = scene.findActor(id);
        EXPECT(found != nullptr, "findActor(id) should find the actor");
        EXPECT(found->getID() == id, "found actor ID should match");
        TEST_PASS();
    }

    TEST_BEGIN("actor tags + findActorsByTag");
    {
        Scene scene("TagScene");
        auto a = scene.createActor("PropA");
        auto b = scene.createActor("PropB");
        a->addTag("pickup");
        a->addTag("glow");
        b->addTag("pickup");
        EXPECT(a->hasTag("pickup"), "has pickup");
        EXPECT(!b->hasTag("glow"), "B no glow");
        auto pickups = scene.findActorsByTag("pickup");
        EXPECT(pickups.size() == 2u, "two pickups");
        a->removeTag("pickup");
        EXPECT(scene.findActorsByTag("pickup").size() == 1u, "one pickup left");
        TEST_PASS();
    }

    TEST_BEGIN("scene actorCount + contains + forEachActor");
    {
        Scene scene("CountScene");
        const auto before = scene.actorCount();
        scene.createActor("A");
        scene.createActor("B");
        EXPECT(scene.actorCount() == before + 2u, "actorCount");
        EXPECT(scene.contains("A"), "contains A");
        int visits = 0;
        scene.forEachActor([&](Actor&) { ++visits; });
        EXPECT(visits == static_cast<int>(scene.actorCount()), "forEach visits all");
        TEST_PASS();
    }

    TEST_BEGIN("MeshBufferInfo on MeshComponent");
    {
        Scene scene("MeshBuf");
        auto actor = scene.createActor("M");
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setBufferInfo(MeshBufferInfo{
            .vertexOffset = 4,
            .indexOffset = 8,
            .indexCount = 36,
            .vertexCount = 24,
        });
        auto info = mesh->getBufferInfo();
        EXPECT(info.vertexOffset == 4u && info.indexEnd() == 44u, "buffer info");
        EXPECT(mesh->getRenderMode() == MeshComponent::RenderMode::Solid, "default solid");
        TEST_PASS();
    }

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
        EXPECT(off.maxBounces == 4, "offline max bounces");
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
        static_assert(layout::MaterialGpuPack::kBytes == 48);
        static_assert(layout::MaterialGpuPack::byteOffset(1) == 48);
        EXPECT(layout::MaterialGpuPack::vec4Count(2) == 6, "2 materials = 6 vec4s");
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

    TEST_BEGIN("ComponentType concept (MeshComponent)");
    {
        static_assert(ComponentType<MeshComponent>);
        static_assert(!ComponentType<int>);
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

    TEST_BEGIN("MeshBufferInfo GpuPod + helpers");
    {
        static_assert(GpuPod<MeshBufferInfo>);
        static_assert(sizeof(MeshBufferInfo) == 16);
        MeshBufferInfo m{.vertexOffset = 10, .indexOffset = 20, .indexCount = 6, .vertexCount = 4};
        EXPECT(!m.empty(), "non-empty mesh");
        EXPECT(m.indexEnd() == 26, "index end");
        EXPECT(m.vertexEnd() == 14, "vertex end");
        EXPECT(kEmptyMeshBuffer.empty(), "empty sentinel");
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

    TEST_BEGIN("to_underlying(LogLevel)");
    {
        EXPECT(to_underlying(LogLevel::Error) == 2, "LogLevel underlying");
        EXPECT(logLevelName(LogLevel::Warning) == "WARNING", "log level name");
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

    TEST_BEGIN("Render graph handles + culling AABB + camera");
    {
        TextureHandle th{};
        EXPECT(!th, "invalid texture handle");
        th.index = 3;
        EXPECT(th && th.isValid(), "valid handle");
        auto usage = TextureUsage::ColorAttachment | TextureUsage::ShaderRead;
        EXPECT(hasFlag(usage, TextureUsage::ShaderRead), "usage flag");
        AABB box{glm::vec3(-1.f), glm::vec3(1.f)};
        EXPECT(box.isValid(), "AABB valid");
        EXPECT(glm::length(box.center()) < 1e-5f, "AABB center origin");
        EXPECT(box.size() == glm::vec3(2.f), "AABB size");
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
    runSceneTests();
    runMetaTests();

    std::cout << "\n================================================\n";
    std::cout << "  Results: " << testsPassed << "/" << testsRun << " passed";
    if (testsFailed > 0)
        std::cout << "  \033[31m(" << testsFailed << " failed)\033[0m";
    std::cout << "\n================================================\n";

    return testsFailed > 0 ? 1 : 0;
}
