// audio_bench: the mixer's budgets (sub-project 11, docs/specs/2026-10-28-audio-design.md,
// "Performance budgets").
//
//   audio_bench [--check budgets.json] [--out build/audio_bench.json] [--periods N]
//               [--reference build/serve_bench_reference.json] [--listen DIR] [--grenade-spatial DIR]
//
// Step 1 measures mix_10ms_ms: the slice's bank (assets/audio/slice.audio.json) on a mixer without
// a device, 32 voices playing (positioned, occluded where the bank says so, topped up between
// periods as one-shots end), every bus under `world` with its low-pass engaged and a send to the
// return bus, then N periods (300) of 480 frames (10 ms at 48 kHz) rendered after 20 warm-ups;
// median and p95 of the time to render one period; since step 2 the mix includes the reverb on the
// return (the bank's defaultZone preset). reverb_10ms_ms (gated from step 2): the Freeverb alone,
// stereo, on noise, one preset (room); reverb_xfade_10ms_ms (reported): the same mid-crossfade, two
// cores running. bank_load_ms (gated from step 3): parsing nothing, decoding every variant of every
// sound into a fresh mixer, median of 5. Step 3's (gated from 3): schedule_step_ms, the audio_schedule
// system's step over the play scene's scripted fight (600 steps, 3 rounds); schedule16_step_ms, the
// same with 16 enemies running and firing in the blockout (the ray cap reached); submit_frame_ms, a
// window's player reading and submitting a frame's commands, over the same fight, a frame a step.
//
// --listen DIR writes step1_buses.wav (48 kHz stereo float): one burst of the fight's sounds, first
// with every bus open, then with each leaf bus soloed in turn, and step1_buses.csv (the left
// channel's RMS per 10 ms, by segment) for a level plot. Since step 3 it also plays the done demo's
// fight (900 steps, the player mortal) through the window's AudioPlayer on a mixer without a device:
// step3_fight.wav, step3_fight_schedule.json (every command with the run's step and the schedule's,
// which a round restart takes back to the round start's) and step3_fight_buses.csv (the
// output's level and every bus's driven level per step); and prints how many variants each footstep
// sound loaded (the local folder first).
//
// --grenade-spatial DIR does only this: the test grenade (G, audio ruling 23) going off left and right
// of a fixed listener at 4 and 20 m and 4 m in front, through the schedule and the window's player,
// rendered to grenade_spatial.wav with grenade_spatial_schedule.json (each boom's window, place and
// listener). tools/audio/grenade_spatial.py runs it and measures every window's left/right balance.
//
// The machine is recorded as the other benches record it (bench_env.hpp). A run >25% slower than the
// reference or beside other processes using more than half a core is `noisy`: exit 2.
#include "audio/audio_bank.hpp"
#include "audio/audio_system.hpp"
#include "audio/reverb_node.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"
#include "runtime/audio_player.hpp"

#include <nlohmann/json.hpp>

#include "bench_env.hpp"   // after the engine headers: it includes windows.h

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr uint32_t kVoices = 32;
constexpr uint32_t kPeriod = 480;   // 10 ms at 48 kHz

struct Stats {
    double median{0.0}, p95{0.0}, min{0.0};
};

Stats stats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95), v.front()};
}

bool readBank(AudioBank& bank, std::string& error) {
    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / "assets/audio/slice.audio.json", std::ios::binary);
    if (!f) {
        error = "cannot read assets/audio/slice.audio.json";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = parseAudioBank(json);
    if (!r) {
        error = "slice.audio.json: " + (r.error().empty() ? std::string("?") : r.error()[0].path + ": " + r.error()[0].message);
        return false;
    }
    bank = std::move(r).value();
    return true;
}

// Is `bus` `ancestor` or below it?
bool under(const AudioBank& bank, int bus, int ancestor) {
    for (int b = bus; b >= 0; b = bank.parentOf(b))
        if (b == ancestor) return true;
    return false;
}

// Where the k-th voice of `sound` plays: around the listener at 2 to 11 m, with the bank's shape.
VoicePlacement placementFor(const AudioBank& bank, const SoundDesc& desc, uint32_t k) {
    VoicePlacement p;
    const ShapeDesc* shape = bank.shape(desc.shape);
    if (!shape) return p;   // "ear"
    const float r = 2.0f + float(k % 10), a = 0.7f * float(k);
    p.spatial = true;
    p.position = {r * std::cos(a), 1.0f, r * std::sin(a)};
    p.shape = SoundShape{shape->minDistance, shape->maxDistance, shape->rolloff, desc.occlude};
    return p;
}

// Fills the mixer up to kVoices playing, cycling through the bank's sounds.
void topUp(AudioSystem& a, uint32_t& next) {
    a.update();
    const auto& bank = a.bank();
    for (std::size_t n = a.activeSoundCount(); n < kVoices; ++n, ++next) {
        const auto s = static_cast<uint16_t>(next % bank.sounds.size());
        (void)a.play(s, next / static_cast<uint32_t>(bank.sounds.size()), placementFor(bank, bank.sounds[s], next));
    }
}

void writeWav(const std::filesystem::path& path, const std::vector<float>& stereo, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<uint32_t>(stereo.size() * sizeof(float));
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(rate); u32(rate * 8); u16(8); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(stereo.data()), bytes);
}

// One burst of the fight (the player's shots, an enemy's, impacts, footsteps, a reload, a fall),
// rendered once with every bus open and then with each leaf bus soloed; returns false on failure.
bool listen(const AudioBank& bank, const std::filesystem::path& dir, std::string& error) {
    struct Hit { const char* sound; float at; };
    const Hit burst[] = {{"fire", 0.0f}, {"impact_concrete", 0.02f}, {"fire", 0.1f}, {"impact_metal", 0.12f},
                         {"enemy_fire", 0.3f}, {"footstep_concrete", 0.45f},
                         {"footstep_concrete", 0.9f}, {"reload", 1.0f}, {"body_fall", 1.4f}};
    std::vector<std::string> segments{"all"};   // then every bus the burst plays on, in bank order
    for (const BusDesc& b : bank.buses)
        for (const Hit& h : burst)
            if (const int s = bank.sound(h.sound); s >= 0 && bank.sounds[s].bus == b.name) {
                segments.push_back(b.name);
                break;
            }
    constexpr float kSeconds = 2.4f;
    std::vector<float> wav;
    std::ofstream csv(dir / "step1_buses.csv", std::ios::binary);
    csv << "segment,t_ms,rms_db\n";
    for (const std::string& seg : segments) {
        AudioSystem a;
        if (!a.initialize(true) || !a.loadBank(bank, OHAO_SOURCE_DIR)) {
            error = "the listen mixer did not start";
            return false;
        }
        a.setListenerPosition({0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0});
        if (seg != "all") a.setSolo(static_cast<uint8_t>(bank.bus(seg)), true);
        const uint64_t f0 = a.engineFrame();
        const uint32_t rate = a.sampleRate();
        uint32_t k = 0;
        for (const Hit& h : burst) {
            const int s = bank.sound(h.sound);
            if (s < 0) continue;
            (void)a.play(static_cast<uint16_t>(s), k, placementFor(bank, bank.sounds[s], k + 3),
                         f0 + static_cast<uint64_t>(h.at * float(rate)));
            ++k;
        }
        const auto frames = static_cast<uint32_t>(kSeconds * float(rate));
        std::vector<float> out(std::size_t(frames) * 2);
        a.render(out, frames);
        for (uint32_t p = 0; p + kPeriod <= frames; p += kPeriod) {
            double sum = 0.0;
            for (uint32_t i = p; i < p + kPeriod; ++i) sum += double(out[i * 2]) * out[i * 2];
            const double rms = std::sqrt(sum / kPeriod);
            csv << seg << ',' << (p * 1000 / rate) << ',' << 20.0 * std::log10(std::max(rms, 1e-6)) << '\n';
        }
        wav.insert(wav.end(), out.begin(), out.end());
        wav.insert(wav.end(), std::size_t(rate / 4) * 2, 0.0f);   // a quarter second between segments
    }
    writeWav(dir / "step1_buses.wav", wav, 48000);
    std::printf("  listen: %s (segments:", (dir / "step1_buses.wav").string().c_str());
    for (const auto& s : segments) std::printf(" %s", s.c_str());
    std::printf(")\n");
    return true;
}

// ---- the schedule and the player (step 3) ----------------------------------------------------------

constexpr float kDt = 1.0f / 60.0f;

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

InputEvent actionEvent(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}

// The done demo's fight, as audio_schedule_tests scripts it: bursts every 40 steps (held 13), a walk
// forward from 100 to 160, a reload at 300.
std::vector<InputEvent> fightScript(int i) {
    std::vector<InputEvent> in;
    if (i % 40 == 0) in.push_back(actionEvent("fire", true));
    if (i % 40 == 13) in.push_back(actionEvent("fire", false));
    if (i == 100 || i == 160) {
        InputEvent walk;
        walk.type = InputEvent::Type::AxisSet;
        walk.name = "move_forward";
        walk.value = i == 100 ? 1.0f : 0.0f;
        in.push_back(walk);
    }
    if (i == 300) in.push_back(actionEvent("reload", true));
    if (i == 302) in.push_back(actionEvent("reload", false));
    return in;
}

struct ScheduleRun {
    std::vector<double> stepMs, submitMs;
    uint64_t plays{0}, rays{0}, capped{0}, shots{0}, mostShots{0}, mostRays{0};
    bool ok{false};
};

// What --listen keeps of the fight: the device's output, every command by step, every bus's level.
struct FightRecord {
    std::vector<float> wav;
    nlohmann::json commands = nlohmann::json::array();
    std::string busCsv;
};

// schedule_step_ms and submit_frame_ms: the play scene's scripted fight (the player unkillable, facing
// EnemyA), the schedule's step timed each step, and a window's player on a mixer without a device timed
// once a frame (a frame a step, 800 frames rendered between them). With `rec` (--listen) the player
// keeps its health, so the round can kill it, and the output, the schedule and the bus levels are kept.
ScheduleRun playSceneFight(int steps, FightRecord* rec = nullptr) {
    ScheduleRun out;
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World w;
    w.seedIds(7);
    level::installLevels(w, files, engineTypes());
    ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
    (void)ecs::installCombat(w);
    ecs::installRoundHook(w, engineTypes());
    (void)ecs::installAnimation(w, files);
    ecs::AudioSchedule& audio = ecs::installAudio(w, files);
    if (!ecs::loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes())) return out;
    for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);
    entt::entity player = entt::null;
    for (auto [e, pc] : w.registry().view<const ecs::PlayerController>().each())
        if (pc.input) player = e;
    const entt::entity enemy = w.findById(0x5c0b1a7e00000010ull);
    if (player == entt::null || enemy == entt::null) return out;
    if (!rec) w.registry().replace<ecs::Health>(player, ecs::Health{1.0e6f, 1.0e6f});
    const glm::vec3 me = physics.playerInfo(player)->pos, it = w.registry().get<ecs::Transform>(enemy).position;
    physics.teleportPlayer(player, me, glm::degrees(std::atan2(-(it.x - me.x), -(it.z - me.z))));
    AudioSystem device;
    if (!device.initialize(true)) return out;
    runtime::AudioPlayer player2;
    player2.initialize(OHAO_SOURCE_DIR, device);
    std::vector<float> period(std::size_t(800) * 2);
    for (int i = 0; i < steps; ++i) {
        w.stepInput(fightScript(i));
        w.update(kDt);
        out.stepMs.push_back(audio.lastStepMs());
        const auto t0 = std::chrono::steady_clock::now();
        player2.update(w, true);
        out.submitMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        (void)device.render(period, 800);
        if (!rec) continue;
        rec->wav.insert(rec->wav.end(), period.begin(), period.end());
        for (const ecs::AudioCommand& c : ecs::audioCommands(w))
            rec->commands.push_back({{"runStep", i + 1}, {"step", audio.steps()}, {"command", ecs::describeAudioCommand(c, audio.bank())}});
        const AudioBank& bank = audio.bank();
        if (rec->busCsv.empty()) {
            rec->busCsv = "step,seconds,out_rms_db";
            for (const auto& b : bank.buses) rec->busCsv += "," + b.name;
            rec->busCsv += "\n";
        }
        double sum = 0.0;
        for (float v : period) sum += double(v) * v;
        char row[64];
        std::snprintf(row, sizeof row, "%d,%.4f,%.2f", i + 1, double(i + 1) / 60.0,
                      10.0 * std::log10(std::max(sum / double(period.size()), 1e-12)));
        rec->busCsv += row;
        for (std::size_t b = 0; b < bank.buses.size(); ++b) {
            std::snprintf(row, sizeof row, ",%.2f", player2.busLevelDb(b));
            rec->busCsv += row;
        }
        rec->busCsv += "\n";
    }
    const auto st = audio.stats();
    out.plays = st.played + st.stole;
    out.rays = st.raysCast;
    out.capped = st.raysCapped;
    out.ok = player2.played() > 0;
    return out;
}

// schedule16_step_ms: 16 enemies about the player in the blockout running in and firing, on the slice's
// bank: their rounds and footsteps take rays (the bank occludes those), past the 16 a step when their
// bursts coincide.
ScheduleRun sixteenEnemies(const AudioBank& bank, int steps) {
    ScheduleRun out;
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World w;
    w.seedIds(7);
    level::installLevels(w, files, engineTypes());
    ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
    (void)ecs::installCombat(w);
    ecs::AudioSchedule& audio = ecs::installAudio(w, files);
    audio.setBank(bank);
    auto& reg = w.registry();
    const entt::entity lv = w.createWithId(0x1e7e1, "Level");
    reg.emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"assets/levels/blockout_room.glb", {}}});
    const entt::entity player = w.createWithId(0xb1a7e2, "Player");
    reg.emplace<ecs::PlayerController>(player);
    reg.emplace<ecs::Health>(player, ecs::Health{1.0e6f, 1.0e6f});
    for (int i = 0; i < 16; ++i) {
        const entt::entity e = w.createWithId(0x100 + static_cast<uint64_t>(i), "Enemy");
        ecs::PlayerController pc;
        pc.input = false;
        pc.walkSpeed = 2.5f;
        reg.emplace<ecs::PlayerController>(e, pc);
        reg.emplace<ecs::Health>(e, ecs::Health{1.0e6f, 1.0e6f});
        ecs::Weapon gun;
        gun.damage = 8.0f;
        gun.fireRate = 4.0f;
        gun.magazine = 12;
        gun.recoilPitchDeg = gun.recoilYawDeg = 0.0f;
        reg.emplace<ecs::Weapon>(e, gun);
        // An ellipse about the player on the room's clear south half (0.88 m apart, nothing between
        // any of them and the player), each facing it and 0.6 m out of its attack range: all see it at
        // once, run the same 0.6 m in and fire on the same steps.
        const float a = 6.2831853f * float(i) / 16.0f;
        const glm::vec3 at(-0.5f + 3.0f * std::sin(a), 0.0f, 1.4f + 1.3f * std::cos(a));
        ecs::Enemy ai;   // the AI's range is from its eye to the player's chest (0.65 of its height)
        const float rise = pc.eyeHeight - 0.65f * ecs::PlayerController{}.height;
        ai.attackRange = glm::length(glm::vec2(glm::length(glm::vec2(at.x + 0.5f, at.z - 1.4f)) - 0.6f, rise));
        reg.emplace<ecs::Enemy>(e, ai);
        const float yaw = std::atan2(-(-0.5f - at.x), -(1.4f - at.z));
        ecs::Transform t{at};
        t.rotation = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
        w.setTransform(e, t);
    }
    w.sync();
    physics.teleportPlayer(player, {-0.5f, 0.0f, 1.4f}, 0.0f);
    uint64_t rays = 0;
    for (int i = 0; i < steps; ++i) {
        w.update(kDt);
        if (i >= 20) out.stepMs.push_back(audio.lastStepMs());
        uint64_t shots = 0;
        for (const ecs::CombatEvent& e : ecs::combatEvents(w))
            if (e.kind == ecs::CombatEvent::Kind::Shot) ++shots;
        out.shots += shots;
        out.mostShots = std::max(out.mostShots, shots);
        out.mostRays = std::max(out.mostRays, audio.stats().raysCast - rays);
        rays = audio.stats().raysCast;
    }
    const auto st = audio.stats();
    out.plays = st.played + st.stole + st.dropped;
    out.rays = st.raysCast;
    out.capped = st.raysCapped;
    out.ok = out.plays > 0;
    return out;
}

// ---- --grenade-spatial DIR: the test grenade placed about a fixed listener (audio ruling 23) ---------

// One boom: where it was meant to go off (azimuth: degrees right of the listener's facing; distance).
struct Boom {
    const char* label;
    float azimuthDeg, distance;
};

// The play scene's player stands outdoors north of the room at kListen, facing north (-z), the
// ambience bus muted (its bed would fill every window alike). For each boom it is teleported to where a
// level throw north lands at the boom's place, presses G, is put back at kListen facing north 70 steps
// later (the grenade lands about 57 steps after the throw), and the grenade goes off 149 steps after
// the throw step; the next throw waits 200 steps after the boom, so a 3 s window per boom holds one
// tail. Everything goes through the schedule and a window's AudioPlayer on a mixer without a device, a
// step's 800 frames rendered after each step. Writes grenade_spatial.wav (48 kHz stereo float) and
// grenade_spatial_schedule.json (every boom's window, place and listener; every command).
bool grenadeSpatial(const std::filesystem::path& dir, std::string& error) {
    constexpr glm::vec3 kListen{0.0f, -0.05f, -20.0f};   // the outdoor ground's top is at -0.05
    constexpr int kSettle = 10, kBackAt = 70, kFuseSteps = 149, kGap = 200, kWindowSteps = 180;
    const Boom booms[] = {{"LEFT-near", -90.0f, 4.0f}, {"LEFT-far", -90.0f, 20.0f}, {"RIGHT-near", 90.0f, 4.0f},
                          {"RIGHT-far", 90.0f, 20.0f}, {"FRONT-near", 0.0f, 4.0f}};

    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World w;
    w.seedIds(7);
    level::installLevels(w, files, engineTypes());
    ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
    (void)ecs::installCombat(w);
    ecs::installRoundHook(w, engineTypes());
    (void)ecs::installAnimation(w, files);
    ecs::AudioSchedule& audio = ecs::installAudio(w, files);
    if (!ecs::loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes())) {
        error = "grenade-spatial: the play scene did not load";
        return false;
    }
    for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);
    entt::entity player = entt::null;
    for (auto [e, pc] : w.registry().view<const ecs::PlayerController>().each())
        if (pc.input) player = e;
    if (player == entt::null) {
        error = "grenade-spatial: no input-driven player";
        return false;
    }
    // A level throw from standing lands this far ahead of the thrower's feet (combat::kGrenade*).
    const float eye = w.registry().get<ecs::PlayerController>(player).eyeHeight, g = -ecs::combat::kGrenadeGravity;
    const float lift = ecs::combat::kGrenadeLift;
    const float flight = (lift + std::sqrt(lift * lift + 2.0f * g * eye)) / g;
    const float reach = ecs::combat::kGrenadeAhead + ecs::combat::kGrenadeSpeed * flight;

    AudioSystem device;
    if (!device.initialize(true)) {
        error = "grenade-spatial: the mixer did not start";
        return false;
    }
    runtime::AudioPlayer out;
    out.initialize(OHAO_SOURCE_DIR, device);
    runtime::MixOverrides mix;
    mix.buses.push_back({"ambience", std::nullopt, true, false});
    out.setOverrides(mix);

    std::vector<float> wav, period(std::size_t(800) * 2);
    nlohmann::json commands = nlohmann::json::array(), windows = nlohmann::json::array();
    int step = 0;
    const auto run = [&](int n, const std::vector<InputEvent>& first = {}) {
        for (int i = 0; i < n; ++i, ++step) {
            w.stepInput(i == 0 ? first : std::vector<InputEvent>{});
            w.update(kDt);
            out.update(w, true);
            (void)device.render(period, 800);
            wav.insert(wav.end(), period.begin(), period.end());
            for (const ecs::AudioCommand& c : ecs::audioCommands(w)) {
                // The schedule reads the scene's bank at its first step: names, not indices, from here.
                const bool play = c.op == ecs::AudioCommand::Op::Play && c.sound < audio.bank().sounds.size();
                const std::string sound = play ? audio.bank().sounds[c.sound].name : std::string();
                nlohmann::json j{{"runStep", step + 1}, {"op", int(c.op)}, {"command", ecs::describeAudioCommand(c, audio.bank())}};
                if (play) j["sound"] = sound;
                commands.push_back(std::move(j));
                if (sound == "explode" && !windows.empty() && !windows.back().contains("pos")) {
                    nlohmann::json& b = windows.back();
                    b["runStep"] = step + 1;
                    b["pos"] = {c.pos.x, c.pos.y, c.pos.z};
                    b["walls"] = c.occl.walls;
                    b["startFrame"] = std::size_t(step) * 800;   // the start of the step that wrote it
                    b["frames"] = std::size_t(kWindowSteps) * 800;
                    if (const auto view = ecs::resolveSceneView(w).camera) {
                        const float p = glm::radians(view->pitchDeg), y = glm::radians(view->yawDeg);
                        const glm::vec3 f(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
                        const glm::vec3 right = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
                        const glm::vec3 to = c.pos - view->position;
                        b["ears"] = {view->position.x, view->position.y, view->position.z};
                        b["forward"] = {f.x, f.y, f.z};
                        b["distance"] = glm::length(to);
                        b["azimuthDeg"] = glm::degrees(std::atan2(glm::dot(to, right), glm::dot(to, f)));
                    }
                }
            }
        }
    };
    physics.teleportPlayer(player, kListen, 0.0f);
    run(kSettle);
    for (const Boom& b : booms) {
        const float a = glm::radians(b.azimuthDeg);   // facing -z, right is +x
        const glm::vec3 at = kListen + glm::vec3(b.distance * std::sin(a), 0.0f, -b.distance * std::cos(a));
        windows.push_back({{"label", b.label}, {"plannedAzimuthDeg", b.azimuthDeg}, {"plannedDistance", b.distance},
                           {"planned", {at.x, at.y, at.z}}});
        physics.teleportPlayer(player, at + glm::vec3(0.0f, 0.0f, reach), 0.0f);
        run(1, {actionEvent("grenade", true)});
        run(1, {actionEvent("grenade", false)});
        run(kBackAt - 2);
        physics.teleportPlayer(player, kListen, 0.0f);
        run(kFuseSteps - kBackAt + 1 + kGap);
        if (!windows.back().contains("pos")) {
            error = std::string("grenade-spatial: no explode play for ") + b.label;
            return false;
        }
    }
    static_assert(kWindowSteps <= kGap, "the last window ends before the run does");
    writeWav(dir / "grenade_spatial.wav", wav, 48000);
    std::ofstream(dir / "grenade_spatial_schedule.json", std::ios::binary)
        << nlohmann::json{{"rate", 48000}, {"channels", 2}, {"framesPerStep", 800}, {"steps", step},
                          {"listen", {kListen.x, kListen.y, kListen.z}}, {"facing", "north (-z)"},
                          {"muted", {"ambience"}}, {"throwReach", reach}, {"booms", windows}, {"commands", commands}}
               .dump(1);
    std::printf("  grenade-spatial: %s (%d steps, %zu booms, %llu voices played)\n", (dir / "grenade_spatial.wav").string().c_str(),
                step, windows.size(), static_cast<unsigned long long>(out.played()));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string checkPath, outPath = "build/audio_bench.json", refPath = "build/serve_bench_reference.json", listenDir, spatialDir;
    int periods = 300;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--periods" && i + 1 < argc) periods = std::max(1, std::atoi(argv[++i]));
        else if (a == "--listen" && i + 1 < argc) listenDir = argv[++i];
        else if (a == "--grenade-spatial" && i + 1 < argc) spatialDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: audio_bench [--check budgets.json] [--out file] [--periods N] "
                                 "[--reference file] [--listen DIR] [--grenade-spatial DIR]\n");
            return 2;
        }
    }
    // The grenade placed about the listener, rendered and nothing else (tools/audio/grenade_spatial.py
    // runs this and measures it).
    if (!spatialDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(spatialDir, ec);
        std::string error;
        if (!grenadeSpatial(spatialDir, error)) {
            std::fprintf(stderr, "audio_bench: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    nlohmann::json budgets;
    int currentStep = 0;
    double maxSlowdown = 1.25, maxExternal = 50.0;
    if (!checkPath.empty()) {
        std::ifstream f(checkPath);
        const nlohmann::json doc = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
        if (!f || doc.is_discarded()) {
            std::fprintf(stderr, "audio_bench: cannot read %s as JSON\n", checkPath.c_str());
            return 2;
        }
        budgets = doc.value("budgets", nlohmann::json::object());
        currentStep = doc.value("currentStep", 0);
        if (doc.contains("noise")) {
            maxSlowdown = doc["noise"].value("maxCalibrationSlowdown", maxSlowdown);
            maxExternal = doc["noise"].value("maxExternalCorePct", maxExternal);
        }
    }

    AudioBank bank;
    std::string error;
    if (!readBank(bank, error)) {
        std::fprintf(stderr, "audio_bench: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> failures;

    if (!listenDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(listenDir, ec);
        if (!listen(bank, listenDir, error)) failures.push_back(error);
        // Step 3: the done demo's fight through the player, without a device.
        FightRecord rec;
        const ScheduleRun run = playSceneFight(900, &rec);
        if (!run.ok) failures.push_back("the listen fight played nothing");
        const std::filesystem::path dir(listenDir);
        writeWav(dir / "step3_fight.wav", rec.wav, 48000);
        std::ofstream(dir / "step3_fight_schedule.json", std::ios::binary) << nlohmann::json{{"steps", 900}, {"commands", rec.commands}}.dump(1);
        std::ofstream(dir / "step3_fight_buses.csv", std::ios::binary) << rec.busCsv;
        std::printf("  listen: %s (%zu commands, %llu plays)\n", (dir / "step3_fight.wav").string().c_str(), rec.commands.size(),
                    static_cast<unsigned long long>(run.plays));
        // Local first: the folder each footstep sound's variants come from, and how many the mixer loaded.
        AudioSystem probe;
        if (probe.initialize(true) && probe.loadBank(bank, OHAO_SOURCE_DIR))
            for (const char* s : {"footstep_concrete", "footstep_metal", "footstep_wood", "footstep_default"})
                if (const int i = bank.sound(s); i >= 0) {
                    const auto files = resolveSoundFiles(bank, bank.sounds[std::size_t(i)], OHAO_SOURCE_DIR);
                    std::printf("  listen: %s loads %u variants from %s\n", s, probe.variantCount(static_cast<uint16_t>(i)),
                                files.empty() ? "nowhere" : files.front().parent_path().string().c_str());
                }
    }

    const double cal0 = bench::calibrationMs();
    bench::LoadMonitor monitor;
    monitor.start();
    log::setLevel(log::Level::Warn);

    // bank_load_ms: decode every variant into a fresh mixer.
    std::vector<double> loads;
    for (int i = 0; i < 5; ++i) {
        AudioSystem a;
        if (!a.initialize(true)) break;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = a.loadBank(bank, OHAO_SOURCE_DIR);
        loads.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (!ok) failures.push_back("loadBank failed on the slice bank");
    }

    // mix_10ms_ms: 32 voices, every world bus filtered and sending.
    AudioSystem a;
    const bool started = a.initialize(true) && a.loadBank(bank, OHAO_SOURCE_DIR);
    if (!started) failures.push_back("the deviceless mixer did not start");
    std::vector<double> samples;
    uint32_t fewest = kVoices, worldBuses = 0;
    double sumSquares = 0.0;
    bool finite = true;
    if (started) {
        a.setListenerPosition({0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0});
        const int world = bank.bus("world");
        for (std::size_t b = 0; b < bank.buses.size(); ++b)
            if (world >= 0 && under(bank, static_cast<int>(b), world)) {
                a.setBus(static_cast<uint8_t>(b), -3.0f, 2000.0 + 500.0 * double(worldBuses), 0);
                a.setBusSend(static_cast<uint8_t>(b), 0.2f);
                ++worldBuses;
            }
        uint32_t next = 0;
        std::vector<float> out(std::size_t(kPeriod) * a.channels());
        for (int i = 0; i < 20 + periods; ++i) {
            topUp(a, next);
            fewest = std::min<uint32_t>(fewest, static_cast<uint32_t>(a.activeSoundCount()));
            const auto t0 = std::chrono::steady_clock::now();
            a.render(out, kPeriod);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (i < 20) continue;
            samples.push_back(ms);
            for (float v : out) {
                finite = finite && std::isfinite(v);
                sumSquares += double(v) * v;
            }
        }
    }
    // reverb_10ms_ms (step 2): the Freeverb alone on noise, stereo, the slice's room preset; then every
    // period mid-crossfade to outdoor (two cores running), reported beside it.
    bool reverbOk = true;
    const auto timeReverb = [&](bool crossfading) {
        std::vector<double> t;
        const ReverbDesc* room = bank.preset("room");
        const ReverbDesc* outdoor = bank.preset("outdoor");
        if (!room || !outdoor) {
            reverbOk = false;
            return t;
        }
        Reverb r(48000);
        r.setPreset(*room, 0);
        if (crossfading) r.setPreset(*outdoor, 0x7fffffffu);   // a fade that outlasts the run
        std::vector<float> in(std::size_t(kPeriod) * 2), wet(in.size());
        uint32_t s = 12345u;
        for (int i = 0; i < 20 + periods; ++i) {
            for (float& v : in) {
                s = s * 1664525u + 1013904223u;
                v = 0.3f * (float(s >> 8) / 8388608.0f - 1.0f);
            }
            const auto t0 = std::chrono::steady_clock::now();
            r.process(in.data(), wet.data(), kPeriod);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            for (float v : wet) reverbOk = reverbOk && std::isfinite(v);
            if (i >= 20) t.push_back(ms);
        }
        if (crossfading && !r.crossfading()) reverbOk = false;
        return t;
    };
    const std::vector<double> verbSamples = timeReverb(false), fadeSamples = timeReverb(true);
    if (!reverbOk) failures.push_back("the reverb was not what was measured (no room/outdoor preset, a non-finite sample, or no crossfade)");

    // Step 3: the schedule in the play scene's fight and with 16 enemies, the player's submission.
    log::setLevel(log::Level::Error);   // the loads' info lines
    std::vector<double> scheduleMs, submitMs, schedule16Ms;
    ScheduleRun fight, sixteen;
    for (int r = 0; r < 3; ++r) {
        fight = playSceneFight(600);
        scheduleMs.insert(scheduleMs.end(), fight.stepMs.begin(), fight.stepMs.end());
        submitMs.insert(submitMs.end(), fight.submitMs.begin(), fight.submitMs.end());
        sixteen = sixteenEnemies(bank, 220);
        schedule16Ms.insert(schedule16Ms.end(), sixteen.stepMs.begin(), sixteen.stepMs.end());
    }
    log::setLevel(log::Level::Warn);
    const bool scheduleOk = fight.ok && fight.plays > 50 && sixteen.ok && sixteen.capped > 0;
    if (!scheduleOk)
        failures.push_back("the schedule was not what was measured: fight " + std::to_string(fight.plays) + " plays, 16 enemies " +
                           std::to_string(sixteen.plays) + " plays and " + std::to_string(sixteen.capped) + " capped rays");

    log::setLevel(log::Level::Info);
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);
    const double cal1 = bench::calibrationMs();

    // A benchmark of broken code fails: 32 voices every period, audible, finite output.
    const double rms = samples.empty() ? 0.0 : std::sqrt(sumSquares / (double(samples.size()) * kPeriod * 2));
    const bool mixWrong = !started || fewest < kVoices || !finite || rms < 1e-3 || worldBuses == 0;
    const bool wrong = mixWrong || !reverbOk || !scheduleOk;
    if (started && mixWrong)
        failures.push_back("the mix is not what was measured: fewest voices " + std::to_string(fewest) + ", rms " +
                           std::to_string(rms) + (finite ? "" : ", non-finite samples") + ", world buses " +
                           std::to_string(worldBuses));

    const Stats mix = stats(samples), load = stats(loads);
    std::printf("audio_bench (%d periods of %u frames after 20 warm-ups; %u voices; %u world buses filtered and sending)\n",
                periods, kPeriod, kVoices, worldBuses);
    std::printf("  mix_10ms_ms        median %8.4f   p95 %8.4f   min %8.4f\n", mix.median, mix.p95, mix.min);
    std::string verdict = "reported";
    if (budgets.contains("mix_10ms_ms")) {
        const double max = budgets["mix_10ms_ms"].value("max", 0.0);
        const int from = budgets["mix_10ms_ms"].value("gatedFrom", 0);
        verdict = currentStep < from ? "pending" : mix.median <= max ? "pass" : "fail";
        if (verdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "mix_10ms_ms: median %.4f ms over its budget of %.4f ms", mix.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, verdict.c_str());
    }
    std::printf("  bank_load_ms       median %8.3f   (%zu sounds)\n", load.median, bank.sounds.size());
    std::string loadVerdict = "reported";
    if (budgets.contains("bank_load_ms")) {
        const double max = budgets["bank_load_ms"].value("max", 0.0);
        const int from = budgets["bank_load_ms"].value("gatedFrom", 0);
        loadVerdict = currentStep < from ? "pending" : load.median <= max ? "pass" : "fail";
        if (loadVerdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "bank_load_ms: median %.3f ms over its budget of %.3f ms", load.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.0f ms, gated from step %d: %s\n", max, from, loadVerdict.c_str());
    }
    const Stats verb = stats(verbSamples), fade = stats(fadeSamples);
    std::printf("  reverb_10ms_ms     median %8.4f   p95 %8.4f   (one preset)\n", verb.median, verb.p95);
    std::string verbVerdict = "reported";
    if (budgets.contains("reverb_10ms_ms")) {
        const double max = budgets["reverb_10ms_ms"].value("max", 0.0);
        const int from = budgets["reverb_10ms_ms"].value("gatedFrom", 0);
        verbVerdict = currentStep < from ? "pending" : verb.median <= max ? "pass" : "fail";
        if (verbVerdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "reverb_10ms_ms: median %.4f ms over its budget of %.4f ms", verb.median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, verbVerdict.c_str());
    }
    std::printf("  reverb_xfade_10ms_ms median %6.4f   p95 %8.4f   (mid-crossfade: two cores; reported)\n", fade.median,
                fade.p95);
    // A budget's verdict for a median: pending before its step, else pass or fail (a failure named).
    const auto judge = [&](const char* name, double median) {
        if (!budgets.contains(name)) return std::string("reported");
        const double max = budgets[name].value("max", 0.0);
        const int from = budgets[name].value("gatedFrom", 0);
        const std::string v = currentStep < from ? "pending" : median <= max ? "pass" : "fail";
        if (v == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "%s: median %.4f ms over its budget of %.4f ms", name, median, max);
            failures.push_back(text);
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, v.c_str());
        return v;
    };
    const Stats sched = stats(scheduleMs), sched16 = stats(schedule16Ms), submit = stats(submitMs);
    std::printf("  schedule_step_ms   median %8.4f   p95 %8.4f   (the play scene's fight, %zu steps, %llu plays)\n", sched.median,
                sched.p95, scheduleMs.size(), static_cast<unsigned long long>(fight.plays));
    const std::string schedVerdict = judge("schedule_step_ms", sched.median);
    std::printf("  schedule16_step_ms median %8.4f   p95 %8.4f   (16 enemies, %zu steps; %llu rounds, at most %llu a step; "
                "%llu rays cast, at most %llu a step; %llu capped)\n",
                sched16.median, sched16.p95, schedule16Ms.size(), static_cast<unsigned long long>(sixteen.shots),
                static_cast<unsigned long long>(sixteen.mostShots), static_cast<unsigned long long>(sixteen.rays),
                static_cast<unsigned long long>(sixteen.mostRays), static_cast<unsigned long long>(sixteen.capped));
    const std::string sched16Verdict = judge("schedule16_step_ms", sched16.median);
    std::printf("  submit_frame_ms    median %8.4f   p95 %8.4f   (the player's frame, the same fight)\n", submit.median, submit.p95);
    const std::string submitVerdict = judge("submit_frame_ms", submit.median);
    for (const auto& [name, b] : budgets.items())
        if (name != "mix_10ms_ms" && name != "bank_load_ms" && name != "reverb_10ms_ms" && name != "schedule_step_ms" &&
            name != "schedule16_step_ms" && name != "submit_frame_ms")
            std::printf("  %-18s not measured yet (gated from step %d)\n", name.c_str(), b.value("gatedFrom", 0));

    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;
    std::printf("  machine: calibration before %.3f ms, after %.3f ms, reference %.3f ms (x%.2f); other processes %.1f%% of a core\n",
                cal0, cal1, ref, slow, external);
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);

    nlohmann::json out;
    out["mix_10ms_ms"] = {{"median", mix.median}, {"p95", mix.p95}, {"min", mix.min}, {"samples", samples},
                          {"verdict", verdict}, {"voices", kVoices}, {"fewestVoices", fewest},
                          {"worldBuses", worldBuses}, {"rms", rms}};
    out["bank_load_ms"] = {{"median", load.median}, {"samples", loads}, {"sounds", bank.sounds.size()}, {"verdict", loadVerdict}};
    out["reverb_10ms_ms"] = {{"median", verb.median}, {"p95", verb.p95}, {"min", verb.min}, {"verdict", verbVerdict}};
    out["reverb_xfade_10ms_ms"] = {{"median", fade.median}, {"p95", fade.p95}, {"min", fade.min}};
    out["schedule_step_ms"] = {{"median", sched.median}, {"p95", sched.p95}, {"min", sched.min}, {"steps", scheduleMs.size()},
                               {"plays", fight.plays}, {"verdict", schedVerdict}};
    out["schedule16_step_ms"] = {{"median", sched16.median}, {"p95", sched16.p95}, {"min", sched16.min}, {"steps", schedule16Ms.size()},
                                 {"plays", sixteen.plays}, {"rays", sixteen.rays}, {"capped", sixteen.capped}, {"verdict", sched16Verdict}};
    out["submit_frame_ms"] = {{"median", submit.median}, {"p95", submit.p95}, {"min", submit.min}, {"frames", submitMs.size()},
                              {"verdict", submitVerdict}};
    nlohmann::json busyJson = nlohmann::json::array();
    for (const auto& p : busy) busyJson.push_back({{"name", p.name}, {"corePct", p.corePct}});
    out["machine"] = {{"calibrationBeforeMs", cal0}, {"calibrationAfterMs", cal1}, {"referenceMs", ref},
                      {"externalCorePct", external}, {"busy", busyJson}};
    out["noisy"] = noisy;
    out["checked"] = !checkPath.empty();
    out["passed"] = failures.empty();
    out["failures"] = failures;
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';

    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (noisy)
        std::printf("  NOISY: %s%s%s; the timings above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyCal && noisyLoad ? ", and " : "", noisyLoad ? "other processes were busy" : "");
    std::printf("%s (results in %s)\n", noisy ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    std::string line = "{\"suite\":\"audio_bench\",\"passed\":" + std::to_string(failures.empty() && !noisy ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() && !noisy ? 0 : 1) + ",\"failures\":[";
    if (noisy) failures.push_back("noisy run (see the machine line above)");
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    std::printf("%s}\n", line.c_str());
    if (wrong) return 1;   // broken code is no machine's noise
    if (noisy) return 2;
    return failures.empty() ? 0 : 1;
}
