#include "ecs/audio_schedule.hpp"

#include "audio/audio_bank.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/world.hpp"
#include "scene/asset/model.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace ohao::ecs {

namespace {

constexpr std::size_t kLogCapacity = 4096;
constexpr float kFaceSlack = 1e-5f;   // a listener exactly on a zone's face is inside it

// The graded occlusion's steps (OPEN ruling 12): 0, 1, 2 or more walls.
Occlusion grade(uint8_t walls) {
    if (walls == 0) return {};
    if (walls == 1) return {1, -8.0f, 1200.0};
    return {2, -14.0f, 700.0};
}

// The character a body belongs to (the entity or an ancestor: a living character holds a
// PlayerController, a dead enemy keeps Enemy and its body boxes), or null.
entt::entity characterOf(const entt::registry& reg, entt::entity e) {
    for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
        if (reg.all_of<PlayerController>(e) || reg.all_of<Enemy>(e)) return e;
        const auto* p = reg.try_get<Parent>(e);
        e = p ? p->entity : entt::null;
    }
    return entt::null;
}

glm::mat4 worldOf(const entt::registry& reg, entt::entity e) {
    if (const auto* wt = reg.try_get<WorldTransform>(e)) return wt->world;
    if (const auto* t = reg.try_get<Transform>(e)) return t->matrix();
    return glm::mat4(1.0f);
}

uint32_t holdSteps(uint32_t holdMs) { return (holdMs * 60u + 999u) / 1000u; }

// A zone's box in its entity's frame.
struct LocalBox {
    glm::vec3 lo{0.0f}, hi{0.0f};
};

bool inside(const entt::registry& reg, entt::entity e, const LocalBox& box, glm::vec3 p) {
    const glm::vec3 local = glm::vec3(glm::inverse(worldOf(reg, e)) * glm::vec4(p, 1.0f));
    return glm::all(glm::greaterThanEqual(local, box.lo - kFaceSlack)) && glm::all(glm::lessThanEqual(local, box.hi + kFaceSlack));
}

// The zone holding `p`: the highest priority, then the lowest id. `box` gives a zone's box (false:
// none, the zone is skipped); `known` whether its preset counts.
template <class BoxOf, class Known>
std::string pickZone(const World& world, glm::vec3 p, BoxOf box, Known known) {
    const auto& reg = world.registry();
    const ReverbZone* best = nullptr;
    uint64_t bestId = 0;
    for (auto [e, z] : reg.view<const ReverbZone>().each()) {
        if (!known(e, z)) continue;
        LocalBox b;
        if (!box(e, z, b) || !inside(reg, e, b, p)) continue;
        const uint64_t id = world.idOf(e);
        if (!best || z.priority > best->priority || (z.priority == best->priority && id < bestId)) {
            best = &z;
            bestId = id;
        }
    }
    return best ? best->preset : std::string();
}

bool sizedBox(const ReverbZone& z, LocalBox& b) {
    if (z.size == glm::vec3(0.0f)) return false;
    b.lo = -glm::abs(z.size);
    b.hi = glm::abs(z.size);
    return true;
}

struct AudioInstalled {
    AudioSchedule* system{nullptr};
};

} // namespace

std::optional<AudioListener> listenerAtStep(const World& world) {
    const auto& reg = world.registry();
    entt::entity player = entt::null;
    uint64_t playerId = std::numeric_limits<uint64_t>::max();
    for (auto [e, pc] : reg.view<const PlayerController>().each()) {
        const uint64_t id = world.idOf(e);
        if (pc.input && id < playerId) player = e, playerId = id;
    }
    if (player != entt::null) {
        const auto* t = reg.try_get<Transform>(player);
        if (!t) return std::nullopt;
        // Its head: its child with a Camera (the lowest id), at the step's pose.
        entt::entity head = entt::null;
        uint64_t headId = std::numeric_limits<uint64_t>::max();
        for (auto [e, cam, parent] : reg.view<const Camera, const Parent>().each()) {
            const uint64_t id = world.idOf(e);
            if (parent.entity == player && id < headId) head = e, headId = id;
        }
        glm::vec3 eye{0.0f, reg.get<PlayerController>(player).eyeHeight, 0.0f};
        if (head != entt::null)
            if (const auto* ht = reg.try_get<Transform>(head)) eye = ht->position;
        return AudioListener{t->position + t->rotation * eye, player};
    }
    entt::entity cam = entt::null;
    uint64_t camId = std::numeric_limits<uint64_t>::max();
    for (auto [e, c] : reg.view<const Camera>().each()) {
        const uint64_t id = world.idOf(e);
        if (c.active && id < camId) cam = e, camId = id;
    }
    if (cam == entt::null) return std::nullopt;
    return AudioListener{glm::vec3(worldOf(reg, cam)[3]), entt::null};
}

Occlusion occlusionBetween(const World& world, const AudioListener& listener, glm::vec3 source) {
    const PhysicsScene* physics = physicsScene(world);
    if (!physics) return {};
    const glm::vec3 toSource = source - listener.position;
    float remaining = glm::length(toSource);
    if (remaining < 1e-3f) return {};
    const glm::vec3 dir = toSource / remaining;
    glm::vec3 from = listener.position;
    entt::entity ignore = listener.body;
    uint8_t walls = 0;
    int hops = 0;
    // Each hit is passed through: a character (the source's own capsule, another enemy, a body box)
    // is skipped up to 4 times, a wall is counted; two walls is the last grade.
    for (int i = 0; i < 8 && remaining > 0.0f && walls < 2; ++i) {
        const auto hit = physics->raycastFrom(from, dir, remaining, ignore);
        if (!hit) break;
        const entt::entity character = characterOf(world.registry(), hit->entity);
        if (character != entt::null) {
            if (++hops > 4) break;
            ignore = character;
        } else {
            ++walls;
            ignore = hit->entity;
        }
        constexpr float kStepPast = 0.01f;
        from = hit->point + dir * kStepPast;
        remaining -= hit->dist + kStepPast;
    }
    return grade(walls);
}

std::string listenerZone(const World& world, glm::vec3 listener) {
    if (const AudioSchedule* s = audioSchedule(world)) return s->zoneAt(listener);
    return pickZone(
        world, listener, [](entt::entity, const ReverbZone& z, LocalBox& b) { return sizedBox(z, b); },
        [](entt::entity, const ReverbZone&) { return true; });
}

const char* audioCommandOpName(AudioCommand::Op op) noexcept {
    switch (op) {
        case AudioCommand::Op::Zone: return "zone";
        case AudioCommand::Op::Occlusion: return "occlusion";
    }
    return "?";
}

// ---- the schedule ------------------------------------------------------------------------------

struct AudioSchedule::Impl {
    World& world;
    AssetResolver& assets;

    // The world's bank (its AudioMix's, else the default).
    std::string bankPath;
    bool bankRead{false};
    AudioBank bank;

    // Zones fitted to their mesh (size 0), by entity: the box in the entity's frame, or none.
    struct Fitted {
        std::string key;   // the mesh it was fitted to
        bool ok{false};
        LocalBox box;
    };
    mutable std::map<entt::entity, Fitted> fitted;

    struct TrackedVoice {
        uint32_t id{0};
        glm::vec3 source{0.0f};
        uint64_t start{0}, end{0};
        uint32_t hold{0};   // steps
        bool occlude{false};
        Occlusion occl;
    };
    std::vector<TrackedVoice> voices;   // by id
    uint32_t nextVoice{0};

    std::string zone;   // the listener's zone's preset ("" before any)
    uint64_t steps{0};
    uint32_t raysThisStep{0};
    AudioScheduleStats stats;

    std::deque<AudioCommand> log;
    std::vector<AudioCommand> stepCommands;
    uint64_t nextSeq{0};

    std::vector<AudioWarning> warnings;
    std::set<std::pair<entt::entity, std::string>> warned;

    Impl(World& w, AssetResolver& a) : world(w), assets(a), bank(defaultBank()) {}

    void warn(entt::entity e, std::string code, std::string message) {
        if (!warned.emplace(e, code).second) return;
        OHAO_LOG(Warn, audio, "audio schedule: {}: {}", code, message);
        warnings.push_back({std::move(code), e, std::move(message)});
    }

    void push(AudioCommand c) {
        c.seq = nextSeq++;
        c.step = steps;
        stepCommands.push_back(c);
        log.push_back(std::move(c));
        if (log.size() > kLogCapacity) log.pop_front();
    }

    // The bank the world's AudioMix names (read once per path), else the default.
    void refreshBank() {
        const auto& reg = world.registry();
        std::string path;
        entt::entity mixEntity = entt::null;
        for (auto [e, mix] : reg.view<const AudioMix>().each()) {
            path = mix.asset.path;
            mixEntity = e;
            break;
        }
        if (bankRead && path == bankPath) return;
        bankRead = true;
        bankPath = path;
        warned.clear();   // presets are judged against the new bank
        bank = defaultBank();
        if (path.empty()) return;
        const auto file = assets.absolutePath(AssetRef{path, {}});
        std::ifstream f(file, std::ios::binary);
        if (file.empty() || !f) {
            warn(mixEntity, "W_AUDIO_BANK", "cannot read '" + path + "'; the default bank stands");
            return;
        }
        const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto parsed = parseAudioBank(json);
        if (!parsed) {
            const auto& e = parsed.error().front();
            warn(mixEntity, "W_AUDIO_BANK", "'" + path + "' " + e.path + ": " + e.message + "; the default bank stands");
            return;
        }
        bank = std::move(parsed).value();
    }

    // A zone's box in its frame: its size, else fitted to its mesh (cached per mesh).
    bool boxOf(entt::entity e, const ReverbZone& z, LocalBox& out) const {
        if (sizedBox(z, out)) return true;
        const auto& reg = world.registry();
        const ModelAsset* asset = nullptr;
        if (const auto* cm = reg.try_get<CollisionMesh>(e)) asset = &cm->source;
        else if (const auto* ms = reg.try_get<MeshSource>(e)) asset = std::get_if<ModelAsset>(&ms->source);
        const std::string key = asset ? asset->asset.path + "#" + asset->mesh : std::string();
        auto& f = fitted[e];
        if (f.key != key || key.empty()) {
            f = Fitted{key, false, {}};
            if (asset) {
                auto model = assets.loadModel(asset->asset, asset->mesh);
                if (model && model.value() && !model.value()->vertices.empty()) {
                    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
                    for (const auto& v : model.value()->vertices) lo = glm::min(lo, v.position), hi = glm::max(hi, v.position);
                    f.ok = true;
                    f.box = {lo, hi};
                }
            }
        }
        out = f.box;
        return f.ok;
    }

    bool known(entt::entity e, const ReverbZone& z) {
        if (bank.preset(z.preset)) return true;
        warn(e, "W_AUDIO_PRESET",
             "zone preset '" + z.preset + "' is not in the bank" + (bankPath.empty() ? std::string() : " (" + bankPath + ")") +
                 "; the zone counts as the default");
        return false;
    }

    std::string zoneAt(glm::vec3 p) const {
        return pickZone(
            world, p, [this](entt::entity e, const ReverbZone& z, LocalBox& b) { return boxOf(e, z, b); },
            [this](entt::entity, const ReverbZone& z) { return bank.preset(z.preset) != nullptr; });
    }

    // One cast within the step's cap; nullopt past it (counted).
    std::optional<Occlusion> cast(const AudioListener& ears, glm::vec3 source) {
        if (raysThisStep >= kRaysPerStep) {
            ++stats.raysCapped;
            return std::nullopt;
        }
        ++raysThisStep;
        ++stats.raysCast;
        return occlusionBetween(world, ears, source);
    }

    void step() {
        ++steps;
        stepCommands.clear();
        raysThisStep = 0;
        refreshBank();
        const auto ears = listenerAtStep(world);

        // 1. The listener's zone. Unknown presets warn here, once per zone.
        for (auto [e, z] : world.registry().view<const ReverbZone>().each()) (void)known(e, z);
        if (ears) {
            const std::string inZone = zoneAt(ears->position);
            const std::string preset = inZone.empty() ? bank.defaultZone : inZone;
            if (preset != zone) {
                zone = preset;
                AudioCommand c;
                c.op = AudioCommand::Op::Zone;
                c.zone = preset;
                if (const ReverbDesc* p = bank.preset(preset)) c.bed = p->bed;
                c.blendSteps = kZoneBlendSteps;
                push(std::move(c));
            }
        }

        // 2. Voices that have ended let go; those still holding are re-cast every 6 steps.
        std::erase_if(voices, [&](const TrackedVoice& v) { return steps >= v.end; });
        stats.voices = static_cast<uint32_t>(voices.size());
        if (!ears) return;
        for (TrackedVoice& v : voices) {
            if (!v.occlude || v.hold <= kRecastSteps || steps <= v.start || (steps - v.start) % kRecastSteps != 0) continue;
            const auto now = cast(*ears, v.source);
            if (!now || now->walls == v.occl.walls) continue;   // capped: the last answer stands
            v.occl = *now;
            AudioCommand c;
            c.op = AudioCommand::Op::Occlusion;
            c.voice = v.id;
            c.occl = v.occl;
            c.blendSteps = kOcclusionBlendSteps;
            push(std::move(c));
        }
    }

    Voice track(glm::vec3 source, uint32_t holdMs, bool occlude) {
        TrackedVoice v;
        v.id = ++nextVoice;
        v.source = source;
        v.start = steps;
        v.hold = holdSteps(holdMs);
        v.end = v.start + v.hold;
        v.occlude = occlude;
        Voice out{v.id, {}, false};
        if (occlude)
            if (const auto ears = listenerAtStep(world)) {
                if (const auto o = cast(*ears, source)) v.occl = out.occl = *o;
                else out.capped = true;
            }
        voices.push_back(v);
        stats.voices = static_cast<uint32_t>(voices.size());
        return out;
    }
};

AudioSchedule::AudioSchedule(World& world, AssetResolver& assets) : m(std::make_unique<Impl>(world, assets)) {}
AudioSchedule::~AudioSchedule() = default;

void AudioSchedule::step() { m->step(); }
AudioSchedule::Voice AudioSchedule::trackVoice(glm::vec3 source, uint32_t holdMs, bool occlude) {
    return m->track(source, holdMs, occlude);
}
std::span<const AudioCommand> AudioSchedule::stepCommands() const noexcept { return m->stepCommands; }
uint64_t AudioSchedule::nextSeq() const noexcept { return m->nextSeq; }
uint64_t AudioSchedule::steps() const noexcept { return m->steps; }
const std::string& AudioSchedule::zone() const noexcept { return m->zone; }
AudioScheduleStats AudioSchedule::stats() const noexcept { return m->stats; }
const std::vector<AudioWarning>& AudioSchedule::warnings() const noexcept { return m->warnings; }
std::string AudioSchedule::zoneAt(glm::vec3 p) const { return m->zoneAt(p); }

std::vector<AudioCommand> AudioSchedule::commandsSince(uint64_t since, std::size_t max, uint64_t& dropped) const {
    std::vector<AudioCommand> out;
    const uint64_t oldest = m->log.empty() ? m->nextSeq : m->log.front().seq;
    dropped = since < oldest ? oldest - since : 0;
    for (const AudioCommand& c : m->log) {
        if (c.seq < since) continue;
        if (out.size() >= max) break;
        out.push_back(c);
    }
    return out;
}

AudioSchedule& installAudio(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<AudioInstalled>()) return *installed->system;
    auto system = std::make_shared<AudioSchedule>(world, assets);
    reg.ctx().emplace<AudioInstalled>(AudioInstalled{system.get()});
    world.addSimulate("audio_schedule", [system](World&, float) { system->step(); });
    return *system;
}

AudioSchedule* audioSchedule(World& world) {
    const auto* installed = world.registry().ctx().find<AudioInstalled>();
    return installed ? installed->system : nullptr;
}

const AudioSchedule* audioSchedule(const World& world) {
    const auto* installed = world.registry().ctx().find<AudioInstalled>();
    return installed ? installed->system : nullptr;
}

std::span<const AudioCommand> audioCommands(const World& world) {
    const AudioSchedule* s = audioSchedule(world);
    return s ? s->stepCommands() : std::span<const AudioCommand>{};
}

} // namespace ohao::ecs
