/**
 * engine_tests.cpp - Unit tests for engine subsystems
 *
 * Tests:
 *   1. EventBus  — subscribe, publish, unsubscribe, clear, typed data, multiple subscribers
 *   2. CommandHistory — execute, undo, redo, canUndo/canRedo, descriptions, clear, max history
 *   3. Scene — createActor, findActor, removeActor, getAllActors, actor count
 *   4. SceneSerializer — serialize JSON structure, round-trip deserialize
 */

#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <cmath>

#include "core/event_bus.hpp"
#include "core/command.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_serializer.hpp"

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
}

// =============================================================================
// SECTION 4 — SCENE SERIALIZER
// =============================================================================

static void runSceneSerializerTests() {
    std::cout << "\n[SceneSerializer]\n";

    TEST_BEGIN("serialize produces valid JSON object");
    {
        Scene scene("SerializeTest");
        auto json = SceneSerializer::serialize(&scene);
        EXPECT(json.is_object(), "serialize should return a JSON object");
        TEST_PASS();
    }

    TEST_BEGIN("serialize includes scene name");
    {
        Scene scene("MyLevel");
        auto json = SceneSerializer::serialize(&scene);
        EXPECT(json.contains("name"), "JSON should contain 'name' key");
        EXPECT(json["name"].get<std::string>() == "MyLevel", "JSON name should match scene name");
        TEST_PASS();
    }

    TEST_BEGIN("serialize includes version field");
    {
        Scene scene("VersionTest");
        auto json = SceneSerializer::serialize(&scene);
        EXPECT(json.contains("version"), "JSON should contain 'version' key");
        TEST_PASS();
    }

    TEST_BEGIN("serialize includes actors array");
    {
        Scene scene("ActorArrayTest");
        scene.createActor("Box1");
        scene.createActor("Sphere1");
        auto json = SceneSerializer::serialize(&scene);
        EXPECT(json.contains("actors"), "JSON should contain 'actors' key");
        EXPECT(json["actors"].is_array(), "actors should be a JSON array");
        TEST_PASS();
    }

    TEST_BEGIN("deserialize populates scene name");
    {
        Scene src("SourceScene");
        auto json = SceneSerializer::serialize(&src);

        Scene dst("empty");
        bool ok = SceneSerializer::deserialize(&dst, json);
        EXPECT(ok, "deserialize should return true on valid JSON");
        EXPECT(dst.getName() == "SourceScene", "deserialized scene should have source name");
        TEST_PASS();
    }

    TEST_BEGIN("deserialize round-trip preserves actor names");
    {
        Scene src("RoundTrip");
        src.createActor("Player");
        src.createActor("Enemy");
        auto json = SceneSerializer::serialize(&src);

        Scene dst("empty");
        SceneSerializer::deserialize(&dst, json);
        // After deserialize, actors should exist
        auto player = dst.findActor("Player");
        auto enemy  = dst.findActor("Enemy");
        EXPECT(player != nullptr, "Player should survive round-trip");
        EXPECT(enemy  != nullptr, "Enemy should survive round-trip");
        TEST_PASS();
    }

    TEST_BEGIN("deserialize fails gracefully on invalid JSON");
    {
        Scene scene("Test");
        nlohmann::json bad = nlohmann::json::array();  // Array instead of object
        bool ok = SceneSerializer::deserialize(&scene, bad);
        // Should either return false or not crash
        (void)ok;  // Either outcome is acceptable; must not throw
        TEST_PASS();
    }

    TEST_BEGIN("saveToFile / loadFromFile round-trip");
    {
        const std::string path = "test_scene_tmp.json";
        Scene src("FileRoundTrip");
        src.createActor("Cube");
        bool saved = SceneSerializer::saveToFile(&src, path);
        EXPECT(saved, "saveToFile should return true");

        Scene dst("empty");
        bool loaded = SceneSerializer::loadFromFile(&dst, path);
        EXPECT(loaded, "loadFromFile should return true");
        EXPECT(dst.getName() == "FileRoundTrip", "loaded scene name should match");
        auto cube = dst.findActor("Cube");
        EXPECT(cube != nullptr, "Cube actor should survive file round-trip");

        // Cleanup
        std::remove(path.c_str());
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
    runSceneSerializerTests();

    std::cout << "\n================================================\n";
    std::cout << "  Results: " << testsPassed << "/" << testsRun << " passed";
    if (testsFailed > 0)
        std::cout << "  \033[31m(" << testsFailed << " failed)\033[0m";
    std::cout << "\n================================================\n";

    return testsFailed > 0 ? 1 : 0;
}
