#include "ecs/animation.hpp"

#include "ecs/snapshot_hook.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/arms_anim.hpp"
#include "ecs/enemy_anim.hpp"
#include "asset/model.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace ohao::ecs {

namespace {

struct AnimationInstalled {
    AnimationSystem* system{nullptr};
};

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int stateIndex(const anim::BoundGraph& g, int layer, const std::string& name, int fallback) {
    if (name.empty()) return fallback;
    const int s = g.state(layer, name);
    return s >= 0 ? s : fallback;
}

// The component's names to the graph's indices (an unknown name: the layer's initial state).
anim::LayerState toState(const anim::BoundGraph& g, int layer, const AnimLayerState& a) {
    anim::LayerState s;
    s.state = stateIndex(g, layer, a.state, g.layers[static_cast<std::size_t>(layer)].initial);
    s.steps = a.steps;
    s.from = a.from.empty() ? -1 : g.state(layer, a.from);
    s.fromSteps = s.from >= 0 ? a.fromSteps : 0;
    s.fade = s.from >= 0 ? static_cast<uint16_t>(std::min<uint32_t>(a.fade, 600)) : 0;
    s.faded = s.from >= 0 ? static_cast<uint16_t>(std::min<uint32_t>(a.faded, 600)) : 0;
    return s;
}

void fromState(const anim::BoundGraph& g, int layer, const anim::LayerState& s, AnimLayerState& a) {
    const auto& names = g.layers[static_cast<std::size_t>(layer)].stateNames;
    a.state = names[static_cast<std::size_t>(s.state)];
    a.steps = s.steps;
    a.from = s.from >= 0 ? names[static_cast<std::size_t>(s.from)] : std::string();
    a.fromSteps = s.from >= 0 ? s.fromSteps : 0;
    a.fade = s.from >= 0 ? s.fade : 0;
    a.faded = s.from >= 0 ? s.faded : 0;
}

void initLayers(const anim::BoundGraph& g, Animator& a) {
    const auto init = anim::initialLayers(g);
    a.layers.assign(init.size(), {});
    for (std::size_t i = 0; i < init.size(); ++i) fromState(g, static_cast<int>(i), init[i], a.layers[i]);
}

// AnimParams by the graph's parameter order (missing: 0).
void toParams(const anim::BoundGraph& g, const AnimParams* p, anim::Params& out) {
    out.values.assign(g.paramNames.size(), 0.0f);
    out.events.clear();
    if (!p) return;
    for (const auto& [name, value] : p->values)
        if (const int i = g.param(name); i >= 0) out.values[static_cast<std::size_t>(i)] = value;
    out.events = p->events;
}

} // namespace

void AnimParams::set(std::string_view name, float value) {
    for (auto& [n, v] : values)
        if (n == name) {
            v = value;
            return;
        }
    values.emplace_back(std::string(name), value);
}

AnimationSystem::AnimationSystem(AssetResolver& assets)
    : m_assets(assets), m_cache([&assets](const std::string& path) -> std::optional<std::vector<uint8_t>> {
          AssetRef ref;
          ref.path = path;
          const std::filesystem::path abs = assets.absolutePath(ref);
          if (abs.empty()) return std::nullopt;
          std::ifstream f(abs, std::ios::binary);
          if (!f) return std::nullopt;
          return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      }) {}

std::shared_ptr<const anim::BoundGraph> AnimationSystem::graph(const std::string& path) {
    auto g = m_cache.graph(path);
    if (g) return g.value();
    if (std::find(m_failed.begin(), m_failed.end(), path) == m_failed.end()) {
        m_failed.push_back(path);
        OHAO_LOG(Error, asset, "animation graph {}: {} {} {}", path, g.error().code, g.error().path, g.error().message);
    }
    return nullptr;
}

void AnimationSystem::setDriver(std::string name, AnimDriver fn) {
    for (auto& [n, f] : m_drivers)
        if (n == name) {
            f = std::move(fn);
            return;
        }
    m_drivers.emplace_back(std::move(name), std::move(fn));
}

std::vector<AnimEvent> AnimationSystem::eventsSince(uint64_t seq, std::size_t max, uint64_t& dropped) const {
    const uint64_t first = m_nextSeq - m_log.size();
    dropped = seq < first ? first - seq : 0;
    std::vector<AnimEvent> out;
    for (std::size_t i = static_cast<std::size_t>(std::max(seq, first) - first); i < m_log.size() && out.size() < max; ++i)
        out.push_back(m_log[i]);
    return out;
}

void AnimationSystem::step(World& world) {
    const auto t0 = std::chrono::steady_clock::now();
    auto& reg = world.registry();
    ++m_steps;
    m_stepEvents.clear();
    m_order.clear();
    for (const entt::entity e : reg.view<Animator>()) m_order.emplace_back(world.idOf(e), e);
    std::sort(m_order.begin(), m_order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    anim::Params params;
    std::vector<anim::FiredEvent> fired;
    for (const auto& [id, e] : m_order) {
        Animator& a = reg.get<Animator>(e);   // the step's own write: no signal, not a placement
        const auto g = graph(a.graph.path);
        if (!g) continue;
        if (a.layers.size() != g->layers.size()) initLayers(*g, a);
        AnimParams* p = reg.try_get<AnimParams>(e);
        if (!g->source.driver.empty()) {
            const auto d = std::find_if(m_drivers.begin(), m_drivers.end(),
                                        [&](const auto& x) { return x.first == g->source.driver; });
            if (d != m_drivers.end()) {
                if (!p) p = &reg.emplace<AnimParams>(e);
                d->second(world, e, *p);
            }
        }
        toParams(*g, p, params);
        for (std::size_t li = 0; li < g->layers.size(); ++li) {
            const int layer = static_cast<int>(li);
            anim::LayerState s = toState(*g, layer, a.layers[li]);
            fired.clear();
            anim::stepLayer(*g, layer, s, params, fired);
            fromState(*g, layer, s, a.layers[li]);
            for (const anim::FiredEvent& f : fired) {
                AnimEvent ev;
                ev.step = m_steps;
                ev.seq = m_nextSeq++;
                ev.entity = id;
                ev.layer = static_cast<uint8_t>(f.layer);
                ev.clip = f.clip >= 0 ? g->clips[static_cast<std::size_t>(f.clip)].name : std::string();
                ev.name = f.name;
                m_stepEvents.push_back(ev);
                m_log.push_back(std::move(ev));
                if (m_log.size() > kLogCapacity) m_log.pop_front();
            }
        }
        if (p) p->events.clear();
    }
    m_lastStepMs = msSince(t0);
}

void AnimationSystem::poseEntity(World& world, entt::entity e, bool placed) {
    auto& reg = world.registry();
    const Animator& a = reg.get<Animator>(e);
    const auto g = graph(a.graph.path);
    if (!g) {
        reg.remove<AnimPose>(e);
        return;
    }
    std::vector<anim::LayerState> layers;
    if (a.layers.size() == g->layers.size()) {
        layers.reserve(a.layers.size());
        for (std::size_t li = 0; li < a.layers.size(); ++li) layers.push_back(toState(*g, static_cast<int>(li), a.layers[li]));
    } else {
        layers = anim::initialLayers(*g);   // not stepped yet: the initial states at step 0
    }
    anim::Params params;
    toParams(*g, reg.try_get<AnimParams>(e), params);
    const float alpha = std::clamp(world.renderAlpha(), 0.0f, 1.0f);
    anim::evaluatePose(*g, layers, params, alpha, m_local, m_scratch);

    AnimPose& pose = reg.get_or_emplace<AnimPose>(e);
    if (pose.graph != g) {
        pose.graph = g;
        pose.boneToJoint.clear();
        placed = true;
    }
    anim::modelMatrices(g->skeleton, m_local, pose.model);

    const auto* skinned = reg.try_get<SkinnedMesh>(e);
    const auto* source = reg.try_get<MeshSource>(e);
    const auto* model = source ? std::get_if<ModelAsset>(&source->source) : nullptr;
    if (!skinned || !model) {
        pose.palette.clear();
        pose.prevPalette.clear();
        ++pose.frame;
        return;
    }
    if (pose.boneToJoint.empty()) {
        auto loaded = m_assets.loadModel(model->asset, model->mesh);
        if (loaded && loaded.value()) {
            const Model& mesh = *loaded.value();
            pose.boneToJoint = anim::boneToJointMap(g->skeleton, mesh.boneNames);
            const glm::vec3 none(3.4e38f), noneMax(-3.4e38f);
            pose.boneBoxes.assign(pose.boneToJoint.size() + 1, {none, noneMax});
            for (const Vertex& v : mesh.vertices) {
                bool held = false;
                for (int k = 0; k < 4; ++k) {
                    const int b = v.boneIndices[k];
                    if (v.boneWeights[k] <= 0.0f || b < 0 || b >= static_cast<int>(pose.boneToJoint.size())) continue;
                    auto& [lo, hi] = pose.boneBoxes[static_cast<std::size_t>(b)];
                    lo = glm::min(lo, v.position);
                    hi = glm::max(hi, v.position);
                    held = true;
                }
                if (!held) {
                    auto& [lo, hi] = pose.boneBoxes.back();
                    lo = glm::min(lo, v.position);
                    hi = glm::max(hi, v.position);
                }
            }
        }
    }
    if (placed || pose.frame == 0 || pose.palette.size() != pose.boneToJoint.size()) {
        anim::skinPalette(g->skeleton, pose.model, pose.boneToJoint, pose.palette);
        pose.prevPalette = pose.palette;
    } else {
        pose.prevPalette.swap(pose.palette);
        anim::skinPalette(g->skeleton, pose.model, pose.boneToJoint, pose.palette);
    }
    // The posed local box: each bone's box through its matrix (its eight corners), and the
    // vertices no bone holds where they are.
    glm::vec3 lo(3.4e38f), hi(-3.4e38f);
    for (std::size_t b = 0; b < pose.boneBoxes.size(); ++b) {
        const auto& [bmin, bmax] = pose.boneBoxes[b];
        if (bmin.x > bmax.x) continue;
        const glm::mat4 m = b < pose.palette.size() ? pose.palette[b] : glm::mat4(1.0f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
            const glm::vec3 p = glm::vec3(m * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    pose.boundsMin = lo.x <= hi.x ? lo : glm::vec3(0.0f);
    pose.boundsMax = lo.x <= hi.x ? hi : glm::vec3(0.0f);
    ++pose.frame;
}

void AnimationSystem::pose(World& world) {
    const auto t0 = std::chrono::steady_clock::now();
    auto& reg = world.registry();
    std::sort(m_placed.begin(), m_placed.end());
    for (const entt::entity e : reg.view<Animator>())
        poseEntity(world, e, std::binary_search(m_placed.begin(), m_placed.end(), e));
    m_placed.clear();
    m_seen.clear();
    for (const entt::entity e : reg.view<AnimPose>(entt::exclude<Animator>)) m_seen.push_back(e);
    for (const entt::entity e : m_seen) reg.remove<AnimPose>(e);
    attach(world);
    m_lastPoseMs = msSince(t0);
}

// JointAttach children: drawn at their parent's joint (its drawn pose × the joint × their own
// Transform), and their subtrees with them (the rifle's muzzle light). The previous drawn pose is
// what this system drew last frame, or (new) the pose.
void AnimationSystem::attach(World& world) {
    auto& reg = world.registry();
    m_seen.clear();
    const auto draw = [&](const auto& self, entt::entity c, const glm::mat4& world, int depth) -> void {
        m_seen.push_back(c);
        const auto last = m_drawn.find(c);
        const glm::mat4 prev = last != m_drawn.end() ? last->second : world;
        const auto* rp = reg.try_get<RenderPose>(c);
        if (!rp || rp->world != world || rp->prevWorld != prev) {
            reg.emplace_or_replace<RenderPose>(c, RenderPose{world, prev});
            reg.emplace_or_replace<RenderDirty>(c);
        }
        m_drawn[c] = world;
        if (depth >= 4) return;
        if (const auto* kids = reg.try_get<Children>(c))
            for (const entt::entity k : kids->entities)
                if (reg.valid(k) && !reg.all_of<JointAttach>(k))
                    if (const auto* t = reg.try_get<Transform>(k)) self(self, k, world * t->matrix(), depth + 1);
    };
    for (const entt::entity c : reg.view<JointAttach, Parent, Transform, WorldTransform>()) {
        const entt::entity parent = reg.get<Parent>(c).entity;
        if (parent == entt::null || !reg.valid(parent)) continue;
        const auto* pose = reg.try_get<AnimPose>(parent);
        const auto* pw = reg.try_get<WorldTransform>(parent);
        if (!pose || !pose->graph || !pw) continue;
        const int joint = pose->graph->skeleton.find(reg.get<JointAttach>(c).joint);
        if (joint < 0 || joint >= static_cast<int>(pose->model.size())) continue;
        const auto* prp = reg.try_get<RenderPose>(parent);
        const glm::mat4 parentWorld = prp ? prp->world : pw->world;
        draw(draw, c, parentWorld * pose->model[static_cast<std::size_t>(joint)] * reg.get<Transform>(c).matrix(), 0);
    }
    // Drawn last frame and no longer attached: back to the step's pose (unless another system drew it).
    for (auto it = m_drawn.begin(); it != m_drawn.end();) {
        const entt::entity x = it->first;
        if (std::find(m_seen.begin(), m_seen.end(), x) != m_seen.end()) {
            ++it;
            continue;
        }
        if (reg.valid(x))
            if (const auto* rp = reg.try_get<RenderPose>(x); rp && rp->world == it->second) {
                reg.remove<RenderPose>(x);
                reg.emplace_or_replace<RenderDirty>(x);
            }
        it = m_drawn.erase(it);
    }
}

AnimationSystem& installAnimation(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<AnimationInstalled>()) return *installed->system;
    auto system = std::make_shared<AnimationSystem>(assets);
    reg.ctx().emplace<AnimationInstalled>(AnimationInstalled{system.get()});
    registerEnemyDriver(*system);
    registerArmsDriver(*system);
    // Animator writes from outside the step are placements (the step writes through get<>()).
    struct Placed {
        AnimationSystem* s;
        void on(entt::registry&, entt::entity e) { s->markPlaced(e); }
    };
    auto placed = std::make_shared<Placed>(Placed{system.get()});
    reg.on_construct<Animator>().connect<&Placed::on>(*placed);
    reg.on_update<Animator>().connect<&Placed::on>(*placed);
    // After physics and combat: it reads what they wrote (ecs::kSimulateOrder).
    world.addSimulate("animation", [system, placed](World& w, float) { system->step(w); });
    // The pose: after the drawn poses of what the step moved (interpolate, the gun), before the
    // render extract reads them (ecs::kSyncOrder).
    world.addSync("pose", [system](World& w) { system->pose(w); });
    // The step count (events carry it). The graphs' state is the Animator's, a component; the log's seq is the log's.
    AnimationSystem* raw = system.get();
    addSnapshotHook(reg, SnapshotHook{"animation", [raw] { return std::make_shared<const uint64_t>(raw->steps()); },
                                      [raw](const std::shared_ptr<const void>& s) {
                                          if (s) raw->setSteps(*static_cast<const uint64_t*>(s.get()));
                                      },
                                      [raw](StateHash& h) { h.u64(raw->steps()); }});
    return *system;
}

AnimationSystem* animationSystem(World& world) {
    const auto* installed = world.registry().ctx().find<AnimationInstalled>();
    return installed ? installed->system : nullptr;
}

const AnimationSystem* animationSystem(const World& world) {
    const auto* installed = world.registry().ctx().find<AnimationInstalled>();
    return installed ? installed->system : nullptr;
}

} // namespace ohao::ecs
