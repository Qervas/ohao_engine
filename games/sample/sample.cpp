#include "sample/sample.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace ohao::sample {

namespace {

Cue hitCue(const TargetHit& h) {
    Cue c;
    c.at = h.point;
    c.normal = h.normal;
    c.source = h.shooter;
    c.target = h.target;
    return c;
}

// The target a body belongs to: the entity or its nearest ancestor with a Target.
entt::entity targetOf(const entt::registry& reg, entt::entity e) {
    for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
        if (reg.all_of<Target>(e)) return e;
        const auto* p = reg.try_get<Parent>(e);
        e = p ? p->entity : entt::entity{entt::null};
    }
    return entt::null;
}

// The `shoot` simulate system, after physics (players where this step moved them): every input-driven player, in id
// order, fires on `fire` pressed, one ray from its eye along its look; a standing target it meets goes down.
class Shooting {
public:
    explicit Shooting(World& world) : m_hits(world.events().writer<TargetHit>(kHitChannel, "shoot", &hitCue)) {}

    void step(World& world) {
        PhysicsScene* physics = physicsScene(world);
        if (!physics) return;
        auto& reg = world.registry();
        std::vector<std::pair<uint64_t, entt::entity>> players;
        for (auto [e, pc] : reg.view<const PlayerController>().each())
            if (pc.input) players.emplace_back(world.idOf(e), e);
        std::sort(players.begin(), players.end());
        for (const auto& [id, e] : players) {
            const PlayerController& pc = reg.get<PlayerController>(e);
            if (!std::as_const(world).input(pc.stream).pressed(kFire)) continue;
            const auto info = physics->playerInfo(e);
            if (!info) continue;
            const glm::vec3 eye = info->pos + glm::vec3(0.0f, pc.eyeHeight, 0.0f);
            const glm::quat look = glm::angleAxis(glm::radians(info->yawDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                   glm::angleAxis(glm::radians(info->pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
            const auto hit = physics->raycastFrom(eye, look * glm::vec3(0.0f, 0.0f, -1.0f), kRange, e);
            if (!hit) continue;
            const entt::entity t = targetOf(reg, hit->entity);
            if (t == entt::null || reg.get<Target>(t).down) continue;
            reg.patch<Target>(t, [](Target& x) { x.down = true; });
            reg.remove<Spin>(t);
            if (reg.all_of<RigidBody>(t)) reg.patch<RigidBody>(t, [](RigidBody& b) { b.motion = Motion::Dynamic; });
            m_hits.emit(TargetHit{id, world.idOf(t), hit->point, hit->normal});
        }
    }

private:
    EventWriter<TargetHit> m_hits;
};

void registerSampleTypes(reflect::TypeRegistry& r) {
    r.type<Target>("Target").component().host(makeComponentOps<Target>())
        .doc("A target (the sample game): it spins on a kinematic body until a player's round meets it, then it is "
             "down: its Spin goes and its body turns dynamic, so it falls.")
        .field("down", &Target::down).doc("A round met it");
    r.type<TargetHit>("TargetHit").event()
        .doc("sample.hit: a player's round met a standing target")
        .field("shooter", &TargetHit::shooter).entity().doc("Who fired")
        .field("target", &TargetHit::target).entity().doc("The target")
        .field("point", &TargetHit::point).doc("Where").unit("m")
        .field("normal", &TargetHit::normal).doc("The surface's normal").unitless();
}

// sample.targets: every target, in id order, standing or down, and where it is.
struct TargetsParams {};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (r.type<TargetsParams>("TargetsParams").doc("sample.targets params"), true);
    (void)once;
    return r;
}

bool targets(game::Call& c, const reflect::JsonNode& params, reflect::DecodeContext& ctx, reflect::JsonWriter& out) {
    TargetsParams p;
    if (!game::decodeParams(c, *paramsTypes().find<TargetsParams>(), params, ctx, &p)) return false;
    const auto& reg = c.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> all;
    for (const entt::entity e : reg.view<const Target>()) all.emplace_back(c.world.idOf(e), e);
    std::sort(all.begin(), all.end());
    std::size_t down = 0;
    out.beginObject();
    out.key("targets");
    out.beginArray();
    for (const auto& [id, e] : all) {
        const bool isDown = reg.get<Target>(e).down;
        down += isDown ? 1 : 0;
        const glm::vec3 pos = reg.all_of<Transform>(e) ? reg.get<Transform>(e).position : glm::vec3(0.0f);
        out.beginObject();
        out.key("entity"); out.hex64(id);
        if (const auto* n = reg.try_get<Name>(e)) { out.key("name"); out.string(n->value); }
        out.key("down"); out.boolean(isDown);
        out.key("pos");
        out.beginArray();
        for (int i = 0; i < 3; ++i) out.float32(std::round(pos[i] * 1000.0f) / 1000.0f);
        out.endArray();
        out.endObject();
    }
    out.endArray();
    out.key("standing"); out.uint64(all.size() - down);
    out.key("down"); out.uint64(down);
    out.endObject();
    return true;
}

class SampleModule final : public game::Module {
public:
    [[nodiscard]] std::string_view name() const override { return "sample"; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerSampleTypes(types); }
    void install(World& world) const override {
        const auto installed = world.simulateNames();
        if (std::find(installed.begin(), installed.end(), "shoot") != installed.end()) return;
        auto shooting = std::make_shared<Shooting>(world);
        world.addSimulate("shoot", [shooting](World& w, float) { shooting->step(w); }, {.after = {"physics"}});
    }
    [[nodiscard]] std::span<const game::Method> methods() const override {
        static const std::vector<game::Method> rows = {
            {{"sample.targets", paramsTypes().find<TargetsParams>(), "{targets: [{entity, name?, down, pos}], standing, down}",
              "The sample's targets in id order: standing or down, and where each is. Shoot with input.inject (fire) "
              "and world.step."},
             targets},
        };
        return rows;
    }
};

} // namespace

const game::Module& module() {
    static const SampleModule m;
    return m;
}

} // namespace ohao::sample

extern "C" __attribute__((visibility("default"))) const ohao::game::Module* ohao_game_module() {
    return &ohao::sample::module();
}
