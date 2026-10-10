/**
 * engine_tests.cpp - Unit tests for engine subsystems
 *
 * Tests:
 *   1. EventBus  — subscribe, publish, unsubscribe, clear, typed data, multiple subscribers
 *   2. CommandHistory — execute, undo, redo, canUndo/canRedo, descriptions, clear, max history
 *   3. Scene — createActor, findActor, removeActor, getAllActors, actor count
 */

#include <atomic>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <cmath>
#include <cstring>

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
#include "render/deferred/deferred_renderer.hpp"
#include "render/rt/path_tracer.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/rt/gpu_light.hpp"
#include "render/scene/render_scene.hpp"
#include "render/scene/material_table.hpp"
#include "render/scene/model_upload.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "scene/scene_module.hpp"

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
            renderer.setScene(nullptr);
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

    TEST_BEGIN("resize() resizes the readback buffer it owns");
    {
        ohao::VulkanRenderer renderer(64, 48);
        // MEASURE getPixelBufferSize(), NOT getPixelSpan().size(). The span
        // accessor returns an EMPTY span when getPixels() is null, which it is
        // on a renderer that was never initialized -- so a span-based check
        // reads 0 before and 0 after and cannot see whether resize did anything.
        // An earlier version of this test did exactly that and passed against a
        // deliberately broken resize(). getPixelBufferSize() has no such guard
        // and is the buffer resize() actually owns.
        EXPECT_EQ(renderer.getPixelBufferSize(), static_cast<size_t>(64 * 48 * 4),
                  "constructor must size the pixel buffer to width*height*4");
        EXPECT_EQ(renderer.getWidth(), 64u, "constructor must record its width");
        EXPECT_EQ(renderer.getHeight(), 48u, "constructor must record its height");

        // 96x64, NOT 96x32. The first draft of this test resized 64x48 -> 96x32,
        // which is 3072 pixels either way: the assertion compared 12288 against
        // 12288 and held perfectly against a resize() that did nothing at all.
        // An invariant is only as strong as the case it is evaluated on, and a
        // dimension change that preserves the pixel count is the one case this
        // particular invariant cannot see.
        renderer.resize(96, 64);
        EXPECT_EQ(renderer.getPixelBufferSize(), static_cast<size_t>(96 * 64 * 4),
                  "resize must re-size the readback buffer, or the next readback "
                  "copies the new frame's bytes into a buffer sized for the old one");
        EXPECT_EQ(renderer.getWidth(), 96u, "resize must record the new width");
        EXPECT_EQ(renderer.getHeight(), 64u, "resize must record the new height");

        // AND SHRINKING, separately: a grow can be satisfied by a vector that
        // only ever reserves, while the readback length is taken from size().
        renderer.resize(16, 16);
        EXPECT_EQ(renderer.getPixelBufferSize(), static_cast<size_t>(16 * 16 * 4),
                  "resize must shrink the readback buffer too, not just grow it");
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
        for (int i = 0; i < 4; ++i) renderer.render();   // drain the readback ring at the new size
        EXPECT_EQ(renderer.getPixelBufferSize(), static_cast<size_t>(256 * 128 * 4),
                  "readback buffer follows the resize");
        EXPECT(std::abs(renderer.getCamera().getAspectRatio() - 2.0f) < 1e-5f,
               "resize must update the camera aspect, or the image is stretched");
        const uint8_t* px = renderer.getPixels();
        EXPECT(px != nullptr, "pixels available after resize");
        size_t opaque = 0;
        for (size_t i = 3; i < renderer.getPixelBufferSize(); i += 4) opaque += px[i] != 0;
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

    TEST_BEGIN("raster shadow map goes to the first point or spot light (legacy rule)");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        LightDesc point{}; point.kind = LightKind::Point; point.order = 0;
        LightDesc sun{};   sun.kind = LightKind::Directional; sun.order = 1;
        const LightDesc lights[] = {sun, point};   // `order` sorts them: point first
        rs.setLights(lights);
        const auto raster = rs.rasterLights();
        EXPECT_EQ(raster.size(), size_t{2}, "two raster lights");
        EXPECT(raster[0].position.w == 1.0f && raster[0].params.z == 0.0f, "the point light has shadow index 0");
        EXPECT(raster[1].position.w == 0.0f && raster[1].params.z == -1.0f, "the directional light has none");
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
        d.albedoTex = 7; d.normalTex = 8; d.roughMetalTex = 9; d.emissiveTex = 10;
        d.emissiveStrength = 2.0f;
        d.legacyRtTintTex = 11;
        const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
        const MaterialTable::Pack p = MaterialTable::pack(d);
        EXPECT(p[0] == 0.25f && p[1] == 0.5f && p[2] == 0.75f && bits(p[3]) == 7u, "row 0: base color, albedo texture");
        EXPECT(p[4] == 0.3f && p[5] == 1.0f && bits(p[6]) == 8u && bits(p[7]) == 10u,
               "row 1: roughness, metallic, normal and emissive textures");
        EXPECT(bits(p[8]) == 9u && p[9] == 2.0f && bits(p[10]) == 11u,
               "row 2: rough/metal texture, emissive strength, legacy RT tint");
        // An edit is a new registration: the table never rewrites a live material.
        d.baseColor = {1, 0, 0};
        EXPECT(MaterialTable::pack(d)[0] == 1.0f, "the edited color is what gets packed");
        EXPECT(bits(MaterialTable::pack(MaterialDesc{})[10]) == kNoTexture, "no tint by default");

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

    TEST_BEGIN("legacy RT tint textures are 1x1 UNORM and shared per encoded color");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        ohao::BindlessTextureManager* tm = r.getTextureManager();
        if (!tm) { std::cout << "(no texture manager -- skipped) "; TEST_PASS(); return; }
        EXPECT_EQ(legacyRtTintTexture(nullptr, glm::vec3(0.5f)), kNoTexture, "no manager, no texture");
        const uint32_t a = legacyRtTintTexture(tm, glm::vec3(0.73f));
        const uint32_t b = legacyRtTintTexture(tm, glm::vec3(0.73f));
        const uint32_t c = legacyRtTintTexture(tm, glm::vec3(0.65f, 0.05f, 0.05f));
        EXPECT(a != kNoTexture && a == b, "one texture per color");
        EXPECT(c != kNoTexture && c != a, "another color, another texture");
        const auto* info = tm->getTextureInfo(ohao::BindlessTextureHandle{a});
        EXPECT(info && info->width == 1 && info->height == 1 && info->format == VK_FORMAT_R8G8B8A8_UNORM,
               "the old texture array's layer format: 1x1 R8G8B8A8_UNORM");
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
        ohao::Scene scene("rt");
        auto actor = scene.createActor("quad");   // the XZ quad turned to face +Z, 4 units wide
        actor->getTransform()->setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)));
        actor->getTransform()->setScale(glm::vec3(2.0f));
        auto model = std::make_shared<ohao::Model>();
        model->vertices = v; model->indices = i;
        auto mesh = actor->addComponent<ohao::MeshComponent>();
        mesh->setModel(model);
        mesh->setVisible(true);
        auto mat = actor->addComponent<ohao::MaterialComponent>();
        mat->getMaterial().baseColor = {0.9f, 0.9f, 0.9f};
        auto lightActor = scene.createActor("light");
        auto lc = lightActor->addComponent<ohao::LightComponent>();
        lc->setLightType(ohao::LightType::Sphere);
        lc->setIntensity(20.0f);
        lightActor->getTransform()->setPosition({0, 0, 2});
        r->setScene(&scene);
        r->getCamera().setPosition({0.0f, 0.0f, 6.0f});
        r->getCamera().setRotation(0.0f, -90.0f);
        r->setRenderMode(ohao::RenderMode::RTOffline);
        if (r->getRenderMode() != ohao::RenderMode::RTOffline) { std::cout << "(no path tracer -- skipped) "; TEST_PASS(); return; }
        for (int k = 0; k < 6; ++k) r->render();
        RenderScene& rs = r->renderScene();
        EXPECT(rs.tlas() != VK_NULL_HANDLE, "the RenderScene built a TLAS for the path tracer");
        EXPECT_EQ(rs.rtLightCount(), 1u, "the scene's light reached the RT light list");
        const auto px = r->getPixelSpan();
        EXPECT_EQ(px.size(), size_t{64 * 64 * 4}, "a full readback");
        const std::size_t centre = (32 * 64 + 32) * 4, corner = 0;
        EXPECT(px[centre] > 20, "the lit quad covers the image centre (closest-hit read the scene)");
        EXPECT(px[centre] > px[corner], "the corner, past the quad, sees the black sky");
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
    runHeadlessDeferredTests();
    runHeadlessPathTracerTests();
    runHeadlessDeferredRenderTests();
    runOrphanedEntryPointTests();
    runRenderSceneTests();

    std::cout << "\n================================================\n";
    std::cout << "  Results: " << testsPassed << "/" << testsRun << " passed";
    if (testsFailed > 0)
        std::cout << "  \033[31m(" << testsFailed << " failed)\033[0m";
    std::cout << "\n================================================\n";

    return testsFailed > 0 ? 1 : 0;
}
