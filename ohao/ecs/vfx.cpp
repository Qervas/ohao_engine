#include "ecs/vfx.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/player.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/world.hpp"
#include "vfx/particle_math.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace ohao::ecs {

namespace {

// A world pose: origin at `at`, -Z along `along` (the throw, the round, the surface's normal).
glm::mat4 poseAlong(glm::vec3 at, glm::vec3 along) {
    const float len = glm::length(along);
    const glm::vec3 minusZ = len > 1e-6f ? along / len : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 z = -minusZ;
    const glm::vec3 helper = std::fabs(z.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 x = glm::normalize(glm::cross(helper, z));
    const glm::vec3 y = glm::cross(z, x);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(x, 0.0f);
    m[1] = glm::vec4(y, 0.0f);
    m[2] = glm::vec4(z, 0.0f);
    m[3] = glm::vec4(at, 1.0f);
    return m;
}

// Where `e` stands at this step: its Transforms up the chain (WorldTransform can be a step behind
// inside one). A decal is placed against this, so every host, made early or late, attaches alike.
glm::mat4 stepWorld(const entt::registry& reg, entt::entity e) {
    const auto* t = reg.try_get<Transform>(e);
    glm::mat4 m = t ? t->matrix() : glm::mat4(1.0f);
    int depth = 0;
    for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity) && depth < 64; p = reg.try_get<Parent>(p->entity), ++depth)
        if (const auto* pt = reg.try_get<Transform>(p->entity)) m = pt->matrix() * m;
    return m;
}

// The nearest hit of a ray `origin + t dir` (world) on a BoxPrim under `e` (and its descendants, in
// their frames), with t >= 0: a character's round is decaled on the mesh box it crosses, or not at all.
struct BoxHit {
    float t{std::numeric_limits<float>::max()};
    entt::entity box{entt::null};
    glm::vec3 point{0.0f}, normal{0.0f};
};

void hitBoxes(const entt::registry& reg, entt::entity e, glm::vec3 origin, glm::vec3 dir, BoxHit& best, int depth) {
    if (depth > 4 || !reg.valid(e)) return;
    if (const auto* ms = reg.try_get<MeshSource>(e))
        if (const auto* box = std::get_if<BoxPrim>(&ms->source)) {
            const glm::mat4 world = stepWorld(reg, e);
            const glm::mat4 toLocal = glm::inverse(world);
            const glm::vec3 o(toLocal * glm::vec4(origin, 1.0f));
            const glm::vec3 d(toLocal * glm::vec4(dir, 0.0f));
            float t0 = -std::numeric_limits<float>::max(), t1 = std::numeric_limits<float>::max();
            int axis = -1;
            float sign = 0.0f;
            bool miss = false;
            for (int i = 0; i < 3 && !miss; ++i) {
                if (std::fabs(d[i]) < 1e-12f) {
                    miss = o[i] < box->lo[i] || o[i] > box->hi[i];
                    continue;
                }
                float a = (box->lo[i] - o[i]) / d[i], b = (box->hi[i] - o[i]) / d[i];
                float s = -1.0f;   // entering through the lo face: its normal is -axis
                if (a > b) {
                    std::swap(a, b);
                    s = 1.0f;
                }
                if (a > t0) {
                    t0 = a;
                    axis = i;
                    sign = s;
                }
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (!miss && axis >= 0 && t0 >= 0.0f) {
                const glm::vec3 local = o + d * t0;
                const glm::vec3 p(world * glm::vec4(local, 1.0f));
                const float t = glm::length(p - origin);
                if (t < best.t) {
                    glm::vec3 n(0.0f);
                    n[axis] = sign;
                    best.t = t;
                    best.box = e;
                    best.point = p;
                    best.normal = glm::normalize(glm::transpose(glm::mat3(toLocal)) * n);
                }
            }
        }
    if (const auto* ch = reg.try_get<Children>(e))
        for (const entt::entity c : ch->entities) hitBoxes(reg, c, origin, dir, best, depth + 1);
}

struct VfxInstalled {
    VfxSystem* system{nullptr};
};

std::size_t kindIndex(VfxRecord::Kind k) { return static_cast<std::size_t>(k); }

} // namespace

const char* vfxKindName(VfxRecord::Kind kind) noexcept {
    switch (kind) {
    case VfxRecord::Kind::Effect: return "effect";
    case VfxRecord::Kind::Tracer: return "tracer";
    case VfxRecord::Kind::Decal: return "decal";
    case VfxRecord::Kind::Light: return "light";
    }
    return "?";
}

struct VfxSystem::Impl {
    // A placed emitter's count since it became active.
    struct EmitterState {
        bool active{false};
        uint64_t since{0};     // the step it became active
        uint64_t emitted{0};   // records written since
    };
    // What a snapshot carries.
    struct State {
        std::vector<VfxRecord> records;   // live, in seq order
        uint64_t nextSeq{1};
        std::array<uint64_t, VfxRecord::kKinds> dropped{};
        uint64_t written{0};
        std::map<uint64_t, EmitterState> emitters;   // by entity id
    };

    World& world;
    AssetResolver& assets;
    vfx::VfxSet set{vfx::defaultVfxSet()};
    bool setForced{false};
    bool setRead{false};
    std::string namedPath;   // what the world's VfxSet names (read once per path)
    std::string setPath;     // where the set in use came from ("" for the default)
    uint64_t setVersion{1};  // changes whenever the set does (the renderer copies it then)
    State st;
    std::vector<VfxWarning> warnings;
    std::set<std::pair<std::string, std::string>> warned;   // (code, what): once each
    double lastMs{0.0};
    // The set's event bindings by channel index (again when the set or the channels change).
    struct Bound {
        const vfx::EventBinding* binding{nullptr};
        uint32_t channel{0};   // the channel's name, hashed: part of an event's seed
    };
    std::vector<Bound> bound;
    uint64_t boundVersion{0};
    std::size_t boundChannels{0};

    Impl(World& w, AssetResolver& a) : world(w), assets(a) {}

    void warn(entt::entity e, const std::string& code, const std::string& what, const std::string& message) {
        if (!warned.emplace(code, what).second) return;
        warnings.push_back({code, e, message});
        OHAO_LOG(Warn, vfx, "vfx: {}: {}", code, message);
    }

    // The set the world's VfxSet names (read once per path), else the default. A changed set drops
    // the live records (their indices named the old set's entries).
    void refreshSet() {
        if (setForced) return;
        const auto& reg = world.registry();
        std::string path;
        entt::entity owner = entt::null;
        for (auto [e, vs] : reg.view<const VfxSet>().each()) {
            path = vs.asset.path;
            owner = e;
            break;
        }
        if (setRead && path == namedPath) return;
        const bool first = !setRead;
        setRead = true;
        namedPath = path;
        setPath.clear();
        set = vfx::defaultVfxSet();
        ++setVersion;
        if (!first) st.records.clear();
        if (path.empty()) return;
        const auto file = assets.absolutePath(AssetRef{path, {}});
        std::ifstream f(file, std::ios::binary);
        if (file.empty() || !f) {
            warn(owner, "W_VFX_SET", path, "cannot read '" + path + "'; the default set stands");
            return;
        }
        const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto parsed = vfx::parseVfxSet(json);
        if (!parsed) {
            const auto& e = parsed.error().front();
            warn(owner, "W_VFX_SET", path, "'" + path + "' " + e.path + ": " + e.message + "; the default set stands");
            return;
        }
        set = std::move(parsed).value();
        setPath = path;
    }

    uint32_t capOf(VfxRecord::Kind k) const {
        switch (k) {
        case VfxRecord::Kind::Tracer: return set.caps.tracers;
        case VfxRecord::Kind::Decal: return set.caps.decals;
        default: return set.caps.records;
        }
    }

    // Appends a record; over its kind's cap, the oldest of that kind goes first (counted).
    void add(VfxRecord r) {
        const uint32_t cap = std::max<uint32_t>(capOf(r.kind), 1);
        std::size_t live = 0;
        for (const VfxRecord& x : st.records) live += x.kind == r.kind;
        while (live >= cap) {
            const auto it = std::find_if(st.records.begin(), st.records.end(), [&](const VfxRecord& x) { return x.kind == r.kind; });
            st.records.erase(it);
            ++st.dropped[kindIndex(r.kind)];
            --live;
        }
        r.seq = st.nextSeq++;
        r.spawnStep = world.steps();
        ++st.written;
        st.records.push_back(r);
    }

    // An effect (and its flash light) at `pose`.
    void addEffect(int effect, const glm::mat4& pose, uint32_t seed, entt::entity source, uint64_t sourceSeq) {
        if (effect < 0) return;
        const vfx::EffectDesc& fx = set.effects[static_cast<std::size_t>(effect)];
        VfxRecord r;
        r.kind = VfxRecord::Kind::Effect;
        r.effect = static_cast<uint16_t>(effect);
        r.seed = seed;
        r.sourceSeq = sourceSeq;
        r.source = source;
        r.pose = pose;
        r.life = fx.maxLife();
        add(r);
        if (fx.light) {
            r.kind = VfxRecord::Kind::Light;
            r.seed = vfx::vfxHash(seed, 0u, 0x11u);
            r.life = fx.light->time;
            add(r);
        }
    }


    bool under(entt::entity e, entt::entity ancestor) const {
        const auto& reg = world.registry();
        for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
            if (e == ancestor) return true;
            const auto* p = reg.try_get<Parent>(e);
            e = p ? p->entity : entt::null;
        }
        return false;
    }

    // Seeds: a function of the event's identity (its step, its channel, its place among the step's events of the
    // channel), so the same fight, a restore and a restarted round draw the same particles.
    uint32_t seedFor(uint32_t channel, uint32_t place, uint32_t part) const {
        return vfx::vfxHash(static_cast<uint32_t>(world.steps()), place * 8u + part, 0xEF7u ^ channel);
    }

    static uint32_t nameHash(std::string_view s) {   // FNV-1a
        uint32_t h = 2166136261u;
        for (const char c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
        return h;
    }

    void bindEvents() {
        const EventLog& log = world.events();
        if (boundVersion == setVersion && boundChannels == log.channels().size()) return;
        boundVersion = setVersion;
        boundChannels = log.channels().size();
        bound.assign(boundChannels, {});
        for (const vfx::EventBinding& b : set.events)
            if (const EventChannelBase* c = log.find(b.on)) {
                bound[c->index()] = {&b, nameHash(b.on)};
                world.events().addReader(b.on, "vfx");
            }
    }

    // What a bound event spawns: its effects at its place (ahead along its direction), facing its normal, else
    // its direction, else up; a tracer from there to where it ended (else kTracerMiss on), at the set's cadence
    // on the cue's count; a decal on what it reached (the player never), or on the world.
    void spawn(const vfx::EventBinding& b, uint32_t channel, uint32_t place, uint64_t seq, const Cue& e, entt::entity player) {
        const auto& reg = world.registry();
        const entt::entity source = world.findById(e.source);
        const std::string surface = e.tag.empty() ? std::string("default") : std::string(e.tag);
        const bool moving = glm::length(e.dir) > 1e-6f;
        const glm::vec3 dir = moving ? glm::normalize(e.dir) : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 facing = glm::length(e.normal) > 1e-6f ? e.normal : moving ? dir : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 from = e.at + (moving ? dir * b.ahead : glm::vec3(0.0f));
        const glm::mat4 pose = poseAlong(from, facing);
        uint32_t part = 0;
        for (const std::string& name : b.effects) {
            const int fx = vfx::resolveEffect(set, name, surface, b.fallback);
            if (fx < 0) warn(source, "W_VFX_EFFECT", b.on + "/" + name + "/" + surface, "no effect '" + name + "' for " + b.on + " on " + surface);
            addEffect(fx, pose, seedFor(channel, place, part++), source, seq);
        }
        if (!b.tracer.empty())
            if (const int ti = set.tracer(b.tracer); ti >= 0) {
                const vfx::TracerDesc& td = set.tracers[static_cast<std::size_t>(ti)];
                const uint32_t every = source != entt::null && source == player ? td.everyPlayer : td.everyEnemy;
                const glm::vec3 end = e.to ? *e.to : e.at + dir * kTracerMiss;
                const float distance = e.to ? glm::length(*e.to - e.at) : kTracerMiss;
                if (every != 0 && e.n % every == 0 && distance >= td.minDistance) {
                    VfxRecord r;
                    r.kind = VfxRecord::Kind::Tracer;
                    r.effect = static_cast<uint16_t>(ti);
                    r.seed = seedFor(channel, place, 7);
                    r.sourceSeq = seq;
                    r.source = source;
                    r.pose = pose;
                    r.a = from;
                    r.b = end;
                    r.life = (glm::length(end - from) + td.length) / td.speed;
                    add(r);
                }
            }
        if (b.decal.empty()) return;
        const entt::entity target = world.findById(e.target);
        if (player != entt::null && target != entt::null && under(target, player)) return;   // the player is never marked
        const int di = vfx::resolveDecal(set, b.decal, surface, b.fallback);
        if (di < 0) return;
        // What it reached, and where on it: a character's on the mesh box its direction crossed, anything else's on
        // its own pose; nothing reached: the world.
        entt::entity on = entt::null;
        glm::mat4 hitPose = pose;
        if (target != entt::null && reg.valid(target)) {
            // The character the target belongs to (the target itself, or the entity it is a part of).
            entt::entity character = entt::null;
            for (entt::entity up = target; up != entt::null && reg.valid(up);) {
                if (reg.try_get<PlayerController>(up)) {
                    character = up;
                    break;
                }
                const auto* p = reg.try_get<Parent>(up);
                up = p ? p->entity : entt::null;
            }
            if (character != entt::null) {
                const glm::vec3 in = moving ? dir : -e.normal;
                BoxHit best;
                hitBoxes(reg, character, e.at - in * 0.05f, in, best, 0);
                if (best.box == entt::null) return;
                on = best.box;
                hitPose = poseAlong(best.point, best.normal);
            } else {
                on = target;
            }
        }
        VfxRecord r;
        r.kind = VfxRecord::Kind::Decal;
        r.effect = static_cast<uint16_t>(di);
        r.seed = seedFor(channel, place, 6);
        r.sourceSeq = seq;
        r.source = source;
        r.on = on;
        r.pose = on != entt::null ? glm::inverse(stepWorld(reg, on)) * hitPose : hitPose;
        r.life = set.decals[static_cast<std::size_t>(di)].life;
        add(r);
    }

    void stepEmitters(float dt) {
        auto& reg = world.registry();
        std::vector<std::pair<uint64_t, entt::entity>> placed;
        for (auto [e, em] : reg.view<const VfxEmitter>().each()) placed.emplace_back(world.idOf(e), e);
        std::sort(placed.begin(), placed.end());   // id order, whatever the storage's
        std::map<uint64_t, EmitterState> seen;
        for (const auto& [id, e] : placed) {
            const VfxEmitter& em = reg.get<VfxEmitter>(e);
            EmitterState es = st.emitters.count(id) ? st.emitters[id] : EmitterState{};
            if (!em.active) {
                es.active = false;
                seen[id] = es;
                continue;
            }
            const int fx = set.effect(em.effect);
            if (fx < 0) warn(e, "W_VFX_EFFECT", em.effect, "a VfxEmitter names no effect '" + em.effect + "'");
            const auto* wt = reg.try_get<WorldTransform>(e);
            const glm::mat4 pose = wt ? wt->world : glm::mat4(1.0f);
            const auto spawn = [&] {
                addEffect(fx, pose, vfx::vfxHash(em.seed, static_cast<uint32_t>(id ^ (id >> 32)), static_cast<uint32_t>(es.emitted)),
                          e, 0);
                ++es.emitted;
            };
            if (!es.active) {   // it became active this step
                es.active = true;
                es.since = world.steps();
                es.emitted = 0;
                if (em.rate <= 0.0f) spawn();
            }
            if (em.rate > 0.0f) {
                const double s = static_cast<double>(world.steps() - es.since + 1);
                const auto due = static_cast<uint64_t>(std::floor(s * static_cast<double>(em.rate) * static_cast<double>(dt) + 1e-6));
                while (es.emitted < due) spawn();
            }
            seen[id] = es;
        }
        st.emitters = std::move(seen);
    }

    void expire(float dt) {
        std::erase_if(st.records, [&](const VfxRecord& r) {
            return r.life > 0.0f && static_cast<float>(world.steps() - r.spawnStep) * dt > r.life;
        });
    }

    void step(float dt) {
        const auto t0 = std::chrono::steady_clock::now();
        refreshSet();
        // A game's events, in the order they happened, as the set binds them.
        bindEvents();
        const entt::entity player = playerOf(world);
        world.events().visitWindow([&](const EventChannelBase& c, std::size_t i) {
            if (c.index() >= bound.size() || !bound[c.index()].binding) return;
            Cue cue;
            if (!c.cue(i, cue)) return;
            spawn(*bound[c.index()].binding, bound[c.index()].channel, static_cast<uint32_t>(i - c.windowBegin()),
                  c.meta(i).seq, cue, player);
        });
        stepEmitters(dt);
        expire(dt);
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};

VfxSystem::VfxSystem(World& world, AssetResolver& assets) : m(std::make_unique<Impl>(world, assets)) {}
VfxSystem::~VfxSystem() = default;

void VfxSystem::step(float dt) { m->step(dt); }

std::span<const VfxRecord> VfxSystem::records() const noexcept { return m->st.records; }

VfxStats VfxSystem::stats() const noexcept {
    VfxStats s;
    for (const VfxRecord& r : m->st.records) ++s.live[kindIndex(r.kind)];
    s.dropped = m->st.dropped;
    s.written = m->st.written;
    s.steps = m->world.steps();
    return s;
}

std::vector<VfxRecord> VfxSystem::recordsSince(uint64_t since, std::size_t max, uint64_t& next) const {
    std::vector<VfxRecord> out;
    next = std::min<uint64_t>(since, m->st.nextSeq - 1);   // never past what was written
    for (const VfxRecord& r : m->st.records) {
        if (r.seq <= since) continue;
        if (out.size() >= max) break;
        out.push_back(r);
        next = r.seq;
    }
    return out;
}

const vfx::VfxSet& VfxSystem::set() const { return m->set; }
const std::string& VfxSystem::setPath() const { return m->setPath; }
uint64_t VfxSystem::setVersion() const noexcept { return m->setVersion; }
float VfxSystem::stepDt() const noexcept { return m->world.stepDt(); }

void VfxSystem::addRecordForTest(VfxRecord record) { m->add(record); }

void VfxSystem::setSet(vfx::VfxSet set) {
    m->set = std::move(set);
    ++m->setVersion;
    m->setForced = true;
    m->setPath.clear();
    m->st.records.clear();
}

const std::vector<VfxWarning>& VfxSystem::warnings() const noexcept { return m->warnings; }
uint64_t VfxSystem::steps() const noexcept { return m->world.steps(); }
double VfxSystem::lastStepMs() const noexcept { return m->lastMs; }

std::shared_ptr<const void> VfxSystem::saveState() const { return std::make_shared<Impl::State>(m->st); }

void VfxSystem::restoreState(const std::shared_ptr<const void>& state) {
    if (const auto* s = static_cast<const Impl::State*>(state.get())) m->st = *s;
}

void VfxSystem::hashState(StateHash& h) const {
    const Impl::State& s = m->st;
    h.u64(s.records.size());
    for (const VfxRecord& r : s.records) {
        h.u32(static_cast<uint32_t>(r.kind));
        h.u32(r.effect);
        h.u32(r.seed);
        h.u64(r.seq);
        h.u64(r.spawnStep);   // r.sourceSeq is the combat log's, which is history: not hashed
        h.entity(r.source);
        h.entity(r.on);
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) h.f32(r.pose[c][k]);
        h.vec3(r.a);
        h.vec3(r.b);
        h.f32(r.life);
    }
    h.u64(s.nextSeq);
    for (const uint64_t d : s.dropped) h.u64(d);
    h.u64(s.written);
    h.u64(s.emitters.size());
    for (const auto& [id, em] : s.emitters) {
        h.u64(id);
        h.boolean(em.active);
        h.u64(em.since);
        h.u64(em.emitted);
    }
}

VfxSystem& installVfx(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<VfxInstalled>()) return *installed->system;
    auto system = std::make_shared<VfxSystem>(world, assets);
    reg.ctx().emplace<VfxInstalled>(VfxInstalled{system.get()});
    // After combat and the animation step; before the audio schedule (the step's last).
    world.addSimulate("vfx", [system](World&, float dt) { system->step(dt); });
    VfxSystem* raw = system.get();
    addSnapshotHook(reg, SnapshotHook{"vfx", [raw] { return raw->saveState(); },
                                      [raw](const std::shared_ptr<const void>& s) {
                                          if (s) raw->restoreState(s);
                                      },
                                      [raw](StateHash& h) { raw->hashState(h); }});
    return *system;
}

VfxSystem* vfxSystem(World& world) {
    const auto* installed = world.registry().ctx().find<VfxInstalled>();
    return installed ? installed->system : nullptr;
}

const VfxSystem* vfxSystem(const World& world) {
    const auto* installed = world.registry().ctx().find<VfxInstalled>();
    return installed ? installed->system : nullptr;
}

std::span<const VfxRecord> vfxRecords(const World& world) {
    const VfxSystem* s = vfxSystem(world);
    return s ? s->records() : std::span<const VfxRecord>{};
}

VfxStats vfxStats(const World& world) {
    const VfxSystem* s = vfxSystem(world);
    return s ? s->stats() : VfxStats{};
}

} // namespace ohao::ecs
