// Audio (sub-project 11): the schedule half of test 27, reverb zones and graded occlusion in the step,
// CPU only. docs/specs/2026-10-28-audio-design.md, "The step: the schedule" (occlusion, reverb zones),
// "Components and hints"; the plan's Task 2.

#include "audio/audio_bank.hpp"
#include "audio/audio_system.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

using namespace ohao::ecs;
using ohao::InputEvent;

namespace {

const ohao::reflect::TypeRegistry& engineTypes() {
    static ohao::reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

constexpr const char* kBlockout = "assets/levels/blockout_room.glb";
constexpr const char* kSlice = "assets/audio/slice.audio.json";
constexpr float kDt = 1.0f / 60.0f;

// In the blockout (engine axes, y up): the room's inside is x in [-4, 4], z in [-3, 3], walls 3 m
// high; the pillar x in [-0.3, 0.3], z in [-0.8, -0.2]; EnemyA's spawn (-2.5, 0.3, -2.0).
constexpr glm::vec3 kSource{-2.5f, 1.5f, -2.0f};      // where an enemy at EnemyA's spawn fires from
constexpr glm::vec3 kOpen{1.5f, 1.62f, 2.2f};         // the player's spawn, eye high: a clear line
constexpr glm::vec3 kBehindPillar{1.5f, 1.62f, 0.4f}; // the pillar between it and kSource

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// A level (the blockout by default), the slice's bank and a camera for ears (no player); level,
// physics and audio installed.
struct Room {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    AudioSchedule* audio{nullptr};
    entt::entity ears{entt::null};

    explicit Room(glm::vec3 at, const std::function<void(World&)>& extra = {}, const char* level = kBlockout) {
        world.seedIds(11);
        auto& reg = world.registry();
        ohao::level::installLevels(world, files, engineTypes());
        (void)installPhysics(world, files);
        audio = &installAudio(world, files);
        const entt::entity lv = world.createWithId(0x1e7e1, "Level");
        reg.emplace<Level>(lv, Level{AssetRef{level, {}}});
        const entt::entity mix = world.createWithId(0xa0d10, "Mix");
        reg.emplace<AudioMix>(mix, AudioMix{AssetRef{kSlice, {}}});
        ears = world.createWithId(0xea125, "Ears");
        reg.emplace<Camera>(ears, Camera{75.0f, 0.05f, 100.0f, true});
        if (extra) extra(world);
        world.sync();
        for (auto [e, cam, ln] : reg.view<Camera, const LevelNode>().each()) cam.active = false;
        move(at);
    }
    void move(glm::vec3 p) { world.setTransform(ears, Transform{p}); }
    // n steps; every command they wrote, in order.
    std::vector<AudioCommand> run(int n) {
        std::vector<AudioCommand> all;
        for (int i = 0; i < n; ++i) {
            world.update(kDt);
            const auto c = audioCommands(world);
            all.insert(all.end(), c.begin(), c.end());
        }
        return all;
    }
    entt::entity node(std::string_view name) const {
        for (auto [e, n, ln] : world.registry().view<const Name, const LevelNode>().each())
            if (n.value == name) return e;
        return entt::null;
    }
};

// A ReverbZone box: `half` extents about `centre`.
entt::entity zone(World& w, uint64_t id, const char* preset, glm::vec3 centre, glm::vec3 half, int32_t priority = 0) {
    const entt::entity e = w.createWithId(id, preset);
    w.registry().emplace<ReverbZone>(e, ReverbZone{preset, priority, half});
    w.setTransform(e, Transform{centre});
    return e;
}

std::vector<AudioCommand> only(const std::vector<AudioCommand>& all, AudioCommand::Op op) {
    std::vector<AudioCommand> out;
    for (const auto& c : all)
        if (c.op == op) out.push_back(c);
    return out;
}

// ---- zones ------------------------------------------------------------------------------------

// In, out, on the face, and in two overlapping zones: the higher priority wins, ties go to the
// lower id.
void t27_zone_containment() {
    World w;
    w.seedIds(3);
    zone(w, 0x30, "room", {0.0f, 1.5f, 0.0f}, {4.0f, 1.5f, 3.0f});
    zone(w, 0x20, "corridor", {3.0f, 1.5f, 0.0f}, {1.0f, 1.5f, 1.0f}, 1);
    zone(w, 0x10, "outdoor", {3.5f, 1.5f, 0.0f}, {1.0f, 1.5f, 1.0f}, 1);
    w.sync();
    CHECK(listenerZone(w, {0.0f, 1.0f, 0.0f}) == "room", "inside the room");
    CHECK(listenerZone(w, {10.0f, 1.0f, 0.0f}).empty(), "outside every zone: none (the bank's default)");
    CHECK(listenerZone(w, {-4.0f, 1.0f, 0.0f}) == "room", "exactly on a face: inside");
    CHECK(listenerZone(w, {-4.001f, 1.0f, 0.0f}).empty(), "a millimetre past it: outside");
    CHECK(listenerZone(w, {2.2f, 1.0f, 0.0f}) == "corridor", "room and corridor: the higher priority");
    CHECK(listenerZone(w, {3.2f, 1.0f, 0.0f}) == "outdoor", "three zones, two at priority 1: the lower id");
}

// Crossing a zone's edge writes one zone command at that step (preset, bed, a 30-step blend), and
// none while the listener stays.
void t27_zone_change_command() {
    Room r({0.0f, 1.6f, 0.0f}, [](World& w) { zone(w, 0x2001, "room", {0.0f, 1.5f, 0.0f}, {4.0f, 1.5f, 3.0f}); });
    const auto first = only(r.run(1), AudioCommand::Op::Zone);
    CHECK(first.size() == 1 && first[0].zone == "room", "the first step: the room's zone");
    CHECK(first[0].step == 1 && first[0].blendSteps == kZoneBlendSteps, "at step 1, blended over 30 steps");
    CHECK(first[0].bed == "bed_room", "with the room preset's bed (assets/audio/ambience/room.wav)");
    CHECK(only(r.run(5), AudioCommand::Op::Zone).empty(), "staying in the room writes nothing");
    r.move({1.5f, 1.6f, 6.0f});   // out through the doorway, into the open
    const auto out = only(r.run(1), AudioCommand::Op::Zone);
    CHECK(out.size() == 1 && out[0].zone == "outdoor" && out[0].step == 7 && out[0].bed == "bed_outdoor",
          "outside: the bank's default zone and its bed, at step 7");
    CHECK(r.audio->zone() == "outdoor", "the schedule's zone");
}

// A level node's verb hint: a trigger box with ReverbZone{preset}, unless the node says otherwise;
// a preset the bank lacks is W_AUDIO_PRESET and counts as the default; the extras schema has verb.
void t27_verb_hint() {
    Room r({0.0f, 1.0f, 0.0f}, {}, "tests/level/fixtures/level_audio.gltf");
    const auto& reg = r.world.registry();
    const entt::entity roomZone = r.node("RoomZone"), cave = r.node("CaveZone"), solid = r.node("SolidZone");
    CHECK(roomZone != entt::null && cave != entt::null && solid != entt::null, "the fixture's three nodes");
    CHECK(reg.all_of<ReverbZone>(roomZone) && reg.get<ReverbZone>(roomZone).preset == "room", "verb room: ReverbZone{room}");
    CHECK(reg.all_of<Collider>(roomZone) && reg.get<Collider>(roomZone).shape == ColliderShape::Box &&
              reg.get<Collider>(roomZone).trigger,
          "verb makes a trigger box collider");
    CHECK(reg.all_of<Collider>(solid) && reg.get<Collider>(solid).shape == ColliderShape::Mesh &&
              !reg.get<Collider>(solid).trigger && reg.get<ReverbZone>(solid).preset == "corridor",
          "a node saying col mesh keeps its solid collider, and is still a zone");
    (void)r.run(1);
    CHECK(r.audio->zone() == "room", "the listener inside RoomZone (fitted to its mesh): room");
    bool warned = false;
    for (const auto& w : r.audio->warnings()) warned = warned || (w.code == "W_AUDIO_PRESET" && w.entity == cave);
    CHECK(warned, "CaveZone's unknown preset: W_AUDIO_PRESET");
    r.move({20.0f, 1.0f, 0.0f});   // inside CaveZone
    (void)r.run(1);
    CHECK(r.audio->zone() == "outdoor", "an unknown preset's zone counts as the default");
    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / "tests/level/extras_schema.json", std::ios::binary);
    const std::string schema((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(schema.find("\"verb\"") != std::string::npos, "the extras schema has verb");
}

// ---- occlusion --------------------------------------------------------------------------------

// 0, 1 and 2 walls between the listener and the source: none; -8 dB and 1.2 kHz; -14 dB and 700 Hz.
void t27_occlusion_graded() {
    Room r(kOpen);
    const World& w = r.world;
    const Occlusion none = occlusionBetween(w, AudioListener{kOpen}, kSource);
    CHECK(none.walls == 0 && none.db == 0.0f && none.lowPassHz == 0.0, "a clear line: none");
    const Occlusion one = occlusionBetween(w, AudioListener{{-6.0f, 1.6f, 1.0f}}, {-2.0f, 1.5f, 1.0f});
    CHECK(one.walls == 1 && near(one.db, -8.0f) && one.lowPassHz == 1200.0, "through the west wall: -8 dB, 1.2 kHz");
    const Occlusion two = occlusionBetween(w, AudioListener{{-6.0f, 1.6f, 1.0f}}, {6.0f, 1.6f, 1.0f});
    CHECK(two.walls == 2 && near(two.db, -14.0f) && two.lowPassHz == 700.0, "through both side walls: -14 dB, 700 Hz");
    const Occlusion pillar = occlusionBetween(w, AudioListener{kBehindPillar}, kSource);
    CHECK(pillar.walls == 1, "behind the pillar: one wall");
}

// A long voice is re-cast every 6 steps; the listener stepping behind the pillar gives one occlusion
// command, at the re-cast after the move; a short one is never re-cast.
void t27_occlusion_recast() {
    Room r(kOpen);
    (void)r.run(1);
    const AudioSchedule::Voice longVoice = r.audio->trackVoice(kSource, 2000, true);
    const AudioSchedule::Voice shortVoice = r.audio->trackVoice(kSource, 100, true);
    CHECK(longVoice.occl.walls == 0 && shortVoice.occl.walls == 0, "both start in the open: unoccluded");
    CHECK(only(r.run(2), AudioCommand::Op::Occlusion).empty(), "no change, no command");
    r.move(kBehindPillar);
    const auto after = only(r.run(16), AudioCommand::Op::Occlusion);   // steps 4 to 19
    CHECK(after.size() == 1, "one occlusion command for the move");
    CHECK(after[0].voice == longVoice.id && after[0].step == 7, "the long voice's, at its re-cast 6 steps after its start");
    CHECK(after[0].occl.walls == 1 && near(after[0].occl.db, -8.0f) && after[0].occl.lowPassHz == 1200.0,
          "graded: one wall");
    CHECK(after[0].blendSteps == kOcclusionBlendSteps, "ramped over 3 steps");
}

// 20 sounds in one step: 16 rays cast, 4 past the cap counted and unoccluded; every voice exists.
void t27_ray_cap() {
    Room r(kBehindPillar);
    (void)r.run(1);
    const AudioScheduleStats before = r.audio->stats();
    std::vector<AudioSchedule::Voice> voices;
    for (int i = 0; i < 20; ++i) voices.push_back(r.audio->trackVoice(kSource, 600, true));
    const AudioScheduleStats after = r.audio->stats();
    CHECK(after.raysCast - before.raysCast == kRaysPerStep, "16 rays cast");
    CHECK(after.raysCapped - before.raysCapped == 4, "4 past the cap, counted");
    CHECK(after.voices == before.voices + 20, "all 20 voices exist");
    const auto capped = std::count_if(voices.begin(), voices.end(), [](const auto& v) { return v.capped; });
    CHECK(capped == 4 && voices.back().capped && voices.back().occl.walls == 0, "past the cap: unoccluded");
    CHECK(voices.front().occl.walls == 1, "under the cap: behind the pillar, occluded");
    (void)r.run(1);   // a new step, a new allowance
    const AudioSchedule::Voice next = r.audio->trackVoice(kSource, 600, true);
    CHECK(!next.capped && next.occl.walls == 1, "the next step casts again");
}

// ---- the play scene, behind cover, rendered ---------------------------------------------------

// The play scene's EnemyA firing from its spawn, the listener in the open and then behind the pillar
// at the same distance: the schedule's occlusion goes from none to one wall, and the rendered shot's
// energy above 2 kHz falls by at least 8 dB, its broadband level by at least 6 dB.
double bandPower(const std::vector<float>& x, double lo, double hi) {
    double sum = 0.0;
    int bins = 0;
    for (double f = lo; f <= hi; f += 250.0, ++bins) {
        const double w = 2.0 * 3.14159265358979 * f / 48000.0, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (float v : x) {
            const double s0 = v + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        sum += (s1 * s1 + s2 * s2 - c * s1 * s2) / (double(x.size()) * double(x.size()));
    }
    return bins ? sum / bins : 0.0;
}

void t27_behind_cover_render() {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    ohao::level::installLevels(world, files, engineTypes());
    (void)installPhysics(world, files);
    AudioSchedule& audio = installAudio(world, files);
    const auto loaded = loadSceneFile(world, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json",
                                      engineTypes());
    CHECK(static_cast<bool>(loaded), "the play scene loads");
    world.sync();
    auto& reg = world.registry();
    entt::entity player = entt::null, enemy = entt::null;
    for (auto [e, pc] : reg.view<const PlayerController>().each())
        if (pc.input) player = e;
    for (auto [e, n] : reg.view<const Name>().each())
        if (n.value == "EnemyA") enemy = e;
    CHECK(player != entt::null && enemy != entt::null, "the player and EnemyA");
    world.update(kDt);
    const glm::vec3 source = reg.get<Transform>(enemy).position + glm::vec3(0.0f, 1.5f, 0.0f);

    // Behind the pillar, and a spot in the open at the same distance from the enemy (60 degrees round).
    const glm::vec2 toCover = glm::vec2(1.5f, 0.4f) - glm::vec2(source.x, source.z);
    const float dist = glm::length(toCover);
    const glm::vec2 open = glm::vec2(source.x, source.z) + dist * glm::vec2(0.5f, 0.8660254f);
    const auto listenFrom = [&](glm::vec2 xz) -> std::pair<Occlusion, glm::vec3> {
        world.setTransform(player, Transform{{xz.x, 0.0f, xz.y}});
        world.update(kDt);
        const auto ears = listenerAtStep(world);
        if (!ears) return {Occlusion{}, glm::vec3(0.0f)};
        return {audio.trackVoice(source, 600, true).occl, ears->position};
    };
    const auto [openOccl, openEars] = listenFrom(open);
    const auto [coverOccl, coverEars] = listenFrom({1.5f, 0.4f});
    std::printf("\n      open %.2f m: %u walls; behind the pillar %.2f m: %u walls\n", glm::length(source - openEars),
                openOccl.walls, glm::length(source - coverEars), coverOccl.walls);
    CHECK(openOccl.walls == 0, "in the open: no occlusion");
    CHECK(coverOccl.walls == 1 && near(coverOccl.db, -8.0f) && coverOccl.lowPassHz == 1200.0, "behind the pillar: one wall");

    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / kSlice, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto parsed = ohao::parseAudioBank(json);
    CHECK(static_cast<bool>(parsed), "the slice bank parses");
    const ohao::AudioBank bank = std::move(parsed).value();
    const int shot = bank.sound("enemy_fire");
    const ohao::ShapeDesc* gunfire = bank.shape("gunfire");
    CHECK(shot >= 0 && gunfire, "enemy_fire on the gunfire curve");
    const auto render = [&](glm::vec3 ears, const Occlusion& o) {
        ohao::AudioSystem a;
        std::vector<float> mono;
        if (!a.initialize(true) || !a.loadBank(bank, OHAO_SOURCE_DIR)) return mono;
        a.setListenerPosition(ears, glm::normalize(source - ears), {0.0f, 1.0f, 0.0f});
        ohao::VoicePlacement p;
        p.spatial = true;
        p.position = source;
        p.shape = ohao::SoundShape{gunfire->minDistance, gunfire->maxDistance, gunfire->rolloff, false};
        p.occlusionDb = o.db;
        p.occlusionLowPassHz = o.lowPassHz;
        if (!ohao::isValidSoundHandle(a.play(static_cast<uint16_t>(shot), 0, p))) return mono;
        std::vector<float> st(std::size_t(24000) * 2);
        a.render(st, 24000);
        mono.resize(24000);
        for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (st[i * 2] + st[i * 2 + 1]);
        return mono;
    };
    const auto clear = render(openEars, openOccl), covered = render(coverEars, coverOccl);
    CHECK(clear.size() == 24000 && covered.size() == 24000, "both shots render");
    double eClear = 0.0, eCovered = 0.0;
    for (std::size_t i = 0; i < clear.size(); ++i) eClear += double(clear[i]) * clear[i], eCovered += double(covered[i]) * covered[i];
    const double broadband = 10.0 * std::log10(eCovered / eClear);
    const double high = 10.0 * std::log10(bandPower(covered, 2000.0, 10000.0) / bandPower(clear, 2000.0, 10000.0));
    std::printf("      behind cover: broadband %.2f dB, above 2 kHz %.2f dB\n", broadband, high);
    CHECK(high <= -8.0, "above 2 kHz at least 8 dB down");
    CHECK(broadband <= -6.0, "broadband at least 6 dB down");
}

// =============================================================================
// Test 28: the schedule (sub-project 11 step 3)
// =============================================================================

ohao::AudioBank sliceBank() {
    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / kSlice, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = ohao::parseAudioBank(json);
    return r ? std::move(r).value() : ohao::AudioBank{};
}

ohao::AudioBank bankFrom(const char* json) {
    auto r = ohao::parseAudioBank(json);
    if (!r) {
        for (const auto& e : r.error()) std::printf("\n      bank error %s: %s", e.path.c_str(), e.message.c_str());
        return {};
    }
    return std::move(r).value();
}

InputEvent actionEvent(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}

InputEvent axisSet(const char* name, float value) {
    InputEvent e;
    e.type = InputEvent::Type::AxisSet;
    e.name = name;
    e.value = value;
    return e;
}

constexpr uint64_t kEnemyA = 0x5c0b1a7e00000010ull, kEnemyB = 0x5c0b1a7e00000020ull;

// The play scene as hosts load it (levels, physics, combat, the round hook, animation, and the audio
// schedule unless `audio` is false).
struct Fight {
    World w;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    PhysicsScene* physics{nullptr};
    CombatSystem* combat{nullptr};
    AudioSchedule* audio{nullptr};
    entt::entity player{entt::null};
    bool loaded{false};

    explicit Fight(bool withAudio = true) {
        w.seedIds(7);
        ohao::level::installLevels(w, files, engineTypes());
        physics = &installPhysics(w, files);
        combat = &installCombat(w);
        installRoundHook(w, engineTypes());
        installAnimation(w, files);
        if (withAudio) audio = &installAudio(w, files);
        loaded = static_cast<bool>(
            loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes()));
        if (loaded)
            for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);
        for (auto [e, pc] : w.registry().view<const PlayerController>().each())
            if (pc.input) player = e;
    }
    uint64_t playerId() const { return w.idOf(player); }
    // n steps with `inputs(i)` at step i (counting from `from`); every command written, in order.
    std::vector<AudioCommand> run(int n, const std::function<std::vector<InputEvent>(int)>& inputs = {}, int from = 0) {
        std::vector<AudioCommand> all;
        for (int i = 0; i < n; ++i) {
            w.stepInput(inputs ? inputs(from + i) : std::vector<InputEvent>{});
            w.update(kDt);
            const auto c = audioCommands(w);
            all.insert(all.end(), c.begin(), c.end());
        }
        return all;
    }
    // Faces `id` (an enemy) from where the player stands.
    void face(uint64_t id) {
        const glm::vec3 me = physics->playerInfo(player)->pos, it = w.registry().get<Transform>(w.findById(id)).position;
        physics->teleportPlayer(player, me, glm::degrees(std::atan2(-(it.x - me.x), -(it.z - me.z))));
    }
};

// The done demo's fight, scripted per step: bursts of fire every 40 steps (held 13), a walk forward
// between steps 100 and 160, a reload at 300.
std::vector<InputEvent> script(int i) {
    std::vector<InputEvent> in;
    if (i % 40 == 0) in.push_back(actionEvent("fire", true));
    if (i % 40 == 13) in.push_back(actionEvent("fire", false));
    if (i == 100) in.push_back(axisSet("move_forward", 1.0f));
    if (i == 160) in.push_back(axisSet("move_forward", 0.0f));
    if (i == 300) in.push_back(actionEvent("reload", true));
    if (i == 302) in.push_back(actionEvent("reload", false));
    return in;
}

std::vector<std::string> lines(const std::vector<AudioCommand>& cmds, const ohao::AudioBank& bank, bool skipResync = false) {
    std::vector<std::string> out;
    for (const auto& c : cmds)
        if (!(skipResync && c.resync)) out.push_back(describeAudioCommand(c, bank));
    return out;
}

// Prints where two dumps first differ (nothing when equal).
void firstDiff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
        if (x == y) continue;
        std::printf("\n      first difference at line %zu of %zu/%zu:\n        %s\n        %s", i, a.size(), b.size(), x.c_str(), y.c_str());
        return;
    }
}

std::size_t countOp(const std::vector<AudioCommand>& cmds, AudioCommand::Op op) {
    return static_cast<std::size_t>(std::count_if(cmds.begin(), cmds.end(), [op](const AudioCommand& c) { return c.op == op; }));
}

// Every case of runtime_tests' sp6_t13_*, sp7_fx_sounds_by_event_and_surface, sp9_t21_voice_cap,
// distance_curves, occlusion_ray and sp10_t23_*, restated on the schedule.
void t28_rules_moved() {
    // Which sounds a combat event names (sp7_fx_sounds_by_event_and_surface).
    using K = CombatEvent::Kind;
    constexpr uint64_t player = 0xb1a7e2, enemy = 0xe1;
    const auto ev = [](K kind, uint64_t source, uint64_t target, float amount = 0.0f, std::string surface = {}) {
        CombatEvent e;
        e.kind = kind;
        e.sourceId = source;
        e.targetId = target;
        e.amount = amount;
        e.surface = std::move(surface);
        return e;
    };
    using V = std::vector<std::string>;
    CHECK(combatSoundNames(ev(K::Shot, player, 0), player) == V{"fire"}, "the player's round: fire");
    CHECK(combatSoundNames(ev(K::Shot, enemy, 0), player) == V{"enemy_fire"}, "an enemy's round: enemy_fire");
    CHECK(combatSoundNames(ev(K::Hit, player, 7, 4.0f, "metal"), player) == V{"impact_metal"}, "a hit: its surface's impact");
    CHECK(combatSoundNames(ev(K::Hit, enemy, player, 4.0f, "default"), player) == V{"impact_default"}, "on anything");
    // Damage makes no sound: the player's hurt cue masked the enemy's shot (Frank, ruling 22).
    CHECK(combatSoundNames(ev(K::Damage, enemy, player, 8.0f), player).empty(), "damage to the player: nothing");
    CHECK(combatSoundNames(ev(K::Damage, player, enemy, 25.0f), player).empty(), "damage to anyone else: nothing");
    CHECK(combatSoundNames(ev(K::Death, player, enemy), player) == V{"death"}, "a death");
    CHECK(combatSoundNames(ev(K::Reload, player, 0, 0.0f), player) == (V{"empty", "reload"}), "the magazine ran dry");
    CHECK(combatSoundNames(ev(K::Reload, player, 0, 12.0f), player) == V{"reload"}, "a reload by the key");
    CHECK(combatSoundNames(ev(K::Reload, enemy, 0, 0.0f), player).empty(), "an enemy's reload: nothing");
    for (const K quiet : {K::Reloaded, K::RoundStart, K::Restart, K::Win})
        CHECK(combatSoundNames(ev(quiet, player, 0), player).empty(), "and nothing else");
    for (const char* own : {"fire", "reload", "empty"}) CHECK(combatSoundAtEar(own), "the player's own at the ear");
    CHECK(!combatSoundAtEar("enemy_fire") && !combatSoundAtEar("impact_metal") && !combatSoundAtEar("death"), "the rest at their point");

    // Handling and footsteps from animation events (sp10_t23_handling_sounds).
    const auto anim = [](std::string name) {
        AnimEvent e;
        e.entity = 0xe1;
        e.clip = "reload";
        e.name = std::move(name);
        return e;
    };
    for (const char* n : {"mag_out", "mag_in", "bolt", "body_fall"}) CHECK(animSoundNames(anim(n)) == V{n}, "a handling event plays its own sound");
    CHECK(animSoundNames(anim("foot_l")) == V{"footstep"} && animSoundNames(anim("foot_r")) == V{"footstep"},
          "a foot event plays a footstep");
    CHECK(animSoundNames(anim("tick")).empty(), "and nothing else");

    // The arms' events are the player's (sp10_t23_arms_sounds_are_the_players).
    {
        World w;
        const entt::entity p = w.createWithId(0xb1a7e2, "Player");
        const entt::entity head = w.createWithId(0xb1a7e3, "Head");
        w.setParent(head, p);
        const entt::entity gun = w.createWithId(0xb1a7e4, "Gun");
        w.setParent(gun, head);
        const entt::entity arms = w.createWithId(0xb1a7e6, "Arms");
        w.setParent(arms, gun);
        (void)w.createWithId(0xe1, "Enemy");
        CHECK(animSoundOwner(w, 0xb1a7e6, 0xb1a7e2) == 0xb1a7e2, "the arms' events are the player's");
        CHECK(animSoundOwner(w, 0xb1a7e2, 0xb1a7e2) == 0xb1a7e2, "the player's own");
        CHECK(animSoundOwner(w, 0xe1, 0xb1a7e2) == 0xe1, "an enemy's are its own");
        CHECK(animSoundOwner(w, 0xb1a7e6, 0) == 0xb1a7e6, "no player: everyone's their own");
    }

    // Footsteps: louder with speed, by the ground's surface (sp6_t13_*).
    CHECK(near(footstepScale(false, 4.0f), 0.65f) && near(footstepScale(false, 6.5f), 0.9f), "a walk 0.65, a sprint 0.9");
    CHECK(near(footstepScale(true, 3.0f), 0.5f) && near(footstepScale(true, 1.0f), 0.4f), "a landing by its impact, at least 0.4");
    const ohao::AudioBank slice = sliceBank();
    CHECK(footstepSound(slice, "metal") == "footstep_metal" && footstepSound(slice, "glass") == "footstep_default",
          "a surface's footsteps, else default's");

    // The distance curves are the bank's shapes, on miniaudio's model (sp9_t21_distance_curves).
    const ohao::ShapeDesc* gun = slice.shape("gunfire");
    const ohao::ShapeDesc* nearShape = slice.shape("near");
    CHECK(gun && gun->minDistance == 1.0f && gun->maxDistance == 60.0f && gun->rolloff == 1.0f, "gunfire: 1 m to 60 m, rolloff 1");
    CHECK(nearShape && nearShape->minDistance == 0.5f && nearShape->maxDistance == 20.0f && nearShape->rolloff == 1.5f,
          "near: 0.5 m to 20 m, rolloff 1.5");
    if (!gun || !nearShape) return;
    for (const float d : {0.25f, 1.0f, 3.0f, 10.0f, 60.0f, 100.0f}) {
        CHECK(distanceGain(d, *gun) == ohao::AudioSystem::distanceGain(d, ohao::SoundShape{1.0f, 60.0f, 1.0f, false}),
              "the schedule's curve is the mixer's (gunfire)");
        CHECK(distanceGain(d, *nearShape) == ohao::AudioSystem::distanceGain(d, ohao::SoundShape{0.5f, 20.0f, 1.5f, false}),
              "and near's");
    }

    // Where each plays, and which get a ray (planCombatSound, planAnimSound, Footsteps).
    Fight f;
    CHECK(f.loaded && f.audio, "the play scene with its schedule");
    if (!f.loaded || !f.audio) return;
    (void)f.run(1);
    const uint64_t me = f.playerId();
    const glm::vec3 eye{-2.5f, 1.9f, -2.0f}, at{-2.5f, 1.4f, -2.0f};
    const auto rays = [&] { return f.audio->stats().raysCast; };
    uint64_t r0 = rays();
    const auto theirs = f.audio->play({"enemy_fire", kEnemyA, eye});
    CHECK(theirs && theirs->spatial && theirs->pos == eye && rays() == r0 + 1, "an enemy's round: at its eye, one ray");
    r0 = rays();
    const auto own = f.audio->play({"fire", me, eye});
    CHECK(own && !own->spatial && rays() == r0, "the player's own: at the ear, no ray");
    const auto impact = f.audio->play({"impact_metal", kEnemyA, at});
    CHECK(impact && impact->spatial && impact->pos == at && rays() == r0, "an impact: at it, no ray");
    const auto handling = f.audio->play({"mag_out", kEnemyA, at});
    CHECK(handling && handling->spatial && rays() == r0 + 1, "an enemy's handling: at it, with a ray");
    r0 = rays();
    const auto ownHandling = f.audio->play({"mag_in", me, at, true});
    CHECK(ownHandling && !ownHandling->spatial && rays() == r0, "the player's own handling: at the ear, no ray");
    const auto ownStep = f.audio->play({"footstep_concrete", me, {1.5f, 0.0f, 2.2f}});
    CHECK(ownStep && ownStep->spatial && rays() == r0, "the player's own footstep: at its feet, no ray");
    const auto theirStep = f.audio->play({"footstep_concrete", kEnemyA, {-2.5f, 0.3f, -2.0f}});
    CHECK(theirStep && theirStep->spatial && rays() == r0 + 1, "an enemy's footstep: a ray");
    CHECK(own && theirs && own->gain == slice.sounds[static_cast<std::size_t>(slice.sound("fire"))].volume &&
              theirs->gain == slice.sounds[static_cast<std::size_t>(slice.sound("enemy_fire"))].volume,
          "the gain is the bank's volume (Feel's: fire 0.85, enemy_fire 0.7)");

    // A wall between the listener and an enemy's round: occluded (sp9_t21_occlusion_ray).
    {
        World w;
        FileAssetResolver files{OHAO_SOURCE_DIR};
        (void)installPhysics(w, files);
        AudioSchedule& audio = installAudio(w, files);
        audio.setBank(slice);
        const entt::entity ground = w.create("Ground", Transform{{0.0f, -0.5f, 0.0f}});
        w.registry().emplace<Collider>(ground, Collider{ColliderShape::Box, {30.0f, 0.5f, 30.0f}});
        const entt::entity p = w.create("Player");
        w.registry().emplace<PlayerController>(p);
        const entt::entity head = w.create("Head", Transform{{0.0f, 1.62f, 0.0f}});
        w.setParent(head, p);
        w.registry().emplace<Camera>(head, Camera{75.0f, 0.05f, 100.0f, true});
        PlayerController enemyPc;
        enemyPc.input = false;
        const entt::entity enemy = w.create("Enemy", Transform{{0.0f, 0.0f, -10.0f}});
        w.registry().emplace<PlayerController>(enemy, enemyPc);
        w.update(kDt);
        const glm::vec3 enemyEye = glm::vec3(w.registry().get<WorldTransform>(enemy).world[3]) + glm::vec3(0.0f, 1.6f, 0.0f);
        const auto open = audio.play({"enemy_fire", w.idOf(enemy), enemyEye});
        CHECK(open && open->occl.walls == 0, "in the open: heard (the source's own capsule passed through)");
        const entt::entity wall = w.create("Wall", Transform{{0.0f, 1.5f, -5.0f}});
        w.registry().emplace<Collider>(wall, Collider{ColliderShape::Box, {4.0f, 2.0f, 0.1f}});
        w.update(kDt);
        const auto blocked = audio.play({"enemy_fire", w.idOf(enemy), enemyEye});
        CHECK(blocked && blocked->occl.walls == 1 && near(blocked->occl.db, -8.0f) && blocked->occl.lowPassHz == 1200.0,
              "a wall between: -8 dB and the 1.2 kHz low-pass");
        const auto ownRound = audio.play({"fire", w.idOf(p), enemyEye});
        CHECK(ownRound && ownRound->occl.walls == 0, "the player's own round: no ray, never occluded");
    }

    // The voice cap (sp9_t21_voice_cap): 30 rounds at 1..30 m alternately near and far on a bus capped
    // at 12; the 12 loudest at the listener hold voices, the rest are lost (dropped, or stolen by a
    // louder one), and the player's own round (higher priority, at the ear) always gets a voice.
    {
        Fight g;
        if (!g.loaded) return;
        (void)g.run(1);
        const auto ears = listenerAtStep(g.w);
        if (!ears) return;
        const int weapons = slice.bus("weapons");
        std::map<uint32_t, float> started;   // voice -> distance
        std::set<uint32_t> lost;
        for (int i = 0; i < 30; ++i) {
            const float d = static_cast<float>(i % 2 == 0 ? 1 + i / 2 : 30 - i / 2);
            const auto c = g.audio->play({"enemy_fire", g.playerId(), ears->position + glm::vec3(0.0f, 0.0f, -d)});
            if (!c) continue;
            if (c->outcome == AudioCommand::Outcome::Dropped) lost.insert(c->voice);
            started[c->voice] = d;
        }
        for (const auto& c : g.audio->stepCommands())
            if (c.op == AudioCommand::Op::Stop) lost.insert(c.voice);
        std::size_t held = 0;
        float quietestKept = 1.0f, loudestLost = 0.0f;
        for (const ScheduledVoice& v : g.audio->voices()) {
            if (v.bus != weapons) continue;
            ++held;
            quietestKept = std::min(quietestKept, distanceGain(started[v.id], *gun));
        }
        for (const uint32_t v : lost) loudestLost = std::max(loudestLost, distanceGain(started[v], *gun));
        std::printf("\n      %zu held on weapons, %zu lost", held, lost.size());
        CHECK(held == 12 && lost.size() == 18, "12 hold (the bus cap), 18 lost");
        CHECK(loudestLost < quietestKept, "the ones lost are the quietest at the listener");
        const auto mine = g.audio->play({"fire", g.playerId(), ears->position});
        CHECK(mine && mine->outcome == AudioCommand::Outcome::Stole, "the player's own round takes a voice (the quietest's)");
    }

    // Footsteps by surface, round robin, every one the walk logged (sp6_t13_footsteps_play_the_walk).
    {
        World w;
        FileAssetResolver files{OHAO_SOURCE_DIR};
        (void)installPhysics(w, files);
        AudioSchedule& audio = installAudio(w, files);
        audio.setBank(slice);
        const entt::entity ground = w.create("Ground", Transform{{0.0f, -0.5f, 0.0f}});
        Collider floor{ColliderShape::Box, {30.0f, 0.5f, 30.0f}};
        floor.surface = "metal";
        w.registry().emplace<Collider>(ground, floor);
        const entt::entity p = w.create("Player");
        w.registry().emplace<PlayerController>(p);
        const entt::entity head = w.create("Head", Transform{{0.0f, 1.62f, 0.0f}});
        w.setParent(head, p);
        w.registry().emplace<Camera>(head, Camera{75.0f, 0.05f, 100.0f, true});
        std::size_t logged = 0;
        std::vector<uint32_t> variants;
        const int metal = slice.sound("footstep_metal");
        for (int i = 0; i < 90; ++i) {
            w.stepInput(i == 0 ? std::vector<InputEvent>{axisSet("move_forward", 1.0f)} : std::vector<InputEvent>{});
            w.update(kDt);
            for (const auto& e : physicsEvents(w)) logged += e.kind == PhysicsEvent::Kind::Footstep;
            for (const auto& c : audioCommands(w))
                if (c.op == AudioCommand::Op::Play && c.sound == metal && c.spatial) variants.push_back(c.variant);
        }
        bool robin = !variants.empty();
        for (std::size_t i = 0; i < variants.size(); ++i) robin = robin && variants[i] == i;
        std::printf("\n      %zu footsteps logged, %zu scheduled", logged, variants.size());
        CHECK(logged >= 6 && variants.size() == logged, "every footstep the steps logged is scheduled once, on its surface's sound");
        CHECK(robin, "variants in turn (the device takes them modulo the files: round robin)");
    }
}

// Over a bus cap: a lower priority is stolen first, then the quieter, then the older; a new voice
// that would lose is dropped; the global cap the same; the same choices every run; a voice ending in
// a step frees its place for that step.
void t28_priority_and_steal() {
    const char* json = R"({
      "buses": {"master": {}, "world": {"parent": "master"}, "weapons": {"parent": "world", "cap": 3},
                "impacts": {"parent": "world", "cap": 2}},
      "voices": 4,
      "shapes": {"near": {"min": 1, "max": 50, "rolloff": 1}},
      "sounds": {
        "shot":  {"files": "t/shot", "bus": "weapons", "priority": 200, "shape": "near", "holdMs": 1000},
        "pop":   {"files": "t/pop", "bus": "weapons", "priority": 100, "shape": "near", "holdMs": 1000},
        "own":   {"files": "t/own", "bus": "weapons", "priority": 250, "shape": "ear", "holdMs": 1000},
        "hit":   {"files": "t/hit", "bus": "impacts", "priority": 120, "shape": "near", "holdMs": 1000},
        "short": {"files": "t/short", "bus": "weapons", "priority": 200, "shape": "near", "holdMs": 50}
      }
    })";
    const ohao::AudioBank bank = bankFrom(json);
    CHECK(!bank.sounds.empty(), "the test bank parses");
    using O = AudioCommand::Outcome;
    const auto scenario = [&](std::vector<std::string>& out) {
        Room r({0.0f, 0.0f, 0.0f});
        r.audio->setBank(bank);
        (void)r.run(1);
        const auto at = [](float d) { return glm::vec3(0.0f, 0.0f, -d); };
        const auto play = [&](const char* s, float d) { return *r.audio->play({s, 0, at(d)}); };
        const AudioCommand p2 = play("pop", 2), p4 = play("pop", 4), s3 = play("shot", 3);
        const AudioCommand s1 = play("shot", 1);   // over weapons' 3: the pops are lower; the quieter goes
        const AudioCommand s10 = play("shot", 10);   // the other pop
        const AudioCommand s20 = play("shot", 20);   // no lower priority, no quieter equal: dropped
        const AudioCommand sHalf = play("shot", 0.5f);   // equal priority, the quietest (10 m) goes
        out = lines({p2, p4, s3, s1, s10, s20, sHalf}, bank);
        CHECK(s1.outcome == O::Stole && s1.victim == p4.voice, "a lower priority first, the quieter of two");
        CHECK(s10.outcome == O::Stole && s10.victim == p2.voice, "then the other lower priority");
        CHECK(s20.outcome == O::Dropped, "a new voice that would lose is dropped");
        CHECK(sHalf.outcome == O::Stole && sHalf.victim == s10.voice, "equal priority: the quietest");
        // The global cap (4): weapons hold 3, an impact the fourth; a louder impact takes the quieter one.
        const AudioCommand h2 = play("hit", 2);
        CHECK(h2.outcome == O::Played, "the fourth voice");
        const AudioCommand h1 = play("hit", 1);
        CHECK(h1.outcome == O::Stole && h1.victim == h2.voice, "the global cap: one victim, the quieter impact");
        // The player's own round (higher priority, at the ear) is never dropped for anything below it.
        const AudioCommand mine = play("own", 0);
        CHECK(mine.outcome == O::Stole, "the player's own takes a voice");
        out.push_back(describeAudioCommand(h2, bank));
        out.push_back(describeAudioCommand(h1, bank));
        out.push_back(describeAudioCommand(mine, bank));
    };
    std::vector<std::string> a, b;
    scenario(a);
    scenario(b);
    CHECK(!a.empty() && a == b, "the same choices every run");

    // Ties: three equal shots, a louder one takes the oldest.
    {
        Room r({0.0f, 0.0f, 0.0f});
        r.audio->setBank(bank);
        (void)r.run(1);
        const auto first = *r.audio->play({"shot", 0, {0.0f, 0.0f, -5.0f}});
        (void)r.audio->play({"shot", 0, {0.0f, 0.0f, 5.0f}});
        (void)r.audio->play({"shot", 0, {5.0f, 0.0f, 0.0f}});
        const auto louder = *r.audio->play({"shot", 0, {0.0f, 0.0f, -1.0f}});
        CHECK(louder.outcome == O::Stole && louder.victim == first.voice, "equal priority and loudness: the oldest");
    }
    // A voice ending in a step frees its place in that step.
    {
        Room r({0.0f, 0.0f, 0.0f});
        r.audio->setBank(bank);
        (void)r.run(1);
        (void)r.audio->play({"short", 0, {0.0f, 0.0f, -1.0f}});   // 50 ms: 3 steps
        (void)r.audio->play({"shot", 0, {0.0f, 0.0f, -1.0f}});
        (void)r.audio->play({"shot", 0, {0.0f, 0.0f, -1.0f}});
        (void)r.run(3);
        const auto next = *r.audio->play({"shot", 0, {0.0f, 0.0f, -30.0f}});
        CHECK(next.outcome == O::Played, "the short voice ended this step: a free place, nothing stolen");
    }
}

// Which file a machine has never changes a command: the hold is the bank's.
void t28_hold_declared() {
    ohao::AudioBank a = sliceBank(), b = sliceBank();
    for (auto& s : b.sounds)
        if (s.name == "fire") s.files = "weapons/reload";   // another, longer file; the same holdMs
    const auto run = [](const ohao::AudioBank& bank) {
        Fight f;
        f.audio->setBank(bank);
        f.face(kEnemyA);
        return lines(f.run(240, script), bank);
    };
    const auto x = run(a), y = run(b);
    CHECK(!x.empty() && x == y, "the same commands with the sound's file swapped for a longer one");
}

// A `fire` ducks its buses while it holds (attack 1 step, release 15), a `sting` the world; the
// deepest duck on a bus wins.
void t28_duck_rules() {
    const char* json = R"({
      "buses": {"master": {}, "world": {"parent": "master"}, "weapons": {"parent": "world"},
                "foley": {"parent": "world"}, "ambience": {"parent": "world"}, "voice": {"parent": "master"}},
      "sounds": {
        "fire":   {"files": "t/fire", "bus": "weapons", "priority": 250, "shape": "ear", "holdMs": 450},
        "reload": {"files": "t/reload", "bus": "foley", "priority": 180, "shape": "ear", "holdMs": 900},
        "sting":  {"files": "t/sting", "bus": "voice", "priority": 240, "shape": "ear", "holdMs": 300}
      },
      "ducks": [
        {"on": "fire", "bus": "ambience", "db": -6, "attackSteps": 1, "releaseSteps": 15},
        {"on": "fire", "bus": "foley", "db": -3, "attackSteps": 1, "releaseSteps": 15},
        {"on": "reload", "bus": "foley", "db": -5, "attackSteps": 2, "releaseSteps": 10},
        {"on": "sting", "bus": "world", "db": -4, "attackSteps": 1, "releaseSteps": 18}
      ]
    })";
    const ohao::AudioBank bank = bankFrom(json);
    Room r({0.0f, 0.0f, 0.0f});
    r.audio->setBank(bank);
    (void)r.run(1);
    const auto busCmds = [&](std::span<const AudioCommand> cmds) {
        std::map<std::string, std::pair<float, uint32_t>> out;
        for (const auto& c : cmds)
            if (c.op == AudioCommand::Op::Bus) out[bank.buses[c.bus].name] = {c.db, c.blendSteps};
        return out;
    };
    (void)r.audio->play({"fire", 0, {}});   // at step 1: held 27 steps (450 ms)
    auto now = busCmds(r.audio->stepCommands());
    CHECK(now.size() == 2 && now["ambience"] == std::make_pair(-6.0f, 1u) && now["foley"] == std::make_pair(-3.0f, 1u),
          "fire: ambience -6 dB and foley -3 dB, attack 1 step");
    (void)r.run(5);
    (void)r.audio->play({"reload", 0, {}});   // at step 6: held 54 steps
    now = busCmds(r.audio->stepCommands());
    CHECK(now.size() == 1 && now["foley"] == std::make_pair(-5.0f, 2u), "a deeper duck on foley wins (-5 dB, its attack)");
    std::map<uint64_t, std::map<std::string, std::pair<float, uint32_t>>> byStep;
    for (int i = 0; i < 60; ++i) {
        (void)r.run(1);
        const auto c = busCmds(r.audio->stepCommands());
        if (!c.empty()) byStep[r.audio->steps()] = c;
    }
    CHECK(byStep.count(28) && byStep[28].size() == 1 && byStep[28]["ambience"] == std::make_pair(0.0f, 15u),
          "fire's hold ends at step 28: ambience back over 15 steps, foley held by the reload");
    CHECK(byStep.count(60) && byStep[60]["foley"] == std::make_pair(0.0f, 10u), "the reload's hold ends at step 60: foley back over 10");
    (void)r.audio->play({"sting", 0, {}});
    now = busCmds(r.audio->stepCommands());
    CHECK(now.size() == 1 && now["world"] == std::make_pair(-4.0f, 1u), "sting: world -4 dB");
}

// dead on the player's death, the default on the restart (dead's out-blend), won on the win, each with
// its blend; a restart writes stop_world first.
void t28_snapshot_from_round() {
    Fight f;
    CHECK(f.loaded && f.audio, "the play scene");
    if (!f.loaded || !f.audio) return;
    // Each snapshot command: its name and blend (a resync's name marked with '*').
    const auto snaps = [](const std::vector<AudioCommand>& cmds) {
        std::vector<std::pair<std::string, uint32_t>> out;
        for (const auto& c : cmds)
            if (c.op == AudioCommand::Op::Snapshot) out.emplace_back(c.snapshot + (c.resync ? "*" : ""), c.blendSteps);
        return out;
    };
    using P = std::vector<std::pair<std::string, uint32_t>>;
    CHECK(snaps(f.run(1)) == (P{{"default", 0}}), "the first step: default, at once");
    (void)f.combat->damage(f.player, 1000.0f, entt::null, {});
    CHECK(snaps(f.run(1)) == (P{{"dead", 18}}), "the player's death: dead over its 18 steps");
    std::vector<AudioCommand> restart;
    for (int i = 0; i < 400 && countOp(restart, AudioCommand::Op::StopWorld) == 0; ++i) restart = f.run(1);
    CHECK(!restart.empty() && restart.front().op == AudioCommand::Op::StopWorld && restart.front().blendSteps == kStopWorldSteps,
          "the restart: stop_world first, over 3 steps");
    CHECK(snaps(restart) == (P{{"default*", 30}, {"default", 0}}),
          "then the device back to the default over dead's 30 out-steps (resync), and the round's start's own default");
    f.w.registry().replace<Health>(f.player, Health{10000.0f, 10000.0f});
    (void)f.combat->damage(f.w.findById(kEnemyA), 1000.0f, entt::null, {});
    (void)f.combat->damage(f.w.findById(kEnemyB), 1000.0f, entt::null, {});
    P won;
    for (int i = 0; i < 3 && won.empty(); ++i) won = snaps(f.run(1));
    CHECK(won == (P{{"won", 60}}), "both enemies dead: won over its 60 steps");
    const auto back = f.run(1, [](int) { return std::vector<InputEvent>{actionEvent("restart", true)}; });
    CHECK(snaps(back) == (P{{"default*", 30}, {"default", 0}}), "the restart key: the default, over won's 30 out-steps");
}

// The done demo's fight twice from a fresh load: the same schedule.
void t28_same_fight_same_schedule() {
    const auto once = [] {
        Fight f;
        f.face(kEnemyA);
        return lines(f.run(600, script), f.audio->bank());
    };
    const auto a = once(), b = once();
    std::size_t plays = 0;
    for (const auto& l : a) plays += l.rfind("play", 0) == 0;
    std::printf("\n      %zu commands, %zu plays", a.size(), plays);
    CHECK(plays > 50 && a == b, "identical dumps (every field but seq)");
}

// A snapshot mid-burst, restored and stepped the same: the same commands (the restore's resync aside).
void t28_restore_mid_fight() {
    Fight f;
    f.face(kEnemyA);
    (void)f.run(205, script);   // step 205: mid-burst (fire held from 200)
    Snapshot snap;
    takeSnapshot(f.w, engineTypes(), snap);
    const auto a = f.run(120, script, 205);
    restoreSnapshot(f.w, snap);
    const auto b = f.run(120, script, 205);
    const auto la = lines(a, f.audio->bank()), lb = lines(b, f.audio->bank(), true);
    std::printf("\n      %zu commands; %zu resync after the restore", la.size(), b.size() - lb.size());
    CHECK(!la.empty() && la == lb, "the same commands, every field but seq");
    CHECK(countOp(b, AudioCommand::Op::StopWorld) >= 1, "the restore stops what the device still played (stop_world, resync)");
}

// The commands after a round restart equal those after the round's start, through the same inputs.
void t28_restart_like_start() {
    Fight f;
    const auto start = f.run(90);
    (void)f.run(60);
    const auto restart = f.run(90, [](int i) { return i == 0 ? std::vector<InputEvent>{actionEvent("restart", true)}
                                                             : i == 1 ? std::vector<InputEvent>{actionEvent("restart", false)}
                                                                      : std::vector<InputEvent>{}; });
    CHECK(!restart.empty() && restart.front().op == AudioCommand::Op::StopWorld, "the restart's stop_world first");
    std::vector<AudioCommand> after(restart.begin() + (restart.empty() ? 0 : 1), restart.end());
    const auto la = lines(start, f.audio->bank()), lb = lines(after, f.audio->bank(), true);
    std::printf("\n      %zu commands after the start, %zu after the restart", la.size(), lb.size());
    CHECK(!la.empty() && la == lb, "the same commands, step numbers included");
}

// Whether the scripted recording replays the world (world.hash at its end), with or without the schedule.
bool worldReplays(bool withAudio, int from = 1, int steps = 300) {
    Fight f(withAudio);
    f.w.registry().replace<Health>(f.player, Health{10000.0f, 10000.0f});
    f.face(kEnemyA);
    (void)f.run(from, script);
    Snapshot start, table;
    takeSnapshot(f.w, engineTypes(), start);
    (void)f.run(steps, script, from);
    const uint64_t a = hashWorld(f.w, engineTypes(), table);
    restoreSnapshot(f.w, start);
    (void)f.run(steps, script, from);
    const uint64_t b = hashWorld(f.w, engineTypes(), table);
    std::printf("\n      %s, from step %d for %d: %016llx / %016llx", withAudio ? "with the schedule" : "without", from, steps,
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return a == b;
}

// input.record then input.replay: the same schedule. The input map's held keys are not in a snapshot,
// so the recording runs from step 100 to 300 (the walk, seven bursts, the enemies' fire), nothing held
// at either end. Whether the world replays is the simulation's: the same with the schedule as without
// (a recording from step 20 to 320 does not replay world.hash either way; one from 100 to 300 does).
void t28_replay() {
    CHECK(worldReplays(false, 20, 300) == worldReplays(true, 20, 300), "the schedule changes nothing about whether the world replays");
    CHECK(worldReplays(false, 100, 200) && worldReplays(true, 100, 200), "the recording's world replays, with the schedule or without");
    Fight f;
    f.w.registry().replace<Health>(f.player, Health{10000.0f, 10000.0f});   // no death: no restart countdown
    f.face(kEnemyA);
    (void)f.run(100, script);
    Snapshot start;
    takeSnapshot(f.w, engineTypes(), start);
    std::vector<std::vector<InputEvent>> recorded;
    std::vector<AudioCommand> a;
    for (int i = 100; i < 300; ++i) {
        recorded.push_back(script(i));
        const auto c = f.run(1, [&](int) { return recorded.back(); });
        a.insert(a.end(), c.begin(), c.end());
    }
    Snapshot table;
    const uint64_t endA = hashWorld(f.w, engineTypes(), table);
    restoreSnapshot(f.w, start);
    const auto b = f.run(200, [&](int i) { return recorded[static_cast<std::size_t>(i)]; });
    const uint64_t endB = hashWorld(f.w, engineTypes(), table);
    std::printf("\n      world.hash after the recording %016llx, after the replay %016llx", static_cast<unsigned long long>(endA),
                static_cast<unsigned long long>(endB));
    const auto la = lines(a, f.audio->bank()), lb = lines(b, f.audio->bank(), true);
    firstDiff(la, lb);
    CHECK(endA == endB, "the world replays (the same world.hash)");
    CHECK(!la.empty() && la == lb, "the replay's schedule is the recording's");
}

// An enemy's run: footsteps at its clip's foot_l and foot_r steps, on its ground's surface, occluded
// by the walls between; its physics footsteps play nothing.
void t28_enemy_footsteps_on_feet() {
    Fight f;
    CHECK(f.loaded && f.audio, "the play scene");
    if (!f.loaded || !f.audio) return;
    f.w.registry().replace<Health>(f.player, Health{10000.0f, 10000.0f});
    f.face(kEnemyA);
    const AnimationSystem* anim = animationSystem(f.w);
    std::size_t feet = 0, steps = 0, physicsSteps = 0, mismatched = 0, wrongSurface = 0, wrongOcclusion = 0;
    for (int i = 0; i < 400; ++i) {
        // Both enemies run a circle (their walk speed, turning 90 degrees a second): the step's physics
        // walks them by their MoveIntent, before their AI writes its own.
        for (const uint64_t id : {kEnemyA, kEnemyB})
            if (const entt::entity e = f.w.findById(id); e != entt::null && f.w.registry().all_of<MoveIntent>(e))
                f.w.registry().replace<MoveIntent>(e, MoveIntent{{0.0f, 1.0f}, 180.0f + 1.5f * static_cast<float>(i), 0.0f, false, false});
        (void)f.run(1);
        std::map<uint64_t, std::size_t> footEvents, footPlays;
        for (const AnimEvent& e : anim->stepEvents())
            if ((e.name == "foot_l" || e.name == "foot_r") && (e.entity == kEnemyA || e.entity == kEnemyB)) ++footEvents[e.entity], ++feet;
        for (const PhysicsEvent& e : physicsEvents(f.w)) {
            const bool enemy = e.aId == kEnemyA || e.aId == kEnemyB;
            physicsSteps += e.kind == PhysicsEvent::Kind::Footstep && enemy;
            if (e.kind == PhysicsEvent::Kind::Land && enemy) ++footEvents[e.aId], ++feet;   // landings stay physics'
        }
        const auto ears = listenerAtStep(f.w);
        for (const AudioCommand& c : audioCommands(f.w)) {
            if (c.op != AudioCommand::Op::Play || (c.source != kEnemyA && c.source != kEnemyB)) continue;
            const std::string& name = f.audio->bank().sounds[c.sound].name;
            if (name.rfind("footstep_", 0) != 0) continue;
            ++footPlays[c.source], ++steps;
            const auto info = f.physics->playerInfo(f.w.findById(c.source));
            wrongSurface += !info || name != footstepSound(f.audio->bank(), info->surface.empty() ? "default" : info->surface);
            if (ears) wrongOcclusion += occlusionBetween(f.w, *ears, c.pos).walls != c.occl.walls;
        }
        mismatched += footEvents != footPlays;
    }
    std::printf("\n      %zu foot events, %zu footsteps scheduled, %zu physics footsteps of enemies (not played)", feet, steps,
                physicsSteps);
    CHECK(feet > 4 && steps == feet && mismatched == 0, "one footstep per foot event, in its step");
    CHECK(wrongSurface == 0, "on the enemy's ground's surface");
    CHECK(wrongOcclusion == 0, "occluded by the walls between, as a ray at the step says");
}

// world.hash after a fight is the same with and without the schedule.
void t28_hash_unchanged() {
    Fight with(true), without(false);
    with.face(kEnemyA);
    without.face(kEnemyA);
    (void)with.run(400, script);
    (void)without.run(400, script);
    Snapshot ta, tb;
    const uint64_t a = hashWorld(with.w, engineTypes(), ta), b = hashWorld(without.w, engineTypes(), tb);
    std::printf("\n      world.hash %016llx with audio, %016llx without", static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
    CHECK(a == b, "the same world.hash");
}

// =============================================================================
// Test 30: the test grenade (ruling 23, Frank's hand test of 2026-10-28)
// =============================================================================

// The player outside, north of the room (its walls between them and the enemies), throws once facing
// north (-z) and turns to `turnYaw` before it goes off. What the grenade left: the explode plays,
// the Explode events, the throw's step and pose, and the listener when it went off.
struct GrenadeRun {
    std::vector<AudioCommand> plays;   // explode plays only
    std::vector<CombatEvent> explodes;
    uint64_t throwStep{0};
    glm::vec3 throwFeet{0.0f};
    float eyeHeight{0.0f}, throwPitch{0.0f};
    std::optional<AudioListener> ears;
    float yawAtExplode{0.0f};
    std::optional<float> groundY;      // the ground under the explosion
};

GrenadeRun throwGrenade(float turnYaw) {
    constexpr int kThrowAt = 5, kTurnAt = 20, kSteps = 200;
    Fight f;
    GrenadeRun out;
    if (!f.loaded) return out;
    const ohao::AudioBank bank = sliceBank();
    const int explode = bank.sound("explode");
    f.physics->teleportPlayer(f.player, {0.0f, -0.05f, -10.0f}, 0.0f);   // yaw 0: facing -z
    out.eyeHeight = f.w.registry().get<PlayerController>(f.player).eyeHeight;
    for (int i = 0; i < kSteps; ++i) {
        std::vector<InputEvent> in;
        if (i == kThrowAt) in.push_back(actionEvent("grenade", true));
        if (i == kThrowAt + 1) in.push_back(actionEvent("grenade", false));
        if (i == kTurnAt) {
            const auto me = f.physics->playerInfo(f.player);
            f.physics->teleportPlayer(f.player, me->pos, turnYaw);
        }
        f.w.stepInput(in);
        f.w.update(kDt);
        if (i == kThrowAt) {
            const auto me = f.physics->playerInfo(f.player);
            out.throwStep = uint64_t(i) + 1;   // the schedule counts steps from 1
            out.throwFeet = me->pos;
            out.throwPitch = me->pitchDeg;
        }
        for (const AudioCommand& c : audioCommands(f.w)) {
            if (c.op == AudioCommand::Op::Play && c.sound == explode) {
                out.plays.push_back(c);
                out.ears = listenerAtStep(f.w);
                out.yawAtExplode = f.physics->playerInfo(f.player)->yawDeg;
            }
        }
        for (const CombatEvent& e : combatEvents(f.w))
            if (e.kind == CombatEvent::Kind::Explode) out.explodes.push_back(e);
    }
    if (!out.explodes.empty()) {
        const glm::vec3 at = out.explodes[0].point;
        if (const auto hit = f.physics->raycast({at.x, 5.0f, at.z}, {0.0f, -1.0f, 0.0f}, 20.0f)) out.groundY = hit->point.y;
    }
    return out;
}

// Rendered from `ears` facing `yawDeg`: 10 log10 of the right channel's energy over the left's.
double renderBalance(const ohao::AudioBank& bank, const AudioCommand& play, glm::vec3 ears, float yawDeg) {
    ohao::AudioSystem a;
    if (!a.initialize(true) || !a.loadBank(bank, OHAO_SOURCE_DIR)) return 0.0;
    const float yaw = glm::radians(yawDeg);
    a.setListenerPosition(ears, {-std::sin(yaw), 0.0f, -std::cos(yaw)}, {0.0f, 1.0f, 0.0f});
    const ohao::ShapeDesc* blast = bank.shape("blast");
    ohao::VoicePlacement p;
    p.spatial = play.spatial;
    p.position = play.pos;
    if (blast) p.shape = ohao::SoundShape{blast->minDistance, blast->maxDistance, blast->rolloff, false};
    p.occlusionDb = play.occl.db;
    p.occlusionLowPassHz = play.occl.lowPassHz;
    if (!ohao::isValidSoundHandle(a.play(play.sound, 0, p))) return 0.0;
    std::vector<float> st(std::size_t(24000) * 2);
    a.render(st, 24000);
    double l = 0.0, r = 0.0;
    for (std::size_t i = 0; i < 24000; ++i) l += double(st[i * 2]) * st[i * 2], r += double(st[i * 2 + 1]) * st[i * 2 + 1];
    return (l > 0.0 && r > 0.0) ? 10.0 * std::log10(r / l) : 0.0;
}

// G throws one grenade: exactly one Explode and one explode voice, 2.5 s (150 steps) after the throw,
// where the throw's arc meets the ground, unoccluded and spatial; the same place and step whichever
// way the player turned, and the voice pans to the right ear facing west and to the left facing east.
void t30_grenade_explode_pans() {
    const GrenadeRun west = throwGrenade(90.0f), east = throwGrenade(270.0f);
    CHECK(west.plays.size() == 1 && east.plays.size() == 1, "one explode voice per throw");
    CHECK(west.explodes.size() == 1 && east.explodes.size() == 1, "one Explode event per throw");
    if (west.plays.size() != 1 || west.explodes.size() != 1 || east.plays.size() != 1) return;
    const AudioCommand& play = west.plays[0];
    const glm::vec3 at = west.explodes[0].point;
    std::printf("\n      thrown at step %llu from (%.3f, %.3f, %.3f); explode at step %llu at (%.3f, %.3f, %.3f), %u walls",
                static_cast<unsigned long long>(west.throwStep), west.throwFeet.x, west.throwFeet.y, west.throwFeet.z,
                static_cast<unsigned long long>(play.step), at.x, at.y, at.z, play.occl.walls);
    CHECK(play.step == west.throwStep + 149, "the throw's step and 149 more: the fuse's 2.5 s");
    CHECK(play.spatial && glm::length(play.pos - at) < 1e-5f, "a positional voice at the Explode's point");
    CHECK(play.occl.walls == 0 && play.source == 0, "in the open: unoccluded (a world source, not the player's own)");
    CHECK(east.plays[0].step == play.step && glm::length(east.plays[0].pos - play.pos) < 1e-6f,
          "the same step and place whichever way the player turned");

    // Where the arc meets the ground, worked out here: from the eye 0.4 m along the look (north, level),
    // 9 m/s along it and 3 m/s up under -9.81 m/s^2, to the ground; it rests 5 cm above where it touched.
    CHECK(near(west.throwPitch, 0.0f, 1e-4f), "thrown level");
    const glm::vec3 eye = west.throwFeet + glm::vec3(0.0f, west.eyeHeight, 0.0f);
    const glm::vec3 from = eye + glm::vec3(0.0f, 0.0f, -0.4f);
    CHECK(west.groundY.has_value(), "ground under the landing");
    if (!west.groundY) return;
    const float t = (3.0f + std::sqrt(9.0f + 2.0f * 9.81f * (from.y - *west.groundY))) / 9.81f;
    const glm::vec3 expected{from.x, *west.groundY + 0.05f, from.z - 9.0f * t};
    std::printf("\n      expected (%.3f, %.3f, %.3f) after %.3f s of flight", expected.x, expected.y, expected.z, t);
    CHECK(glm::length(at - expected) < 0.01f, "on the arc, at the ground, within 1 cm");

    const ohao::AudioBank bank = sliceBank();
    const double facingWest = renderBalance(bank, play, west.ears ? west.ears->position : glm::vec3(0.0f), west.yawAtExplode);
    const double facingEast = renderBalance(bank, east.plays[0], east.ears ? east.ears->position : glm::vec3(0.0f), east.yawAtExplode);
    std::printf("\n      yaw %.0f: right over left %+.2f dB; yaw %.0f: %+.2f dB", west.yawAtExplode, facingWest,
                east.yawAtExplode, facingEast);
    CHECK(west.ears && east.ears, "a listener at both explosions");
    CHECK(near(std::fabs(west.yawAtExplode - east.yawAtExplode), 180.0f, 0.01f), "the two listeners 180 degrees apart");
    CHECK(facingWest >= 3.0, "facing west, the grenade to the north is on the right");
    CHECK(facingEast <= -3.0, "facing east, on the left");
}

// ---- --listen DIR: step 2's listening renders ------------------------------------------------------
//
// step2_zones.wav: the play scene's player fires a burst in the room, then stands outside the doorway
// and fires again; the schedule's zone commands set the return's reverb (the first at once, a change
// over its blend). step2_cover.wav: EnemyA's burst heard in the open, behind the pillar, then behind
// two walls, each graded by occlusionBetween, each segment's reverb the preset listenerZone gives
// there. Each comes with a JSON schedule (step2_zones.json, step2_cover.json) of what drove it. Step 3
// applies a sound's own send per voice; until then its bus sends at the sound's send.

void writeWav(const std::filesystem::path& path, const std::vector<float>& stereo) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<uint32_t>(stereo.size() * sizeof(float));
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(48000); u32(48000 * 8); u16(8); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(stereo.data()), bytes);
}

std::string vec3Json(glm::vec3 v) {
    char text[96];
    std::snprintf(text, sizeof text, "[%.3f, %.3f, %.3f]", v.x, v.y, v.z);
    return text;
}

// The play scene loaded with level, physics and audio installed; the input-driven player and EnemyA.
struct PlayScene {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    AudioSchedule* audio{nullptr};
    entt::entity player{entt::null}, enemy{entt::null};
    ohao::AudioBank bank;

    bool load() {
        ohao::level::installLevels(world, files, engineTypes());
        (void)installPhysics(world, files);
        audio = &installAudio(world, files);
        if (!loadSceneFile(world, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes()))
            return false;
        world.sync();
        for (auto [e, pc] : world.registry().view<const PlayerController>().each())
            if (pc.input) player = e;
        for (auto [e, n] : world.registry().view<const Name>().each())
            if (n.value == "EnemyA") enemy = e;
        std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / kSlice, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto parsed = ohao::parseAudioBank(json);
        if (!parsed) return false;
        bank = std::move(parsed).value();
        return player != entt::null && enemy != entt::null;
    }
    // A mixer on the slice's bank, `sound`'s bus sending at the sound's send.
    bool mixer(ohao::AudioSystem& a, std::initializer_list<const char*> sounds) const {
        if (!a.initialize(true) || !a.loadBank(bank, OHAO_SOURCE_DIR)) return false;
        for (const char* s : sounds)
            if (const int i = bank.sound(s); i >= 0) a.setBusSend(static_cast<uint8_t>(bank.bus(bank.sounds[i].bus)), bank.sounds[i].send);
        return true;
    }
    void setPreset(ohao::AudioSystem& a, const std::string& zone, uint32_t frames) const {
        if (const ohao::ReverbDesc* p = bank.preset(zone.empty() ? bank.defaultZone : zone)) a.setReverb(*p, frames);
    }
};

ohao::VoicePlacement placed(const ohao::AudioBank& bank, int sound, glm::vec3 at, const Occlusion& o = {}) {
    ohao::VoicePlacement p;
    const ohao::ShapeDesc* shape = bank.shape(bank.sounds[sound].shape);
    if (!shape) return p;   // "ear"
    p.spatial = true;
    p.position = at;
    p.shape = ohao::SoundShape{shape->minDistance, shape->maxDistance, shape->rolloff, false};
    p.occlusionDb = o.db;
    p.occlusionLowPassHz = o.lowPassHz;
    return p;
}

bool listenZones(const std::filesystem::path& dir) {
    PlayScene s;
    if (!s.load()) return false;
    ohao::AudioSystem a;
    if (!s.mixer(a, {"fire", "impact_concrete"})) return false;
    const int fire = s.bank.sound("fire"), impact = s.bank.sound("impact_concrete");
    if (fire < 0 || impact < 0) return false;
    const uint32_t rate = a.sampleRate();
    std::vector<float> wav;
    std::string json = "{\n  \"segments\": [\n";
    const struct { const char* name; glm::vec3 feet; } stands[] = {{"in the room", {1.5f, 0.0f, 2.2f}},
                                                                  {"outside the doorway", {1.5f, 0.0f, 5.0f}}};
    for (std::size_t k = 0; k < 2; ++k) {
        s.world.setTransform(s.player, Transform{stands[k].feet});
        s.world.update(kDt);
        const auto ears = listenerAtStep(s.world);
        if (!ears) return false;
        std::string zone;
        uint32_t blend = 0;
        for (const AudioCommand& c : audioCommands(s.world))
            if (c.op == AudioCommand::Op::Zone) zone = c.zone, blend = c.blendSteps;
        // The first zone is the mixer's starting state; a change crossfades over its blend.
        if (!zone.empty()) s.setPreset(a, zone, k == 0 ? 0 : blend * (rate / 60));
        a.setListenerPosition(ears->position, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
        const uint64_t f0 = a.engineFrame() + (k == 0 ? 0 : uint64_t(0.6 * rate));   // after the crossfade
        const glm::vec3 wall = ears->position + glm::vec3(0.0f, 0.0f, -3.0f);
        for (int shot = 0; shot < 3; ++shot) {
            const uint64_t at = f0 + uint64_t(shot) * (rate / 10);
            (void)a.play(static_cast<uint16_t>(fire), uint32_t(shot), placed(s.bank, fire, ears->position), at);
            (void)a.play(static_cast<uint16_t>(impact), uint32_t(shot), placed(s.bank, impact, wall), at + rate / 100);
        }
        const uint32_t frames = (k == 0 ? 2 : 3) * rate;
        std::vector<float> out(std::size_t(frames) * 2);
        a.render(out, frames);
        wav.insert(wav.end(), out.begin(), out.end());
        char seg[512];
        std::snprintf(seg, sizeof seg,
                      "    {\"segment\": \"%s\", \"step\": %llu, \"ears\": %s, \"zoneCommand\": \"%s\", \"blendSteps\": %u, "
                      "\"crossfadeFrames\": %u, \"burstAtSeconds\": %.3f, \"seconds\": %u}%s\n",
                      stands[k].name, static_cast<unsigned long long>(s.audio->steps()), vec3Json(ears->position).c_str(),
                      zone.c_str(), blend, k == 0 ? 0u : blend * (rate / 60), double(wav.size() / 2 - frames) / rate + (k == 0 ? 0.0 : 0.6),
                      frames / rate, k == 0 ? "," : "");
        json += seg;
        std::printf("      zones: %s, ears %s, zone command '%s' (blend %u steps)\n", stands[k].name,
                    vec3Json(ears->position).c_str(), zone.c_str(), blend);
    }
    json += "  ],\n  \"sends\": {\"weapons\": " + std::to_string(s.bank.sounds[fire].send) + ", \"impacts\": " +
            std::to_string(s.bank.sounds[impact].send) + "}\n}\n";
    writeWav(dir / "step2_zones.wav", wav);
    std::ofstream(dir / "step2_zones.json", std::ios::binary) << json;
    return true;
}

bool listenCover(const std::filesystem::path& dir) {
    PlayScene s;
    if (!s.load()) return false;
    s.world.update(kDt);
    const glm::vec3 source = s.world.registry().get<Transform>(s.enemy).position + glm::vec3(0.0f, 1.5f, 0.0f);
    // In the open and behind the pillar at the same distance (as t27_behind_cover_render); behind two
    // walls: on through the pillar and out through the east wall.
    const glm::vec2 src{source.x, source.z};
    const float dist = glm::length(glm::vec2(1.5f, 0.4f) - src);
    const glm::vec2 open = src + dist * glm::vec2(0.5f, 0.8660254f);
    const glm::vec2 throughPillar = glm::normalize(glm::vec2(0.0f, -0.5f) - src);
    struct Spot { std::string name; glm::vec3 ears; Occlusion occl; std::string zone; };
    std::vector<Spot> spots{{"in the open", {open.x, 1.62f, open.y}, {}, {}},
                            {"behind the pillar", {1.5f, 1.62f, 0.4f}, {}, {}}};
    for (float t = 8.0f; t <= 12.0f; t += 0.5f) {
        const glm::vec2 p = src + t * throughPillar;
        const Occlusion o = occlusionBetween(s.world, AudioListener{{p.x, 1.62f, p.y}}, source);
        if (o.walls == 2) {
            spots.push_back({"behind two walls", {p.x, 1.62f, p.y}, o, {}});
            break;
        }
    }
    if (spots.size() != 3) return false;
    for (Spot& sp : spots) {
        if (sp.occl.walls == 0) sp.occl = occlusionBetween(s.world, AudioListener{sp.ears}, source);
        sp.zone = listenerZone(s.world, sp.ears);
    }
    ohao::AudioSystem a;
    if (!s.mixer(a, {"enemy_fire"})) return false;
    const int shot = s.bank.sound("enemy_fire");
    if (shot < 0) return false;
    const uint32_t rate = a.sampleRate();
    std::vector<float> wav;
    std::string json = "{\n  \"source\": " + vec3Json(source) + ",\n  \"segments\": [\n";
    for (std::size_t k = 0; k < spots.size(); ++k) {
        const Spot& sp = spots[k];
        s.setPreset(a, sp.zone, 0);
        a.setListenerPosition(sp.ears, glm::normalize(source - sp.ears), {0.0f, 1.0f, 0.0f});
        const uint64_t f0 = a.engineFrame();
        for (int r = 0; r < 3; ++r)   // a burst of 3 at 4 Hz, as the enemies fire
            (void)a.play(static_cast<uint16_t>(shot), uint32_t(r), placed(s.bank, shot, source, sp.occl), f0 + uint64_t(r) * (rate / 4));
        std::vector<float> out(std::size_t(2) * rate * 2);
        a.render(out, 2 * rate);
        wav.insert(wav.end(), out.begin(), out.end());
        char seg[512];
        std::snprintf(seg, sizeof seg,
                      "    {\"segment\": \"%s\", \"ears\": %s, \"distance\": %.2f, \"walls\": %u, \"db\": %.1f, "
                      "\"lowPassHz\": %.0f, \"zone\": \"%s\", \"atSeconds\": %.1f}%s\n",
                      sp.name.c_str(), vec3Json(sp.ears).c_str(), glm::length(source - sp.ears), sp.occl.walls, sp.occl.db,
                      sp.occl.lowPassHz, sp.zone.empty() ? s.bank.defaultZone.c_str() : sp.zone.c_str(), 2.0 * double(k),
                      k + 1 < spots.size() ? "," : "");
        json += seg;
        std::printf("      cover: %s, %.2f m, %u walls (%.1f dB, %.0f Hz), zone '%s'\n", sp.name.c_str(),
                    glm::length(source - sp.ears), sp.occl.walls, sp.occl.db, sp.occl.lowPassHz, sp.zone.c_str());
    }
    json += "  ],\n  \"send\": {\"weapons\": " + std::to_string(s.bank.sounds[shot].send) + "}\n}\n";
    writeWav(dir / "step2_cover.wav", wav);
    std::ofstream(dir / "step2_cover.json", std::ios::binary) << json;
    return true;
}

std::filesystem::path g_listenDir;

void step2_listen() {
    std::error_code ec;
    std::filesystem::create_directories(g_listenDir, ec);
    CHECK(listenZones(g_listenDir), "step2_zones.wav written");
    CHECK(listenCover(g_listenDir), "step2_cover.wav written");
}

} // namespace

int main(int argc, char** argv) {
    std::printf("audio_schedule_tests\n");
    RUN(t27_zone_containment);
    RUN(t27_zone_change_command);
    RUN(t27_verb_hint);
    RUN(t27_occlusion_graded);
    RUN(t27_occlusion_recast);
    RUN(t27_ray_cap);
    RUN(t27_behind_cover_render);
    RUN(t28_rules_moved);
    RUN(t28_priority_and_steal);
    RUN(t28_hold_declared);
    RUN(t28_duck_rules);
    RUN(t28_snapshot_from_round);
    RUN(t28_same_fight_same_schedule);
    RUN(t28_restore_mid_fight);
    RUN(t28_restart_like_start);
    RUN(t28_replay);
    RUN(t28_enemy_footsteps_on_feet);
    RUN(t28_hash_unchanged);
    RUN(t30_grenade_explode_pans);
    if (argc == 3 && std::string_view(argv[1]) == "--listen") {
        g_listenDir = argv[2];
        RUN(step2_listen);
    }
    return ohao::test::finish("audio_schedule_tests");
}
