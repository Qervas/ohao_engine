// Audio (sub-project 11): the schedule half of test 27, reverb zones and graded occlusion in the step,
// CPU only. docs/specs/2026-10-28-audio-design.md, "The step: the schedule" (occlusion, reverb zones),
// "Components and hints"; the plan's Task 2.

#include "audio/audio_bank.hpp"
#include "audio/audio_system.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

using namespace ohao::ecs;

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
    CHECK(first[0].bed.empty(), "the slice's room preset has no bed yet (step 3 adds the beds)");
    CHECK(only(r.run(5), AudioCommand::Op::Zone).empty(), "staying in the room writes nothing");
    r.move({1.5f, 1.6f, 6.0f});   // out through the doorway, into the open
    const auto out = only(r.run(1), AudioCommand::Op::Zone);
    CHECK(out.size() == 1 && out[0].zone == "outdoor" && out[0].step == 7, "outside: the bank's default zone, at step 7");
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
    if (argc == 3 && std::string_view(argv[1]) == "--listen") {
        g_listenDir = argv[2];
        RUN(step2_listen);
    }
    return ohao::test::finish("audio_schedule_tests");
}
