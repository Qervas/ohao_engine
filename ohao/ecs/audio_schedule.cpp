#include "ecs/audio_schedule.hpp"

#include "anim/anim_graph.hpp"
#include "audio/audio_bank.hpp"
#include "core/log.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/world.hpp"
#include "asset/model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

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

// ---- the listener, occlusion, zones ------------------------------------------------------------

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
        return AudioListener{t->position + t->rotation * eye, player, playerId};
    }
    entt::entity cam = entt::null;
    uint64_t camId = std::numeric_limits<uint64_t>::max();
    for (auto [e, c] : reg.view<const Camera>().each()) {
        const uint64_t id = world.idOf(e);
        if (c.active && id < camId) cam = e, camId = id;
    }
    if (cam == entt::null) return std::nullopt;
    return AudioListener{glm::vec3(worldOf(reg, cam)[3]), entt::null, 0};
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
        case AudioCommand::Op::Play: return "play";
        case AudioCommand::Op::Stop: return "stop";
        case AudioCommand::Op::Bus: return "bus";
        case AudioCommand::Op::Snapshot: return "snapshot";
        case AudioCommand::Op::Zone: return "zone";
        case AudioCommand::Op::Occlusion: return "occlusion";
        case AudioCommand::Op::StopWorld: return "stop_world";
    }
    return "?";
}

const char* audioOutcomeName(AudioCommand::Outcome o) noexcept {
    switch (o) {
        case AudioCommand::Outcome::Played: return "played";
        case AudioCommand::Outcome::Stole: return "stole";
        case AudioCommand::Outcome::Dropped: return "dropped";
    }
    return "?";
}

std::string describeAudioCommand(const AudioCommand& c, const AudioBank& bank) {
    const auto busName = [&](uint8_t b) { return b < bank.buses.size() ? bank.buses[b].name : std::to_string(b); };
    char text[512];
    const auto step = static_cast<unsigned long long>(c.step);
    switch (c.op) {
        case AudioCommand::Op::Play: {
            const std::string sound = c.sound < bank.sounds.size() ? bank.sounds[c.sound].name : std::to_string(c.sound);
            char where[96];
            if (c.spatial) std::snprintf(where, sizeof where, "at (%.4f, %.4f, %.4f)", c.pos.x, c.pos.y, c.pos.z);
            else std::snprintf(where, sizeof where, "at the ear");
            std::snprintf(text, sizeof text, "play step %llu %s v%u bus %s prio %u %s gain %.4f send %.3f walls %u %s voice %u victim %u source %016llx",
                          step, sound.c_str(), c.variant, busName(c.bus).c_str(), c.priority, where, c.gain, c.send, c.occl.walls,
                          audioOutcomeName(c.outcome), c.voice, c.victim, static_cast<unsigned long long>(c.source));
            break;
        }
        case AudioCommand::Op::Stop: std::snprintf(text, sizeof text, "stop step %llu voice %u", step, c.voice); break;
        case AudioCommand::Op::Bus:
            std::snprintf(text, sizeof text, "bus step %llu %s %.2f dB over %u", step, busName(c.bus).c_str(), c.db, c.blendSteps);
            break;
        case AudioCommand::Op::Snapshot:
            std::snprintf(text, sizeof text, "snapshot step %llu %s over %u", step, c.snapshot.c_str(), c.blendSteps);
            break;
        case AudioCommand::Op::Zone:
            std::snprintf(text, sizeof text, "zone step %llu %s bed '%s' over %u", step, c.zone.c_str(), c.bed.c_str(), c.blendSteps);
            break;
        case AudioCommand::Op::Occlusion:
            std::snprintf(text, sizeof text, "occlusion step %llu voice %u walls %u %.1f dB %.0f Hz over %u", step, c.voice, c.occl.walls,
                          c.occl.db, c.occl.lowPassHz, c.blendSteps);
            break;
        case AudioCommand::Op::StopWorld: std::snprintf(text, sizeof text, "stop_world step %llu over %u", step, c.blendSteps); break;
    }
    std::string out = text;
    if (c.resync) out += " resync";
    return out;
}

// ---- the rules -----------------------------------------------------------------------------------

std::vector<std::string> combatSoundNames(const CombatEvent& e, uint64_t playerId) {
    using K = CombatEvent::Kind;
    const bool byPlayer = playerId != 0 && e.sourceId == playerId;
    switch (e.kind) {
        case K::Shot: return {byPlayer ? "fire" : "enemy_fire"};
        case K::Hit: return {"impact_" + (e.surface.empty() ? std::string("default") : e.surface)};
        case K::Death: return {"death"};
        case K::Explode: return {"explode"};
        case K::Reload:
            if (!byPlayer) return {};
            if (e.amount == 0.0f) return {"empty", "reload"};
            return {"reload"};
        default: return {};
    }
}

bool combatSoundAtEar(std::string_view name) noexcept {
    return name == "fire" || name == "reload" || name == "empty";
}

std::vector<std::string> animSoundNames(const AnimEvent& e) {
    if (e.name == "mag_out" || e.name == "mag_in" || e.name == "bolt" || e.name == "body_fall") return {e.name};
    if (e.name == "foot_l" || e.name == "foot_r") return {"footstep"};
    return {};
}

uint64_t animSoundOwner(const World& world, uint64_t entityId, uint64_t playerId) {
    if (playerId == 0) return entityId;
    const auto& reg = world.registry();
    entt::entity at = world.findById(entityId);
    for (int depth = 0; depth < 64 && at != entt::null && reg.valid(at); ++depth) {
        if (world.idOf(at) == playerId) return playerId;
        const auto* p = reg.try_get<Parent>(at);
        at = p ? p->entity : entt::null;
    }
    return entityId;
}

float footstepScale(bool landing, float speed) noexcept {
    return landing ? std::clamp(speed / 6.0f, 0.4f, 1.0f) : std::clamp(0.25f + 0.1f * speed, 0.2f, 1.0f);
}

std::string footstepSound(const AudioBank& bank, std::string_view surface) {
    const std::string own = "footstep_" + std::string(surface.empty() ? std::string_view("default") : surface);
    if (bank.sound(own) >= 0) return own;
    if (bank.sound("footstep_default") >= 0) return "footstep_default";
    return {};
}

float distanceGain(float distance, const ShapeDesc& shape) noexcept {
    if (shape.minDistance >= shape.maxDistance) return 1.0f;
    const float d = std::clamp(distance, shape.minDistance, shape.maxDistance);
    return shape.minDistance / (shape.minDistance + shape.rolloff * (d - shape.minDistance));
}

// ---- the schedule ------------------------------------------------------------------------------

struct AudioSchedule::Impl {
    World& world;
    AssetResolver& assets;

    // The world's bank (its AudioMix's, else the default; or the one set).
    std::string bankPath;
    bool bankRead{false}, bankSet{false};
    AudioBank bank;
    uint64_t bankVersion{0};
    std::vector<bool> worldBus;   // per bus: under `world` (a restart stops them)
    int returnBus{-1};

    // Zones fitted to their mesh (size 0), by entity: the box in the entity's frame, or none.
    struct Fitted {
        std::string key;   // the mesh it was fitted to
        bool ok{false};
        LocalBox box;
    };
    mutable std::map<entt::entity, Fitted> fitted;
    std::map<std::string, bool> footGraphs;   // a graph path: has foot_l / foot_r events

    // The director's state: what a snapshot carries.
    struct Voice {
        uint32_t id{0};
        uint64_t seq{0};
        uint16_t sound{0};
        uint8_t bus{0}, priority{0};
        float audible{0.0f};
        uint64_t start{0}, end{0};
        uint32_t hold{0};
        glm::vec3 source{0.0f};
        bool occlude{false};
        bool tracked{false};   // trackVoice's: no play command, outside the caps
        Occlusion occl;
    };
    struct State {
        std::vector<Voice> voices;   // in id order
        uint32_t nextVoice{0};
        std::map<uint16_t, uint32_t> robin;   // per sound: the next variant
        std::vector<uint64_t> duckUntil;      // per duck rule: held while steps < this
        std::vector<float> busDuck;           // per bus: the duck level decided
        std::vector<uint32_t> busRelease;     // per bus: the release of the duck that set it
        std::string zone, snapshot;
        uint64_t steps{0};
    } s;

    // What the device was last told (never snapshotted): after a restore, what differs is resent.
    struct Heard {
        std::string zone, snapshot;
        std::vector<float> busDuck;
    } heard;
    bool restored{false};
    bool trace{false};
    uint32_t lastSnapshotBlend{0};   // the last snapshot command's blend (audio.info; never snapshotted)

    std::optional<AudioListener> ears;   // the step's listener (refreshed by each step and each play)
    uint32_t raysThisStep{0};
    double lastStepMs{0.0};
    AudioScheduleStats stats;

    std::deque<AudioCommand> log;
    std::vector<AudioCommand> stepCommands;
    uint64_t nextSeq{0};

    std::vector<AudioWarning> warnings;
    std::set<std::pair<entt::entity, std::string>> warned;

    Impl(World& w, AssetResolver& a) : world(w), assets(a), bank(defaultBank()) { shapeState(); }

    void warn(entt::entity e, std::string code, std::string message) {
        if (!warned.emplace(e, code).second) return;
        OHAO_LOG(Warn, audio, "audio schedule: {}: {}", code, message);
        warnings.push_back({std::move(code), e, std::move(message)});
    }

    void push(AudioCommand c) {
        c.seq = nextSeq++;
        c.step = s.steps;
        if (c.op == AudioCommand::Op::Snapshot) lastSnapshotBlend = c.blendSteps;
        if (trace) OHAO_LOG(Info, audio, "schedule: {}", describeAudioCommand(c, bank));
        stepCommands.push_back(c);
        log.push_back(std::move(c));
        if (log.size() > kLogCapacity) log.pop_front();
    }

    // The per-bus and per-duck vectors sized to the bank (kept values where they fit).
    void shapeState() {
        s.duckUntil.resize(bank.ducks.size(), 0);
        s.busDuck.resize(bank.buses.size(), 0.0f);
        s.busRelease.resize(bank.buses.size(), 0);
        heard.busDuck.resize(bank.buses.size(), 0.0f);
        worldBus.assign(bank.buses.size(), false);
        const int worldRoot = bank.bus("world");
        returnBus = -1;
        for (std::size_t b = 0; b < bank.buses.size(); ++b) {
            if (bank.buses[b].isReturn && returnBus < 0) returnBus = static_cast<int>(b);
            if (worldRoot < 0) {
                worldBus[b] = !bank.buses[b].isReturn;
                continue;
            }
            for (int at = static_cast<int>(b); at >= 0; at = bank.parentOf(at))
                if (at == worldRoot) worldBus[b] = true;
        }
    }

    void bankChanged() {
        ++bankVersion;
        warned.clear();   // presets are judged against the new bank
        shapeState();
    }

    // The bank the world's AudioMix names (read once per path), else the default.
    void refreshBank() {
        if (bankSet) return;
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
        bank = defaultBank();
        bankChanged();
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
        bankChanged();
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
    std::optional<Occlusion> cast(const AudioListener& at, glm::vec3 source) {
        if (raysThisStep >= kRaysPerStep) {
            ++stats.raysCapped;
            return std::nullopt;
        }
        ++raysThisStep;
        ++stats.raysCast;
        return occlusionBetween(world, at, source);
    }

    // Whether a character's graph plays its footsteps from clip events (foot_l, foot_r).
    bool footFromClips(entt::entity character) {
        const auto* animator = world.registry().try_get<Animator>(character);
        if (!animator || animator->graph.path.empty()) return false;
        const auto it = footGraphs.find(animator->graph.path);
        if (it != footGraphs.end()) return it->second;
        bool has = false;
        if (AnimationSystem* anim = animationSystem(world))
            if (const auto g = anim->graph(animator->graph.path))
                for (const auto& c : g->clips)
                    for (const auto& ev : c.clip.events) has = has || ev.name == "foot_l" || ev.name == "foot_r";
        footGraphs[animator->graph.path] = has;
        return has;
    }

    // ---- the voice table ------------------------------------------------------------------

    // Whether bus `b` is `under` or below it.
    bool below(int b, int under) const {
        for (int at = b; at >= 0; at = bank.parentOf(at))
            if (at == under) return true;
        return false;
    }

    // The victims a new voice on `bus` (`priority`, `audible`) needs over the bus caps (its bus and
    // its ancestors with one) and the global cap, one per cap reached; false when one cap has none (the
    // new voice is dropped). Candidates in a cap's scope: a lower priority, else an equal one quieter;
    // the lowest priority goes, then the quietest, then the oldest.
    bool victims(int bus, uint8_t priority, float audible, std::vector<std::size_t>& out) const {
        std::vector<bool> gone(s.voices.size(), false);
        const auto over = [&](int scope) {   // scope -1: every bus
            uint32_t cap = scope < 0 ? bank.voices : bank.buses[static_cast<std::size_t>(scope)].cap;
            if (cap == 0) return false;
            uint32_t n = 0;
            for (std::size_t i = 0; i < s.voices.size(); ++i)
                if (!gone[i] && !s.voices[i].tracked && (scope < 0 || below(s.voices[i].bus, scope))) ++n;
            return n >= cap;
        };
        std::vector<int> scopes;
        for (int at = bus; at >= 0; at = bank.parentOf(at)) scopes.push_back(at);
        scopes.push_back(-1);
        for (const int scope : scopes) {
            while (over(scope)) {
                std::size_t pick = s.voices.size();
                for (std::size_t i = 0; i < s.voices.size(); ++i) {
                    const Voice& v = s.voices[i];
                    if (gone[i] || v.tracked || (scope >= 0 && !below(v.bus, scope))) continue;
                    const bool candidate = v.priority < priority || (v.priority == priority && v.audible < audible);
                    if (!candidate) continue;
                    if (pick == s.voices.size()) {
                        pick = i;
                        continue;
                    }
                    const Voice& p = s.voices[pick];
                    if (v.priority < p.priority || (v.priority == p.priority && (v.audible < p.audible || (v.audible == p.audible && v.seq < p.seq))))
                        pick = i;
                }
                if (pick == s.voices.size()) return false;
                gone[pick] = true;
                out.push_back(pick);
            }
        }
        return true;
    }

    std::optional<AudioCommand> playRequest(const SoundRequest& req) {
        const int si = bank.sound(req.sound);
        if (si < 0) return std::nullopt;
        const SoundDesc& desc = bank.sounds[static_cast<std::size_t>(si)];
        const int bus = std::max(0, bank.bus(desc.bus));
        const ShapeDesc* shape = desc.shape == "ear" ? nullptr : bank.shape(desc.shape);
        const bool ear = req.ear || shape == nullptr;
        const bool own = ears && ears->playerId != 0 && req.owner == ears->playerId;

        AudioCommand c;
        c.op = AudioCommand::Op::Play;
        c.sound = static_cast<uint16_t>(si);
        c.bus = static_cast<uint8_t>(bus);
        c.priority = desc.priority;
        c.spatial = !ear;
        c.pos = ear ? (ears ? ears->position : glm::vec3(0.0f)) : req.at;
        c.gain = desc.volume * req.scale;
        c.send = desc.send;
        c.source = req.owner;
        c.variant = req.variant ? *req.variant : s.robin[c.sound]++;

        const bool occlude = desc.occlude && !own && !ear;
        if (occlude && ears)
            if (const auto o = cast(*ears, req.at)) c.occl = *o;
        const float distance = ears && !ear ? glm::length(req.at - ears->position) : 0.0f;
        const float audible = c.gain * (shape && !ear ? distanceGain(distance, *shape) : 1.0f) * dbToGain(c.occl.db);

        c.voice = ++s.nextVoice;
        std::vector<std::size_t> taken;
        if (!victims(bus, desc.priority, audible, taken)) {
            c.outcome = AudioCommand::Outcome::Dropped;
            ++stats.dropped;
            push(c);
            return c;
        }
        // The victims stop first (a 2 ms fade in the device), then the new voice starts.
        std::sort(taken.begin(), taken.end());
        for (const std::size_t i : taken) {
            AudioCommand stop;
            stop.op = AudioCommand::Op::Stop;
            stop.voice = s.voices[i].id;
            push(stop);
            c.victim = s.voices[i].id;
        }
        for (auto it = taken.rbegin(); it != taken.rend(); ++it) s.voices.erase(s.voices.begin() + static_cast<std::ptrdiff_t>(*it));
        c.outcome = taken.empty() ? AudioCommand::Outcome::Played : AudioCommand::Outcome::Stole;
        ++(taken.empty() ? stats.played : stats.stole);
        push(c);

        Voice v;
        v.id = c.voice;
        v.seq = log.back().seq;
        v.sound = c.sound;
        v.bus = c.bus;
        v.priority = c.priority;
        v.audible = audible;
        v.start = s.steps;
        v.hold = holdSteps(desc.holdMs);
        v.end = v.start + v.hold;
        v.source = req.at;
        v.occlude = occlude;
        v.occl = c.occl;
        s.voices.push_back(v);
        // Ducks this sound triggers hold while it holds its voice.
        for (std::size_t i = 0; i < bank.ducks.size(); ++i)
            if (bank.ducks[i].on == desc.name) s.duckUntil[i] = std::max(s.duckUntil[i], s.steps + v.hold);
        stats.voices = static_cast<uint32_t>(s.voices.size());
        return c;
    }

    // Each bus's duck: the deepest rule holding it; a change writes `bus` (its attack going down, the
    // release of the one that held it going up).
    void updateDucks() {
        for (std::size_t b = 0; b < bank.buses.size(); ++b) {
            float target = 0.0f;
            uint32_t attack = 0, release = 0;
            for (std::size_t i = 0; i < bank.ducks.size(); ++i) {
                const DuckDesc& d = bank.ducks[i];
                if (bank.bus(d.bus) != static_cast<int>(b) || s.duckUntil[i] <= s.steps) continue;
                if (d.db < target) target = d.db, attack = d.attackSteps, release = d.releaseSteps;
            }
            if (target == s.busDuck[b]) continue;
            AudioCommand c;
            c.op = AudioCommand::Op::Bus;
            c.bus = static_cast<uint8_t>(b);
            c.db = target;
            c.blendSteps = target < s.busDuck[b] ? attack : s.busRelease[b];
            s.busDuck[b] = target;
            s.busRelease[b] = release;
            heard.busDuck[b] = target;
            push(c);
        }
    }

    // ---- the step ---------------------------------------------------------------------------

    std::string snapshotTarget() const {
        std::string want = "default";
        if (const CombatSystem* combat = combatSystem(world)) {
            const RoundInfo round = combat->round();
            if (round.phase == RoundPhase::Dead) want = "dead";
            else if (round.phase == RoundPhase::Won) want = "won";
        }
        const auto has = [&](const std::string& n) {
            return std::any_of(bank.snapshots.begin(), bank.snapshots.end(), [&](const SnapshotDesc& d) { return d.name == n; });
        };
        return has(want) ? want : "default";
    }

    const SnapshotDesc* snapshotDesc(const std::string& name) const {
        for (const auto& d : bank.snapshots)
            if (d.name == name) return &d;
        return nullptr;
    }

    // The listener's zone at the step: the zone holding it, else the bank's default ("" without a
    // listener: unchanged).
    std::string zoneTarget() const {
        if (!ears) return s.zone;
        const std::string inZone = zoneAt(ears->position);
        return inZone.empty() ? bank.defaultZone : inZone;
    }

    // A snapshot change's blend: the one entered's in-steps, or on the way back to the default the
    // out-steps of the one the device leaves.
    uint32_t snapshotBlend(const std::string& to, const std::string& from) const {
        if (to != "default") {
            const SnapshotDesc* d = snapshotDesc(to);
            return d ? d->inSteps : 0;
        }
        const SnapshotDesc* left = from.empty() ? nullptr : snapshotDesc(from);
        return left ? left->outSteps : 0;
    }

    // After a restore (a round restart's too): the device is brought from what it heard to where this
    // step's mix will be, by `resync` commands; the step then writes the commands it always would, and
    // the device takes a zone or snapshot it is already in as nothing.
    void resync() {
        if (const std::string zone = zoneTarget(); !zone.empty() && zone != heard.zone && !heard.zone.empty()) {
            AudioCommand c;
            c.op = AudioCommand::Op::Zone;
            c.zone = zone;
            if (const ReverbDesc* p = bank.preset(zone)) c.bed = p->bed;
            c.blendSteps = kZoneBlendSteps;
            c.resync = true;
            heard.zone = zone;
            push(c);
        }
        if (const std::string snap = snapshotTarget(); snap != heard.snapshot && !heard.snapshot.empty()) {
            AudioCommand c;
            c.op = AudioCommand::Op::Snapshot;
            c.snapshot = snap;
            c.blendSteps = snapshotBlend(snap, heard.snapshot);
            c.resync = true;
            heard.snapshot = snap;
            push(c);
        }
        for (std::size_t b = 0; b < s.busDuck.size() && b < heard.busDuck.size(); ++b) {
            if (s.busDuck[b] == heard.busDuck[b]) continue;
            AudioCommand c;
            c.op = AudioCommand::Op::Bus;
            c.bus = static_cast<uint8_t>(b);
            c.db = s.busDuck[b];
            c.blendSteps = kOcclusionBlendSteps;
            c.resync = true;
            heard.busDuck[b] = s.busDuck[b];
            push(c);
        }
    }

    void step() {
        ++s.steps;
        stepCommands.clear();
        raysThisStep = 0;
        refreshBank();
        ears = listenerAtStep(world);
        const CombatSystem* combat = combatSystem(world);
        const std::span<const CombatEvent> combatEvents = combat ? combat->stepEvents() : std::span<const CombatEvent>{};
        // A round restart in this step: what came before it was undone (the restore that put this
        // state back happened inside the combat step).
        std::size_t afterRestart = 0;
        bool restart = false;
        for (std::size_t i = 0; i < combatEvents.size(); ++i)
            if (combatEvents[i].kind == CombatEvent::Kind::Restart) afterRestart = i + 1, restart = true;

        // 1. The restart, or any restore: the device's world voices fade; the restored mix is resent.
        if (restart || restored) {
            AudioCommand c;
            c.op = AudioCommand::Op::StopWorld;
            c.blendSteps = kStopWorldSteps;
            c.resync = !restart;
            push(c);
        }
        if (restored) {
            resync();
            restored = false;
        }

        // 2. The listener's zone. Unknown presets warn here, once per zone.
        for (auto [e, z] : world.registry().view<const ReverbZone>().each()) (void)known(e, z);
        if (const std::string preset = zoneTarget(); ears && preset != s.zone) {
            s.zone = preset;
            heard.zone = preset;
            AudioCommand c;
            c.op = AudioCommand::Op::Zone;
            c.zone = preset;
            if (const ReverbDesc* p = bank.preset(preset)) c.bed = p->bed;
            c.blendSteps = kZoneBlendSteps;
            push(std::move(c));
        }

        // 3. The snapshot from the round.
        if (const std::string want = snapshotTarget(); want != s.snapshot) {
            AudioCommand c;
            c.op = AudioCommand::Op::Snapshot;
            c.snapshot = want;
            c.blendSteps = snapshotBlend(want, s.snapshot);
            s.snapshot = want;
            heard.snapshot = want;
            push(std::move(c));
        }

        // 4. Voices that have ended let go; then the step's sounds.
        std::erase_if(s.voices, [&](const Voice& v) { return s.steps >= v.end; });
        const uint64_t playerId = ears ? ears->playerId : 0;
        const auto& reg = world.registry();
        if (!restart)   // physics stepped the timeline the restart undid
            if (const PhysicsScene* physics = physicsScene(world))
                for (const PhysicsEvent& e : physics->stepEvents()) {
                    const bool land = e.kind == PhysicsEvent::Kind::Land;
                    if (e.kind != PhysicsEvent::Kind::Footstep && !land) continue;
                    if (!land) {
                        const entt::entity who = world.findById(e.aId);
                        if (who != entt::null && footFromClips(who)) continue;   // its clip's feet play its steps
                    }
                    const std::string sound = footstepSound(bank, e.surface);
                    if (!sound.empty()) (void)playRequest({sound, e.aId, e.point, false, footstepScale(land, e.speed)});
                }
        for (std::size_t i = afterRestart; i < combatEvents.size(); ++i) {
            const CombatEvent& e = combatEvents[i];
            // A grenade is a world source, not the thrower's own sound: occluded like an enemy's.
            const uint64_t owner = e.kind == CombatEvent::Kind::Explode ? 0 : e.sourceId;
            for (std::string name : combatSoundNames(e, playerId)) {
                if (name.rfind("impact_", 0) == 0 && bank.sound(name) < 0) name = "impact_default";
                (void)playRequest({name, owner, e.point, combatSoundAtEar(name)});
            }
        }
        if (const AnimationSystem* anim = animationSystem(world))
            for (const AnimEvent& e : anim->stepEvents())
                for (const std::string& name : animSoundNames(e)) {
                    const entt::entity who = world.findById(e.entity);
                    if (who == entt::null) continue;
                    const uint64_t owner = animSoundOwner(world, e.entity, playerId);
                    glm::vec3 at = glm::vec3(worldOf(reg, who)[3]);
                    if (name == "footstep") {
                        const entt::entity character = characterOf(reg, who);
                        const PhysicsScene* physics = physicsScene(world);
                        const auto info = physics && character != entt::null ? physics->playerInfo(character) : std::nullopt;
                        const glm::vec3 v = info ? info->velocity : glm::vec3(0.0f);
                        const std::string sound = footstepSound(bank, info ? info->surface : std::string());
                        if (!sound.empty())
                            (void)playRequest({sound, owner, at, false, footstepScale(false, std::sqrt(v.x * v.x + v.z * v.z))});
                        continue;
                    }
                    at.y += name == "body_fall" ? 0.2f : 1.1f;   // the floor, or the hands at the chest
                    (void)playRequest({name, owner, at, playerId != 0 && owner == playerId});
                }

        // 5. Ducks.
        updateDucks();

        // 6. Held voices re-cast every 6 steps.
        if (ears)
            for (Voice& v : s.voices) {
                if (!v.occlude || v.hold <= kRecastSteps || s.steps <= v.start || (s.steps - v.start) % kRecastSteps != 0) continue;
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
        stats.voices = static_cast<uint32_t>(s.voices.size());
    }

    Voice track(glm::vec3 source, uint32_t holdMs, bool occlude, AudioSchedule::Voice& out) {
        Voice v;
        v.id = ++s.nextVoice;
        v.source = source;
        v.start = s.steps;
        v.hold = holdSteps(holdMs);
        v.end = v.start + v.hold;
        v.occlude = occlude;
        v.tracked = true;
        out = AudioSchedule::Voice{v.id, {}, false};
        if (occlude)
            if (const auto at = listenerAtStep(world)) {
                if (const auto o = cast(*at, source)) v.occl = out.occl = *o;
                else out.capped = true;
            }
        s.voices.push_back(v);
        stats.voices = static_cast<uint32_t>(s.voices.size());
        return v;
    }
};

AudioSchedule::AudioSchedule(World& world, AssetResolver& assets) : m(std::make_unique<Impl>(world, assets)) {}
AudioSchedule::~AudioSchedule() = default;

void AudioSchedule::step() {
    const auto t0 = std::chrono::steady_clock::now();
    m->step();
    m->lastStepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double AudioSchedule::lastStepMs() const noexcept { return m->lastStepMs; }

std::optional<AudioCommand> AudioSchedule::play(const SoundRequest& request) {
    m->refreshBank();
    m->ears = listenerAtStep(m->world);
    auto c = m->playRequest(request);
    m->updateDucks();
    return c;
}

AudioSchedule::Voice AudioSchedule::trackVoice(glm::vec3 source, uint32_t holdMs, bool occlude) {
    Voice out;
    (void)m->track(source, holdMs, occlude, out);
    return out;
}

void AudioSchedule::setTrace(bool on) noexcept { m->trace = on; }

void AudioSchedule::setBank(const AudioBank& bank) {
    m->bankSet = true;
    m->bank = bank;
    m->bankChanged();
}
const AudioBank& AudioSchedule::bank() const noexcept { return m->bank; }
uint64_t AudioSchedule::bankVersion() const noexcept { return m->bankVersion; }
std::span<const AudioCommand> AudioSchedule::stepCommands() const noexcept { return m->stepCommands; }
uint64_t AudioSchedule::nextSeq() const noexcept { return m->nextSeq; }
uint64_t AudioSchedule::steps() const noexcept { return m->s.steps; }
const std::string& AudioSchedule::zone() const noexcept { return m->s.zone; }
const std::string& AudioSchedule::snapshot() const noexcept { return m->s.snapshot; }
AudioScheduleStats AudioSchedule::stats() const noexcept { return m->stats; }
float AudioSchedule::busDuckDb(std::size_t bus) const noexcept {
    return bus < m->s.busDuck.size() ? m->s.busDuck[bus] : 0.0f;
}
uint32_t AudioSchedule::snapshotBlendSteps() const noexcept { return m->lastSnapshotBlend; }
const std::vector<AudioWarning>& AudioSchedule::warnings() const noexcept { return m->warnings; }
std::string AudioSchedule::zoneAt(glm::vec3 p) const { return m->zoneAt(p); }

std::vector<ScheduledVoice> AudioSchedule::voices() const {
    std::vector<ScheduledVoice> out;
    for (const auto& v : m->s.voices)
        if (!v.tracked) out.push_back({v.id, v.seq, v.sound, v.bus, v.priority, v.audible, v.start, v.end});
    return out;
}

std::shared_ptr<const void> AudioSchedule::saveState() const { return std::make_shared<const Impl::State>(m->s); }

void AudioSchedule::restoreState(const std::shared_ptr<const void>& state) {
    if (!state) return;
    m->s = *static_cast<const Impl::State*>(state.get());
    m->shapeState();
    m->restored = true;
}

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
    // Snapshots carry the schedule's state (restored after physics', ecs::kSnapshotOrder).
    AudioSchedule* raw = system.get();
    addSnapshotHook(reg, SnapshotHook{"audio_schedule", [raw] { return raw->saveState(); },
                                      [raw](const std::shared_ptr<const void>& s) {
                                          if (s) raw->restoreState(s);
                                      }});
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
