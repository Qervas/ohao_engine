#include "slice/view_model.hpp"

#include "slice/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ohao::slice {

glm::mat4 viewModelRemap(const glm::mat4& view, float sceneFovDeg, float fovDeg, float depthScale) {
    // Projecting (x, y, z / k) through the scene's lens is projecting (x, y, z) through a lens k times
    // narrower: k = tan(scene / 2) / tan(fov / 2) > 1 for a narrower gun lens. Shrinking z by 1 / k (and
    // everything by depthScale) keeps every point nearer the eye than depthScale of its distance.
    const float k = std::tan(glm::radians(sceneFovDeg) * 0.5f) / std::tan(glm::radians(fovDeg) * 0.5f);
    glm::mat4 s(1.0f);
    s[0][0] = depthScale;
    s[1][1] = depthScale;
    s[2][2] = depthScale / k;
    return s * view;
}

namespace {

// Aimed down the sights, a live frame draws the view through this (provisional ruling 11).
constexpr float kAdsFovDeg = 55.0f;

struct Gun {
    ViewModelMotion motion;
    bool hasView{false};
    float lastYawDeg{0.0f}, lastPitchDeg{0.0f};
    float drawnAim{0.0f};   // WeaponState.aim followed at render rate
};

// The holder in the registry's context: each gun's motion, the event history's cursor, and what this
// system drew last frame (its RenderPose, so the next frame's prevWorld is it).
struct ViewModelState {
    std::unordered_map<entt::entity, Gun> guns;
    std::unordered_map<entt::entity, glm::mat4> drawn;   // last frame's drawn world, per entity
    uint64_t cursor{0};   // the last seq read
    bool cursorSet{false};
};

struct ViewModelInstalled {
    std::shared_ptr<ViewModelState> state;
};

float wrapDeg(float d) { return std::remainder(d, 360.0f); }

// The view's yaw (about +Y from -Z, counter-clockwise from above) and pitch (up +) from its world.
void yawPitch(const glm::mat4& world, float& yawDeg, float& pitchDeg) {
    const glm::vec3 f = glm::normalize(-glm::vec3(world[2]));
    yawDeg = glm::degrees(std::atan2(-f.x, -f.z));
    pitchDeg = glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
}

const glm::mat4& drawnOrStep(const entt::registry& reg, entt::entity e) {
    if (const auto* rp = reg.try_get<RenderPose>(e)) return rp->world;
    return reg.get<WorldTransform>(e).world;
}

// The nearest ancestor of e (not e) holding a Camera, and the root above it (the gun's owner).
entt::entity cameraAbove(const entt::registry& reg, entt::entity e, entt::entity& root) {
    entt::entity camera = entt::null;
    root = e;
    for (int depth = 0; depth < 64; ++depth) {
        const auto* p = reg.try_get<Parent>(root);
        if (!p || p->entity == entt::null || !reg.valid(p->entity)) break;
        root = p->entity;
        if (camera == entt::null && reg.all_of<Camera>(root)) camera = root;
    }
    return camera;
}

class ViewModelSystem {
public:
    explicit ViewModelSystem(std::shared_ptr<ViewModelState> state) : m_state(std::move(state)) {}

    void run(World& world) {
        auto& reg = world.registry();
        ViewModelState& st = *m_state;
        const float dt = world.presentationDt();
        const bool live = dt > 0.0f;

        // The rounds (and restarts) since the last frame, from the slice's events (the World's log).
        m_shots.clear();
        bool restarted = false;
        {
            const EventLog& log = world.events();
            if (!st.cursorSet) {
                st.cursor = log.nextSeq() - 1;
                st.cursorSet = true;
            }
            const auto* shots = log.find<ShotEvent>(slice_events::kShot);
            const auto* restores = log.find<RestoreEvent>(kRestoreChannel);
            st.cursor = log.visit(st.cursor, SIZE_MAX, [&](const EventChannelBase& c, std::size_t i) {
                if (restores && &c == restores) restarted = true;   // any restore: the guns' motion starts over
                else if (shots && &c == shots) m_shots.push_back(world.findById(shots->at(i).value.shooter));
            });
        }

        m_seen.clear();
        m_narrowed.clear();
        for (auto [e, vm, t] : reg.view<const ViewModel, const Transform>().each()) {
            entt::entity owner = entt::null;
            const entt::entity camera = cameraAbove(reg, e, owner);
            if (camera == entt::null || !reg.all_of<WorldTransform>(camera)) continue;
            const glm::mat4& view = drawnOrStep(reg, camera);
            Gun& gun = st.guns[e];
            ViewModelMotion& m = gun.motion;

            float yaw = 0.0f, pitch = 0.0f;
            yawPitch(view, yaw, pitch);
            if (!live || restarted) {
                const uint64_t rounds = m.rounds;
                m = ViewModelMotion{};
                m.rounds = rounds;
            }
            if (live) {
                // Return first, then this frame's turn and rounds, so a turn shows in full on its frame.
                const float swayKeep = std::exp(-vm.swayReturnPerSec * dt);
                const float kickKeep = std::exp(-vm.kickReturnPerSec * dt);
                m.swayYawDeg *= swayKeep;
                m.swayPitchDeg *= swayKeep;
                m.kickBack *= kickKeep;
                m.kickPitchDeg *= kickKeep;
                if (gun.hasView && !restarted) {
                    m.swayYawDeg = std::clamp(m.swayYawDeg - wrapDeg(yaw - gun.lastYawDeg) * vm.swayDegPerDeg,
                                              -vm.swayMaxDeg, vm.swayMaxDeg);
                    m.swayPitchDeg = std::clamp(m.swayPitchDeg - (pitch - gun.lastPitchDeg) * vm.swayDegPerDeg,
                                                -vm.swayMaxDeg, vm.swayMaxDeg);
                }
                const auto n = static_cast<float>(std::count(m_shots.begin(), m_shots.end(), owner));
                m.kickBack += n * vm.kickBack;
                m.kickPitchDeg += n * vm.kickPitchDeg;
                m.rounds += static_cast<uint64_t>(n);
                // Bob: one cycle a bobStride walked on the ground; its weight eases in and out.
                float target = 0.0f;
                if (const auto* ps = reg.try_get<PlayerState>(owner); ps && ps->onGround) {
                    const float speed = glm::length(glm::vec2(ps->velocity.x, ps->velocity.z));
                    if (speed > 0.05f) {
                        m.bobPhase = std::fmod(m.bobPhase + speed * dt / std::max(vm.bobStride, 0.01f), 1.0f);
                        target = 1.0f;
                    }
                }
                m.bobWeight += (target - m.bobWeight) * (1.0f - std::exp(-10.0f * dt));
            }
            gun.hasView = true;
            gun.lastYawDeg = yaw;
            gun.lastPitchDeg = pitch;

            // ADS: the drawn aim follows the step's WeaponState.aim at the weapon's own rate (1 / adsTime a
            // second), so a frame between steps is never ahead of the simulation and never overshoots; the
            // view narrows from the camera's field of view toward kAdsFovDeg with it, in live frames only.
            const float sceneFov = reg.get<Camera>(camera).fovDeg;
            const auto* ws = reg.try_get<WeaponState>(owner);
            const auto* weapon = reg.try_get<Weapon>(owner);
            const float aim = ws ? ws->aim : 0.0f;
            if (!live || restarted) {
                gun.drawnAim = aim;
            } else {
                const float by = weapon && weapon->adsTime > 0.0f ? dt / weapon->adsTime : 1.0f;
                gun.drawnAim = std::clamp(aim, gun.drawnAim - by, gun.drawnAim + by);
            }
            float fov = sceneFov;
            if (live && gun.drawnAim > 0.0f) fov = sceneFov + (std::min(sceneFov, kAdsFovDeg) - sceneFov) * gun.drawnAim;
            if (fov != sceneFov) {
                reg.emplace_or_replace<DrawnFov>(camera, DrawnFov{fov});
                m_narrowed.push_back(camera);
            }

            // The motion in view space, about the gun's own origin; then the remap.
            const glm::mat4 local = t.matrix();   // the gun's pose in its parent's (the view's) space
            const glm::vec3 pivot(local[3]);
            const float twoPi = 6.28318530718f;
            const glm::vec3 offset(m.bobWeight * vm.bobAmplitude * 0.5f * std::sin(0.5f * twoPi * m.bobPhase),
                                   m.bobWeight * vm.bobAmplitude * std::sin(twoPi * m.bobPhase), m.kickBack);
            glm::mat4 motion = glm::translate(glm::mat4(1.0f), offset + pivot);
            motion = glm::rotate(motion, glm::radians(m.swayYawDeg), glm::vec3(0, 1, 0));
            motion = glm::rotate(motion, glm::radians(m.swayPitchDeg + m.kickPitchDeg), glm::vec3(1, 0, 0));
            motion = glm::translate(motion, -pivot);
            const glm::mat4 toDrawn = view * viewModelRemap(motion, fov, vm.fovDeg, vm.depthScale);

            // The gun and its subtree, each from its pose in the view's space (a JointAttach child is
            // drawn at its joint by the pose system, after this).
            const auto* gp = reg.try_get<Parent>(e);
            const bool directChild = gp && gp->entity == camera;
            m_stack.clear();
            m_stack.emplace_back(e, directChild ? local : glm::inverse(view) * reg.get<WorldTransform>(e).world);
            while (!m_stack.empty()) {
                const auto [x, inView] = m_stack.back();
                m_stack.pop_back();
                setDrawn(reg, x, toDrawn * inView);
                if (const auto* ch = reg.try_get<Children>(x))
                    for (const entt::entity c : ch->entities)
                        if (const auto* ct = reg.valid(c) ? reg.try_get<Transform>(c) : nullptr; ct && !reg.all_of<JointAttach>(c))
                            m_stack.emplace_back(c, inView * ct->matrix());
            }
        }

        // A camera no gun narrowed this frame draws at its own field of view.
        m_wide.clear();
        for (const entt::entity c : reg.view<DrawnFov>())
            if (std::find(m_narrowed.begin(), m_narrowed.end(), c) == m_narrowed.end()) m_wide.push_back(c);
        for (const entt::entity c : m_wide) reg.remove<DrawnFov>(c);

        // What it drew last frame and no longer draws (a ViewModel removed) goes back to the step's pose,
        // unless something else drew it this frame.
        for (auto it = st.drawn.begin(); it != st.drawn.end();) {
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
            it = st.drawn.erase(it);
        }
        for (auto it = st.guns.begin(); it != st.guns.end();)
            it = reg.valid(it->first) && reg.all_of<ViewModel>(it->first) ? std::next(it) : st.guns.erase(it);
    }

private:
    // x drawn at `world` this frame. Its previous pose is what this system drew last frame (or, new,
    // the pose itself: no motion). Tagged for the render extract only when the drawn pose changed.
    void setDrawn(entt::registry& reg, entt::entity x, const glm::mat4& world) {
        if (!reg.valid(x) || !reg.all_of<WorldTransform>(x)) return;
        m_seen.push_back(x);
        auto& drawn = m_state->drawn;
        const auto last = drawn.find(x);
        const glm::mat4 prev = last != drawn.end() ? last->second : world;
        const auto* rp = reg.try_get<RenderPose>(x);
        if (!rp || rp->world != world || rp->prevWorld != prev) {
            reg.emplace_or_replace<RenderPose>(x, RenderPose{world, prev});
            reg.emplace_or_replace<RenderDirty>(x);
        }
        drawn[x] = world;
    }

    std::shared_ptr<ViewModelState> m_state;
    std::vector<entt::entity> m_shots;
    std::vector<entt::entity> m_seen;
    std::vector<entt::entity> m_narrowed, m_wide;   // cameras with a DrawnFov this frame, and the rest
    std::vector<std::pair<entt::entity, glm::mat4>> m_stack;
};

} // namespace

namespace {
// Every ViewModel is drawn first person (the engine's FirstPerson), from the moment it exists to its removal.
void addFirstPerson(entt::registry& reg, entt::entity e) { reg.emplace_or_replace<FirstPerson>(e); }
void removeFirstPerson(entt::registry& reg, entt::entity e) { reg.remove<FirstPerson>(e); }
} // namespace

void installViewModel(World& world) {
    auto& reg = world.registry();
    if (reg.ctx().contains<ViewModelInstalled>()) return;
    reg.on_construct<ViewModel>().connect<&addFirstPerson>();
    reg.on_destroy<ViewModel>().connect<&removeFirstPerson>();
    for (const entt::entity e : reg.view<ViewModel>()) reg.emplace_or_replace<FirstPerson>(e);
    auto state = std::make_shared<ViewModelState>();
    reg.ctx().emplace<ViewModelInstalled>(ViewModelInstalled{state});
    auto system = std::make_shared<ViewModelSystem>(state);
    // The gun follows the drawn camera, and the arms' pose follows the gun.
    world.addSync("view_model", [system](World& w) { system->run(w); }, {.after = {"interpolate"}, .before = {"pose"}});
}

const ViewModelMotion* viewModelMotion(const World& world, entt::entity gun) {
    const auto* installed = world.registry().ctx().find<ViewModelInstalled>();
    if (!installed) return nullptr;
    const auto it = installed->state->guns.find(gun);
    return it != installed->state->guns.end() ? &it->second.motion : nullptr;
}

} // namespace ohao::slice
