// The animation module's API method (animation/module.hpp): animation.info (sub-project 10, step 1;
// docs/specs/2026-10-28-animation-design.md, "The agent-facing surface"), every Animator's layers, a row of the
// session's catalogue where the module is installed, with its params in the module's own registry. Its clip events are
// events.read's anim.clip_event (step 3c). Reading changes nothing, so it does not sync. Forcing a state is
// component.set on Animator (saved), through world.apply like any edit.
#include "animation/module.hpp"

#include "animation/anim_graph.hpp"
#include "animation/animation.hpp"
#include "animation/components.hpp"
#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace ohao::ecs {

namespace {

using reflect::DecodeContext;
using reflect::hex16;
using reflect::JsonNode;
using reflect::JsonWriter;

constexpr std::size_t kMaxAnimators = 16;   // the first, in id order, then "more"
constexpr std::size_t kMaxName = 24;        // names in replies, in bytes (a whole UTF-8 sequence)

float r3(float v) { return std::fabs(v) < 0.0005f ? 0.0f : std::round(v * 1000.0f) / 1000.0f; }

std::string utf8Prefix(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) return s;
    std::size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string nameOf(const World& world, entt::entity e) {
    const auto& reg = world.registry();
    const auto* n = e != entt::null && reg.valid(e) ? reg.try_get<Name>(e) : nullptr;
    return n ? utf8Prefix(n->value, kMaxName) : std::string();
}

struct AnimationInfoParams {
    std::optional<uint64_t> entity;   // one animator; absent: every animator (the first 16)
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        r.type<AnimationInfoParams>("AnimationInfoParams").doc("animation.info params")
            .field("entity", &AnimationInfoParams::entity).doc("One animator, 16 hex digits; absent: every animator");
        return true;
    }();
    (void)once;
    return r;
}

// One layer: its state, the clip that times it and the time into that clip (the newest step's,
// wrapped or clamped as the pose samples it), and the fade.
void writeLayer(const anim::BoundGraph& g, int index, const AnimLayerState& a, const anim::Params& params,
                float dt, JsonWriter& out) {
    const anim::BoundLayer& l = g.layers[static_cast<std::size_t>(index)];
    out.beginObject();
    out.key("state"); out.string(a.state.empty() ? l.stateNames[static_cast<std::size_t>(l.initial)] : a.state);
    const int s = a.state.empty() ? l.initial : g.state(index, a.state);
    if (s >= 0) {
        const anim::BoundState& st = l.states[static_cast<std::size_t>(s)];
        const int clip = st.clip >= 0 ? st.clip : (st.points.empty() ? -1 : st.points.front().first);
        if (clip >= 0) {
            const anim::BoundClip& c = g.clips[static_cast<std::size_t>(clip)];
            const float t = anim::stateTime(g, st, clip, static_cast<float>(a.steps), dt, params);
            out.key("clip"); out.string(c.name);
            out.key("t"); out.float32(r3(anim::clipTime(c.clip.duration, t, c.loop)));
        }
    }
    if (!a.from.empty() && a.fade > 0) {
        out.key("from"); out.string(a.from);
        out.key("blend"); out.float32(r3(static_cast<float>(std::min(a.faded, a.fade)) / static_cast<float>(a.fade)));
    }
    out.endObject();
}

bool info(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AnimationInfoParams p;
    if (!decodeParams(c, *paramsTypes().find<AnimationInfoParams>(), params, ctx, &p)) return false;
    AnimationSystem* anim = animationSystem(c.world);
    const auto& reg = c.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> animators;
    if (p.entity) {
        const entt::entity e = c.world.findById(*p.entity);
        if (e == entt::null || !reg.all_of<Animator>(e)) {
            ctx.push("entity");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "entity"),
                      "entity " + hex16(*p.entity) + (e == entt::null ? " does not exist" : " has no Animator"),
                      "the id of an entity with an Animator", "\"" + hex16(*p.entity) + "\"");
            ctx.pop();
            return false;
        }
        animators.emplace_back(*p.entity, e);
    } else {
        for (const entt::entity e : reg.view<Animator>()) animators.emplace_back(c.world.idOf(e), e);
        std::sort(animators.begin(), animators.end());
    }

    out.beginObject();
    out.key("steps"); out.uint64(c.world.steps());
    out.key("animators");
    out.beginArray();
    for (std::size_t i = 0; i < animators.size() && i < kMaxAnimators; ++i) {
        const auto& [id, e] = animators[i];
        const Animator& a = reg.get<Animator>(e);
        out.beginObject();
        out.key("entity"); out.string(hex16(id));
        out.key("name"); out.string(nameOf(c.world, e));
        out.key("graph"); out.string(a.graph.path);
        const auto g = anim->graph(a.graph.path);
        out.key("layers");
        out.beginArray();
        if (g) {
            // The drivers' last values (a clip whose rate comes from a parameter is timed by it).
            anim::Params values;
            values.values.assign(g->paramNames.size(), 0.0f);
            if (const auto* ap = reg.try_get<AnimParams>(e))
                for (const auto& [name, v] : ap->values)
                    if (const int k = g->param(name); k >= 0) values.values[static_cast<std::size_t>(k)] = v;
            for (std::size_t li = 0; li < g->layers.size(); ++li) {
                // Before its first step an animator has no layers: each at its initial state.
                const AnimLayerState st = li < a.layers.size() ? a.layers[li] : AnimLayerState{};
                writeLayer(*g, static_cast<int>(li), st, values, c.world.stepDt(), out);
            }
        }
        out.endArray();
        out.endObject();
    }
    out.endArray();
    if (animators.size() > kMaxAnimators) { out.key("more"); out.uint64(animators.size() - kMaxAnimators); }

    out.endObject();
    return true;
}

} // namespace

std::span<const Method> animationMethods() {
    static const std::vector<Method> methods = {
        {{"animation.info", paramsTypes().find<AnimationInfoParams>(),
          "{steps, animators: [{entity, name, graph, layers: [{state, clip?, t?, from?, blend?}]}], more?}",
          "Each Animator's layers (state, clip time in s, the fade); its clip events are events.read's "
          "anim.clip_event. Force a state with component.set on Animator."},
         info},
    };
    return methods;
}

} // namespace ohao::ecs
