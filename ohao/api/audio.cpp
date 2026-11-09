// audio.info and audio.mix (sub-project 11, step 4; docs/specs/2026-10-28-audio-design.md, "The
// agent-facing surface").
//
// The session reaches the voice schedule through what installAudio left on the World
// (ecs::audioSchedule); a World without it answers E_STATE. audio.info reads the schedule (every host
// has it: what plays is decided in the step) and, from a window host, the device's half
// (Host::audioDevice). audio.mix keeps its overrides here, beside the session, and hands them all to the
// host each time they change: never the world's, the schedule's or a snapshot's, so world.hash and
// every recorded schedule stay as they were.
#include "api/internal.hpp"

#include "audio/audio_bank.hpp"
#include "ecs/audio_schedule.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

constexpr float kStepMs = 1000.0f / 60.0f;   // the schedule's step: the player's horizon (OPEN ruling 7)

// To 1/100, without -0 (as combat.cpp).
float r2(float v) { return std::fabs(v) < 0.005f ? 0.0f : std::round(v * 100.0f) / 100.0f; }

void writeVec3(JsonWriter& out, glm::vec3 v) {
    out.beginArray();
    for (int i = 0; i < 3; ++i) out.float32(r2(v[i]));
    out.endArray();
}

ecs::AudioSchedule* needSchedule(Session::Impl& s, const JsonNode& params, DecodeContext& ctx) {
    ecs::AudioSchedule* schedule = ecs::audioSchedule(s.world);
    if (!schedule)
        ctx.error("E_STATE", params, "this world has no voice schedule", "a host that installs audio (ohao serve, ohao run)");
    return schedule;
}

// The game's level of a bus now: its snapshot's level plus its duck's.
float gameDb(const AudioBank& bank, const ecs::AudioSchedule& schedule, std::size_t bus) {
    float db = 0.0f;
    for (const SnapshotDesc& snap : bank.snapshots)
        if (snap.name == schedule.snapshot())
            for (const auto& [name, level] : snap.buses)
                if (name == bank.buses[bus].name) db = level.db;
    return db + schedule.busDuckDb(bus);
}

const AudioMixOverrides::Bus* overrideOf(const AudioMixOverrides& mix, std::string_view bus) {
    for (const auto& b : mix.buses)
        if (b.name == bus) return &b;
    return nullptr;
}

std::string busNames(const AudioBank& bank) {
    std::string names;
    for (const BusDesc& b : bank.buses) names += (names.empty() ? "" : ", ") + b.name;
    return names;
}

// audio.info's result (audio.mix's too). The schedule's commands are events.read's audio.* (step 3c).
void writeInfo(Session::Impl& s, const ecs::AudioSchedule& schedule, JsonWriter& out) {
    const AudioBank& bank = schedule.bank();
    const std::optional<AudioDeviceState> device = s.render ? s.render->audioDevice() : std::nullopt;
    const ecs::AudioScheduleStats st = schedule.stats();
    std::vector<uint32_t> busVoices(bank.buses.size(), 0);
    for (const ecs::ScheduledVoice& v : schedule.voices())
        if (v.bus < busVoices.size()) ++busVoices[v.bus];

    out.beginObject();
    out.key("device"); out.string(device ? "default" : "none");
    if (device) { out.key("sampleRate"); out.uint64(device->sampleRate); }
    out.key("horizonMs"); out.float32(r2(device ? static_cast<float>(device->horizonMs) : kStepMs));
    out.key("snapshot");
    out.beginObject();
    out.key("name"); out.string(schedule.snapshot().empty() ? "default" : schedule.snapshot());
    out.key("blend"); out.uint64(schedule.snapshotBlendSteps());
    if (!s.audioMix.snapshot.empty()) { out.key("forced"); out.string(s.audioMix.snapshot); }
    out.endObject();
    out.key("zone"); out.string(schedule.zone().empty() ? bank.defaultZone : schedule.zone());
    if (const auto ears = ecs::listenerAtStep(s.world)) {
        out.key("listener");
        out.beginObject();
        out.key("pos"); writeVec3(out, ears->position);
        out.endObject();
    }
    out.key("buses");
    out.beginArray();
    for (std::size_t b = 0; b < bank.buses.size(); ++b) {
        out.beginObject();
        out.key("name"); out.string(bank.buses[b].name);
        if (const float db = r2(gameDb(bank, schedule, b)); db != 0.0f) { out.key("db"); out.float32(db); }
        if (bank.buses[b].cap) { out.key("cap"); out.uint64(bank.buses[b].cap); }
        if (busVoices[b]) { out.key("voices"); out.uint64(busVoices[b]); }
        if (device && b < device->busDb.size()) {   // the device's half: what it drives the bus to, what sounds on it
            if (const float db = r2(device->busDb[b]); db != r2(gameDb(bank, schedule, b))) { out.key("out"); out.float32(db); }
            if (b < device->busVoices.size() && device->busVoices[b]) { out.key("playing"); out.uint64(device->busVoices[b]); }
        }
        if (const auto* o = overrideOf(s.audioMix, bank.buses[b].name)) {
            out.key("mix");
            out.beginObject();
            if (o->db) { out.key("db"); out.float32(r2(*o->db)); }
            if (o->mute) { out.key("mute"); out.boolean(true); }
            if (o->solo) { out.key("solo"); out.boolean(true); }
            out.endObject();
        }
        out.endObject();
    }
    out.endArray();
    out.key("voices");
    out.beginObject();
    out.key("playing"); out.uint64(st.voices);
    out.key("dropped"); out.uint64(st.dropped);
    out.key("stolen"); out.uint64(st.stole);
    if (device) { out.key("device"); out.uint64(device->voices); }
    out.endObject();
    out.key("rays");
    out.beginObject();
    out.key("cast"); out.uint64(st.raysCast);
    out.key("capped"); out.uint64(st.raysCapped);
    out.endObject();
    if (device) {
        out.key("late"); out.uint64(device->late);
        if (device->mixMs) { out.key("mixMs"); out.float32(static_cast<float>(std::round(*device->mixMs * 1000.0) / 1000.0)); }
    }
    out.endObject();
}

} // namespace

bool methodAudioInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AudioInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const ecs::AudioSchedule* schedule = needSchedule(s, params, ctx);
    if (!schedule) return false;
    writeInfo(s, *schedule, out);
    return true;
}

bool methodAudioMix(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AudioMixParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const ecs::AudioSchedule* schedule = needSchedule(s, params, ctx);
    if (!schedule) return false;
    const AudioBank& bank = schedule->bank();
    const bool busEdit = p.db || p.mute || p.solo;
    if (busEdit && !p.bus) {
        ctx.error("E_COMMAND", params, "db, mute and solo set a bus: name it", "\"bus\": one of " + busNames(bank));
        return badParams(s);
    }
    if (p.bus && !busEdit) {
        ctx.push("bus");
        ctx.error("E_COMMAND", *ctx.doc.member(params, "bus"), "a bus alone changes nothing", "db, mute or solo with it");
        ctx.pop();
        return badParams(s);
    }
    if (p.bus && bank.bus(*p.bus) < 0) {
        ctx.push("bus");
        ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "bus"), "the bank has no bus '" + *p.bus + "'", "one of " + busNames(bank));
        ctx.pop();
        return false;
    }
    if (p.snapshot && !p.snapshot->empty() &&
        std::none_of(bank.snapshots.begin(), bank.snapshots.end(), [&](const SnapshotDesc& d) { return d.name == *p.snapshot; })) {
        std::string names;
        for (const SnapshotDesc& d : bank.snapshots) names += (names.empty() ? "" : ", ") + d.name;
        ctx.push("snapshot");
        ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "snapshot"), "the bank has no snapshot '" + *p.snapshot + "'",
                  "one of " + names + ", or \"\" for the schedule's");
        ctx.pop();
        return false;
    }
    // All checked: the overrides change together.
    AudioMixOverrides mix = p.clear ? AudioMixOverrides{} : s.audioMix;
    if (p.bus) {
        auto it = std::find_if(mix.buses.begin(), mix.buses.end(), [&](const auto& b) { return b.name == *p.bus; });
        if (it == mix.buses.end()) it = mix.buses.insert(mix.buses.end(), AudioMixOverrides::Bus{*p.bus, std::nullopt, false, false});
        if (p.db) it->db = *p.db;
        if (p.mute) it->mute = *p.mute;
        if (p.solo) it->solo = *p.solo;
        if (!it->db && !it->mute && !it->solo) mix.buses.erase(it);
    }
    if (p.snapshot) mix.snapshot = *p.snapshot;
    s.audioMix = std::move(mix);
    if (s.render) s.render->setAudioMix(s.audioMix);
    writeInfo(s, *schedule, out);
    return true;
}

} // namespace ohao::api
