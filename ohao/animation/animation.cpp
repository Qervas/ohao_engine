#include "animation/animation.hpp"

#include "ecs/snapshot_hook.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "visual/components.hpp"
#include "asset/cooked.hpp"
#include "core/jobs/graph.hpp"

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

void AnimationSystem::removeDriver(std::string_view name) {
    std::erase_if(m_drivers, [&](const auto& d) { return d.first == name; });
}

std::vector<std::string> AnimationSystem::driverNames() const {
    std::vector<std::string> out;
    for (const auto& [n, f] : m_drivers) out.push_back(n);
    return out;
}

void AnimationSystem::setEvents(EventLog& log) { m_clipEvents = log.writer<ClipEvent>(kClipEventChannel, "animation"); }

void AnimationSystem::step(World& world) {
    const auto t0 = std::chrono::steady_clock::now();
    auto& reg = world.registry();
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
            anim::stepLayer(*g, layer, s, params, world.stepDt(), fired);
            fromState(*g, layer, s, a.layers[li]);
            if (m_clipEvents)
                for (const anim::FiredEvent& f : fired)
                    m_clipEvents.emit(ClipEvent{id, static_cast<uint32_t>(f.layer),
                                                f.clip >= 0 ? g->clips[static_cast<std::size_t>(f.clip)].name : std::string(),
                                                f.name});
        }
        if (p) p->events.clear();
    }
    m_lastStepMs = msSince(t0);
}

// What changes the registry or reads the assets, in view order: the graph, the AnimPose (emplaced, or removed when the
// graph is missing), a new graph's or mesh's bone map and bone boxes. Returns whether the entity is posed.
bool AnimationSystem::preparePose(World& world, entt::entity e, bool& placed, bool& skinned) {
    auto& reg = world.registry();
    const Animator& a = reg.get<Animator>(e);
    const auto g = graph(a.graph.path);
    if (!g) {
        reg.remove<AnimPose>(e);
        return false;
    }
    AnimPose& pose = reg.get_or_emplace<AnimPose>(e);
    if (pose.graph != g) {
        pose.graph = g;
        pose.boneToJoint.clear();
        placed = true;
    }
    const auto* skinnedMesh = reg.try_get<SkinnedMesh>(e);
    const auto* source = reg.try_get<MeshSource>(e);
    const auto* model = source ? std::get_if<ModelAsset>(&source->source) : nullptr;
    skinned = skinnedMesh && model;
    if (!skinned || !pose.boneToJoint.empty()) return true;
    auto loaded = m_assets.loadMesh(model->asset, model->mesh);
    if (loaded && loaded.value()) {
        const cooked::Mesh& mesh = *loaded.value();
        pose.boneToJoint = anim::boneToJointMap(g->skeleton, mesh.bones);
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
    return true;
}

// One animator's pose into its own AnimPose, from its Animator and AnimParams, nothing else written: it runs on any
// thread, beside the others.
void AnimationSystem::evaluatePose(const PoseJob& job, const Animator& a, const AnimParams* p, AnimPose& pose,
                                   float alpha, float dt, PoseWork& work) {
    const anim::BoundGraph& g = *pose.graph;
    if (a.layers.size() == g.layers.size()) {
        work.layers.clear();
        for (std::size_t li = 0; li < a.layers.size(); ++li) work.layers.push_back(toState(g, static_cast<int>(li), a.layers[li]));
    } else {
        work.layers = anim::initialLayers(g);   // not stepped yet: the initial states at step 0
    }
    toParams(g, p, work.params);
    anim::evaluatePose(g, work.layers, work.params, alpha, dt, work.local, work.scratch);
    anim::modelMatrices(g.skeleton, work.local, pose.model);
    if (!job.skinned) {
        pose.palette.clear();
        pose.prevPalette.clear();
        ++pose.frame;
        return;
    }
    if (job.placed || pose.frame == 0 || pose.palette.size() != pose.boneToJoint.size()) {
        anim::skinPalette(g.skeleton, pose.model, pose.boneToJoint, pose.palette);
        pose.prevPalette = pose.palette;
    } else {
        pose.prevPalette.swap(pose.palette);
        anim::skinPalette(g.skeleton, pose.model, pose.boneToJoint, pose.palette);
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
            const glm::vec3 q = glm::vec3(m * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
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
    // 1. On this thread, in view order: what changes the registry or reads the assets.
    m_jobs.clear();
    std::size_t joints = 0;
    for (const entt::entity e : reg.view<Animator>()) {
        PoseJob job{e, std::binary_search(m_placed.begin(), m_placed.end(), e), false};
        if (!preparePose(world, e, job.placed, job.skinned)) continue;
        joints += reg.get<AnimPose>(e).graph->skeleton.joints.size();
        m_jobs.push_back(job);
    }
    // 2. The poses, each into its own AnimPose, in chunks of about kPoseJointsPerChunk joints: a pose writes nothing
    // another reads, so the result is the same however the chunks run (a world of a few small rigs is one chunk, on
    // this thread).
    if (!m_jobs.empty()) {
        const std::size_t chunks = std::clamp<std::size_t>(joints / kPoseJointsPerChunk, 1, m_jobs.size());
        const std::size_t grain = (m_jobs.size() + chunks - 1) / chunks;
        if (m_work.size() < chunks) m_work.resize(chunks);
        const auto& animators = reg.storage<Animator>();
        const auto& params = reg.storage<AnimParams>();
        auto& poses = reg.storage<AnimPose>();
        const float alpha = std::clamp(world.renderAlpha(), 0.0f, 1.0f);
        const float dt = world.stepDt();
        jobs::parallelFor(0, m_jobs.size(), grain, [&](std::size_t lo, std::size_t hi) {
            PoseWork& work = m_work[lo / grain];
            for (std::size_t i = lo; i < hi; ++i) {
                const entt::entity e = m_jobs[i].e;
                evaluatePose(m_jobs[i], animators.get(e), params.contains(e) ? &params.get(e) : nullptr, poses.get(e),
                             alpha, dt, work);
            }
        }, "pose");
    }
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
    system->setEvents(world.events());
    // Animator writes from outside the step are placements (the step writes through get<>()).
    struct Placed {
        AnimationSystem* s;
        void on(entt::registry&, entt::entity e) { s->markPlaced(e); }
    };
    auto placed = std::make_shared<Placed>(Placed{system.get()});
    const SignalEffect placement{.writesResources = {"animation.placed"}};
    world.onConstruct<Animator, &Placed::on>(placed.get(), placement);
    world.onUpdate<Animator, &Placed::on>(placed.get(), placement);
    // It animates what the step moved; a game's drivers' event readers order it after their writers. Exclusive: the
    // drivers, a game's code, read the registry through the World.
    world.addSimulate("animation", [system, placed](SystemContext& c, float) { system->step(c.world()); },
                      {.after = {"physics"}, .reads = types<Animator, AnimParams>(), .writes = types<Animator, AnimParams>(),
                       .writesResources = {"animation"}, .exclusive = true});
    // The pose: after the drawn poses of what the step moved (a game's view model declares itself before it); the
    // render extract declares itself after it. After the transforms too, which a world without physics (and so
    // without interpolate) would otherwise leave unordered against it. Exclusive: it emplaces and removes poses as animators come and go.
    world.addSync("pose", [system](SystemContext& c) { system->pose(c.world()); },
                  {.after = {"transform", "interpolate"},
                   .reads = types<Animator, AnimParams, SkinnedMesh, MeshSource, JointAttach, Parent, Children, Transform,
                                  WorldTransform>(),
                   .writes = types<AnimPose, RenderPose, RenderDirty>(), .readsResources = {"animation"},
                   .exclusive = true});
    return *system;
}

AnimationSystem* animationSystem(World& world) {
    const auto* installed = world.context().find<AnimationInstalled>();
    return installed ? installed->system : nullptr;
}

const AnimationSystem* animationSystem(const World& world) {
    const auto* installed = world.context().find<AnimationInstalled>();
    return installed ? installed->system : nullptr;
}

bool graphFires(World& world, const std::string& path, std::span<const std::string_view> names) {
    AnimationSystem* anim = animationSystem(world);
    if (!anim) return false;
    const auto g = anim->graph(path);
    if (!g) return false;
    for (const auto& c : g->clips)
        for (const auto& ev : c.clip.events)
            if (std::find(names.begin(), names.end(), ev.name) != names.end()) return true;
    return false;
}

} // namespace ohao::ecs
