#include "ecs/audio_schedule.hpp"

#include "anim/anim_graph.hpp"
#include "audio/audio_bank.hpp"
#include "core/log.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/player.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/world.hpp"
#include "asset/model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace ohao::ecs {

namespace {

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

// Durations in data (ms) as steps of the World's dt: a hold rounds up (it lasts at least its time), a ramp or a period
// to the nearest step (at least one). In double, with 1e-6 of slack, so a whole number of steps stays one.
uint32_t holdSteps(uint32_t holdMs, float dt) {
    return static_cast<uint32_t>(std::ceil(holdMs / (1000.0 * dt) - 1e-6));
}
uint32_t periodSteps(uint32_t ms, float dt) {
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(ms / (1000.0 * dt))));
}

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
    const entt::entity player = playerOf(world);   // the device's player hears
    const uint64_t playerId = player != entt::null ? world.idOf(player) : 0;
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

// ---- the rules -----------------------------------------------------------------------------------

std::vector<std::string> animSoundNames(const ClipEvent& e) {
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
    } s;

    // What the device was last told (never snapshotted): after a restore, what differs is resent.
    struct Heard {
        std::string zone, snapshot;
        std::vector<float> busDuck;
    } heard;
    bool restored{false};
    uint32_t lastSnapshotBlend{0};   // the last snapshot command's blend (audio.info; never snapshotted)

    std::optional<AudioListener> ears;   // the step's listener (refreshed by each step and each play)
    uint32_t raysThisStep{0};
    double lastStepMs{0.0};
    AudioScheduleStats stats;

    // What it emits and reads (the World's log): engine events by type.
    struct Out {
        EventWriter<AudioPlay> play;
        EventWriter<AudioStop> stop;
        EventWriter<AudioBus> bus;
        EventWriter<AudioSnapshot> snapshot;
        EventWriter<AudioZone> zone;
        EventWriter<AudioOcclusion> occlusion;
        EventWriter<AudioStopWorld> stopWorld;
    } out;
    EventReader<FootEvent> footsteps, landings;
    EventReader<ClipEvent> clips;
    // A game's channels the bank binds (its `events`), by channel index: the bindings in the bank's order.
    std::vector<std::vector<const EventSound*>> bound;
    uint64_t boundVersion{~0ull};
    std::size_t boundChannels{0};

    std::vector<AudioWarning> warnings;
    std::set<std::pair<entt::entity, std::string>> warned;

    Impl(World& w, AssetResolver& a) : world(w), assets(a), bank(defaultBank()) {
        shapeState();
        namespace ae = audio_events;
        EventLog& log = w.events();
        out.play = log.writer<AudioPlay>(ae::kPlay, "audio_schedule");
        out.stop = log.writer<AudioStop>(ae::kStop, "audio_schedule");
        out.bus = log.writer<AudioBus>(ae::kBus, "audio_schedule");
        out.snapshot = log.writer<AudioSnapshot>(ae::kSnapshot, "audio_schedule");
        out.zone = log.writer<AudioZone>(ae::kZone, "audio_schedule");
        out.occlusion = log.writer<AudioOcclusion>(ae::kOcclusion, "audio_schedule");
        out.stopWorld = log.writer<AudioStopWorld>(ae::kStopWorld, "audio_schedule");
        footsteps = log.reader<FootEvent>(physics_events::kFootstep, "audio_schedule");
        landings = log.reader<FootEvent>(physics_events::kLand, "audio_schedule");
        clips = log.reader<ClipEvent>(kClipEventChannel, "audio_schedule");
    }

    void warn(entt::entity e, std::string code, std::string message) {
        if (!warned.emplace(e, code).second) return;
        OHAO_LOG(Warn, audio, "audio schedule: {}: {}", code, message);
        warnings.push_back({std::move(code), e, std::move(message)});
    }

    [[nodiscard]] std::string busName(std::size_t b) const { return b < bank.buses.size() ? bank.buses[b].name : std::string(); }
    void emitSnapshot(AudioSnapshot c) {
        lastSnapshotBlend = c.blendMs;
        out.snapshot.emit(std::move(c));
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
                if (!gone[i] && (scope < 0 || below(s.voices[i].bus, scope))) ++n;
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
                    if (gone[i] || (scope >= 0 && !below(v.bus, scope))) continue;
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

    void playRequest(const SoundRequest& req) {
        const int si = bank.sound(req.sound);
        if (si < 0) return;
        const SoundDesc& desc = bank.sounds[static_cast<std::size_t>(si)];
        const int bus = std::max(0, bank.bus(desc.bus));
        const ShapeDesc* shape = desc.shape == "ear" ? nullptr : bank.shape(desc.shape);
        const bool ear = req.ear || shape == nullptr;
        const bool own = ears && ears->playerId != 0 && req.owner == ears->playerId;

        const auto sound = static_cast<uint16_t>(si);
        AudioPlay c;
        c.sound = desc.name;
        if (!ear) c.pos = req.at;
        c.gain = desc.volume * req.scale;
        c.variant = req.variant ? *req.variant : s.robin[sound]++;

        const bool occlude = desc.occlude && !own && !ear;
        Occlusion occl;
        if (occlude && ears)
            if (const auto o = cast(*ears, req.at)) occl = *o;
        c.walls = occl.walls;
        c.occlusionDb = occl.db;
        c.lowPassHz = static_cast<float>(occl.lowPassHz);
        const float distance = ears && !ear ? glm::length(req.at - ears->position) : 0.0f;
        const float audible = c.gain * (shape && !ear ? distanceGain(distance, *shape) : 1.0f) * dbToGain(occl.db);

        c.voice = ++s.nextVoice;
        std::vector<std::size_t> taken;
        if (!victims(bus, desc.priority, audible, taken)) {
            c.outcome = AudioOutcome::Dropped;
            ++stats.dropped;
            out.play.emit(std::move(c));
            return;
        }
        // The victims stop first (a 2 ms fade in the device), then the new voice starts.
        std::sort(taken.begin(), taken.end());
        for (const std::size_t i : taken) {
            out.stop.emit(AudioStop{s.voices[i].id});
            c.victim = s.voices[i].id;
        }
        for (auto it = taken.rbegin(); it != taken.rend(); ++it) s.voices.erase(s.voices.begin() + static_cast<std::ptrdiff_t>(*it));
        c.outcome = taken.empty() ? AudioOutcome::Played : AudioOutcome::Stole;
        ++(taken.empty() ? stats.played : stats.stole);
        const uint32_t voice = c.voice;
        const uint64_t seq = out.play.emit(std::move(c));

        Voice v;
        v.id = voice;
        v.seq = seq;
        v.sound = sound;
        v.bus = static_cast<uint8_t>(bus);
        v.priority = desc.priority;
        v.audible = audible;
        v.start = world.steps();
        v.hold = holdSteps(desc.holdMs, world.stepDt());
        v.end = v.start + v.hold;
        v.source = req.at;
        v.occlude = occlude;
        v.occl = occl;
        s.voices.push_back(v);
        // Ducks this sound triggers hold while it holds its voice.
        for (std::size_t i = 0; i < bank.ducks.size(); ++i)
            if (bank.ducks[i].on == desc.name) s.duckUntil[i] = std::max(s.duckUntil[i], world.steps() + v.hold);
        stats.voices = static_cast<uint32_t>(s.voices.size());
    }

    // Each bus's duck: the deepest rule holding it; a change writes `bus` (its attack going down, the
    // release of the one that held it going up).
    void updateDucks() {
        for (std::size_t b = 0; b < bank.buses.size(); ++b) {
            float target = 0.0f;
            uint32_t attack = 0, release = 0;
            for (std::size_t i = 0; i < bank.ducks.size(); ++i) {
                const DuckDesc& d = bank.ducks[i];
                if (bank.bus(d.bus) != static_cast<int>(b) || s.duckUntil[i] <= world.steps()) continue;
                if (d.db < target) target = d.db, attack = d.attackMs, release = d.releaseMs;
            }
            if (target == s.busDuck[b]) continue;
            const uint32_t blend = target < s.busDuck[b] ? attack : s.busRelease[b];
            s.busDuck[b] = target;
            s.busRelease[b] = release;
            heard.busDuck[b] = target;
            out.bus.emit(AudioBus{busName(b), target, blend, false});
        }
    }

    // ---- the step ---------------------------------------------------------------------------

    // The snapshot the game asks for (requestMixSnapshot), if the bank has it, else the default.
    std::string snapshotTarget() const {
        const auto* request = world.registry().ctx().find<MixRequest>();
        const std::string want = request ? request->snapshot : std::string("default");
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
            return d ? d->inMs : 0;
        }
        const SnapshotDesc* left = from.empty() ? nullptr : snapshotDesc(from);
        return left ? left->outMs : 0;
    }

    // After a restore (a round restart's too): the device is brought from what it heard to where this
    // step's mix will be, by `resync` commands; the step then writes the commands it always would, and
    // the device takes a zone or snapshot it is already in as nothing.
    void resync() {
        if (const std::string zone = zoneTarget(); !zone.empty() && zone != heard.zone && !heard.zone.empty()) {
            const ReverbDesc* p = bank.preset(zone);
            heard.zone = zone;
            out.zone.emit(AudioZone{zone, p ? p->bed : std::string(), kZoneBlendMs, true});
        }
        if (const std::string snap = snapshotTarget(); snap != heard.snapshot && !heard.snapshot.empty()) {
            const uint32_t blend = snapshotBlend(snap, heard.snapshot);
            heard.snapshot = snap;
            emitSnapshot(AudioSnapshot{snap, blend, true});
        }
        for (std::size_t b = 0; b < s.busDuck.size() && b < heard.busDuck.size(); ++b) {
            if (s.busDuck[b] == heard.busDuck[b]) continue;
            heard.busDuck[b] = s.busDuck[b];
            out.bus.emit(AudioBus{busName(b), s.busDuck[b], kOcclusionBlendMs, true});
        }
    }

    // The bank's event bindings by channel index (again when the bank or the channels change).
    void bindEvents() {
        const EventLog& log = world.events();
        if (boundVersion == bankVersion && boundChannels == log.channels().size()) return;
        boundVersion = bankVersion;
        boundChannels = log.channels().size();
        bound.assign(boundChannels, {});
        for (const EventSound& d : bank.events)
            if (const EventChannelBase* c = log.find(d.on)) {
                bound[c->index()].push_back(&d);
                world.events().addReader(d.on, "audio_schedule");
            }
    }

    // A game's events, in the order they happened: the first binding each matches plays its sounds.
    void playBound(uint64_t playerId) {
        bindEvents();
        world.events().visitWindow([&](const EventChannelBase& c, std::size_t i) {
            if (c.index() >= bound.size() || bound[c.index()].empty()) return;
            Cue cue;
            if (!c.cue(i, cue)) return;
            const bool byPlayer = playerId != 0 && cue.source == playerId;
            for (const EventSound* d : bound[c.index()]) {
                if (d->by == EventSound::By::Player && !byPlayer) continue;
                if (d->by == EventSound::By::Other && byPlayer) continue;
                if (d->tag && *d->tag != cue.tag) continue;
                for (std::string name : d->play) {
                    if (const auto at = name.find("{tag}"); at != std::string::npos)
                        name.replace(at, 5, cue.tag.empty() ? std::string_view("default") : cue.tag);
                    if (bank.sound(name) < 0 && !d->fallback.empty()) name = d->fallback;
                    playRequest({name, d->world ? 0 : cue.source, cue.at, d->ear});
                }
                return;
            }
        });
    }

    void step() {
        raysThisStep = 0;
        refreshBank();
        ears = listenerAtStep(world);

        // 1. Any restore since the last step (a round's restart too): the device's world voices fade; the
        // restored mix is resent.
        if (restored) {
            out.stopWorld.emit(AudioStopWorld{kStopWorldMs});
            resync();
            restored = false;
        }

        // 2. The listener's zone. Unknown presets warn here, once per zone.
        for (auto [e, z] : world.registry().view<const ReverbZone>().each()) (void)known(e, z);
        if (const std::string preset = zoneTarget(); ears && preset != s.zone) {
            s.zone = preset;
            heard.zone = preset;
            const ReverbDesc* p = bank.preset(preset);
            out.zone.emit(AudioZone{preset, p ? p->bed : std::string(), kZoneBlendMs, false});
        }

        // 3. The snapshot from the round.
        if (const std::string want = snapshotTarget(); want != s.snapshot) {
            const uint32_t blend = snapshotBlend(want, s.snapshot);
            s.snapshot = want;
            heard.snapshot = want;
            emitSnapshot(AudioSnapshot{want, blend, false});
        }

        // 4. Voices that have ended let go; then the step's sounds.
        std::erase_if(s.voices, [&](const Voice& v) { return world.steps() >= v.end; });
        const uint64_t playerId = ears ? ears->playerId : 0;
        const auto& reg = world.registry();
        // Footsteps and landings in the order physics emitted them (a restart in the step ended what came before).
        {
            const auto steps = footsteps.read(), lands = landings.read();
            for (std::size_t i = 0, j = 0; i < steps.size() || j < lands.size();) {
                const bool land = j < lands.size() && (i == steps.size() || lands[j].meta.seq < steps[i].meta.seq);
                const FootEvent& e = land ? lands[j++].value : steps[i++].value;
                if (!land) {
                    const entt::entity who = world.findById(e.player);
                    if (who != entt::null && footFromClips(who)) continue;   // its clip's feet play its steps
                }
                const std::string sound = footstepSound(bank, e.surface);
                if (!sound.empty()) playRequest({sound, e.player, e.point, false, footstepScale(land, e.speed)});
            }
        }
        playBound(playerId);
        for (const auto& [meta, e] : clips.read())
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
                            playRequest({sound, owner, at, false, footstepScale(false, std::sqrt(v.x * v.x + v.z * v.z))});
                        continue;
                    }
                    at.y += name == "body_fall" ? 0.2f : 1.1f;   // the floor, or the hands at the chest
                    playRequest({name, owner, at, playerId != 0 && owner == playerId});
                }

        // 5. Ducks.
        updateDucks();

        // 6. Held voices re-cast every kRecastMs.
        const uint32_t recast = periodSteps(kRecastMs, world.stepDt());
        if (ears)
            for (Voice& v : s.voices) {
                if (!v.occlude || v.hold <= recast || world.steps() <= v.start || (world.steps() - v.start) % recast != 0) continue;
                const auto now = cast(*ears, v.source);
                if (!now || now->walls == v.occl.walls) continue;   // capped: the last answer stands
                v.occl = *now;
                out.occlusion.emit(AudioOcclusion{v.id, v.occl.walls, v.occl.db, static_cast<float>(v.occl.lowPassHz),
                                                  kOcclusionBlendMs});
            }
        stats.voices = static_cast<uint32_t>(s.voices.size());
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

void AudioSchedule::setBank(const AudioBank& bank) {
    m->bankSet = true;
    m->bank = bank;
    m->bankChanged();
}
const AudioBank& AudioSchedule::bank() const noexcept { return m->bank; }
uint64_t AudioSchedule::bankVersion() const noexcept { return m->bankVersion; }
uint64_t AudioSchedule::steps() const noexcept { return m->world.steps(); }
const std::string& AudioSchedule::zone() const noexcept { return m->s.zone; }
const std::string& AudioSchedule::snapshot() const noexcept { return m->s.snapshot; }
AudioScheduleStats AudioSchedule::stats() const noexcept { return m->stats; }
float AudioSchedule::busDuckDb(std::size_t bus) const noexcept {
    return bus < m->s.busDuck.size() ? m->s.busDuck[bus] : 0.0f;
}
uint32_t AudioSchedule::snapshotBlendMs() const noexcept { return m->lastSnapshotBlend; }
const std::vector<AudioWarning>& AudioSchedule::warnings() const noexcept { return m->warnings; }
std::string AudioSchedule::zoneAt(glm::vec3 p) const { return m->zoneAt(p); }

std::vector<ScheduledVoice> AudioSchedule::voices() const {
    std::vector<ScheduledVoice> out;
    for (const auto& v : m->s.voices)
        out.push_back({v.id, v.seq, v.sound, v.bus, v.priority, v.audible, v.start, v.end});
    return out;
}

std::shared_ptr<const void> AudioSchedule::saveState() const { return std::make_shared<const Impl::State>(m->s); }

void AudioSchedule::restoreState(const std::shared_ptr<const void>& state) {
    if (!state) return;
    m->s = *static_cast<const Impl::State*>(state.get());
    m->shapeState();
    m->restored = true;
}

void AudioSchedule::hashState(StateHash& h) const {
    const Impl::State& s = m->s;
    h.u64(s.voices.size());
    // v.seq is its play command's place in the log, which is history (never rewound by a restore) and orders
    // voices only against each other, an order a restore keeps: not hashed.
    for (const Impl::Voice& v : s.voices) {
        h.u32(v.id);
        h.u32(v.sound);
        h.u32(v.bus);
        h.u32(v.priority);
        h.f32(v.audible);
        h.u64(v.start);
        h.u64(v.end);
        h.u32(v.hold);
        h.vec3(v.source);
        h.boolean(v.occlude);
        h.u32(v.occl.walls);
        h.f32(v.occl.db);
        h.f32(static_cast<float>(v.occl.lowPassHz));
    }
    h.u32(s.nextVoice);
    h.u64(s.robin.size());
    for (const auto& [sound, next] : s.robin) {
        h.u32(sound);
        h.u32(next);
    }
    h.u64(s.duckUntil.size());
    for (const uint64_t d : s.duckUntil) h.u64(d);
    h.u64(s.busDuck.size());
    for (const float d : s.busDuck) h.f32(d);
    h.u64(s.busRelease.size());
    for (const uint32_t r : s.busRelease) h.u32(r);
    h.string(s.zone);
    h.string(s.snapshot);
}

AudioSchedule& installAudio(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<AudioInstalled>()) return *installed->system;
    auto system = std::make_shared<AudioSchedule>(world, assets);
    reg.ctx().emplace<AudioInstalled>(AudioInstalled{system.get()});
    // Its typed readers order it after physics and animation; it plays the cues its bank binds.
    world.addSimulate("audio_schedule", [system](World&, float) { system->step(); }, {.readsCues = true});
    // Snapshots carry the schedule's state.
    AudioSchedule* raw = system.get();
    addSnapshotHook(reg, SnapshotHook{"audio_schedule", [raw] { return raw->saveState(); },
                                      [raw](const std::shared_ptr<const void>& s) {
                                          if (s) raw->restoreState(s);
                                      },
                                      [raw](StateHash& h) { raw->hashState(h); }});
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

} // namespace ohao::ecs

namespace ohao::ecs {

void requestMixSnapshot(World& world, std::string_view snapshot) {
    auto& ctx = world.registry().ctx();
    if (auto* r = ctx.find<MixRequest>()) {
        if (r->snapshot != snapshot) r->snapshot = snapshot;
    } else {
        ctx.emplace<MixRequest>(MixRequest{std::string(snapshot)});
    }
}

} // namespace ohao::ecs
