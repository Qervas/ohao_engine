#include "ecs/scene_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace ohao::ecs {

namespace {

// The first entity (in creation order) holding T and passing `keep`.
template <class T, class Keep = bool (*)(const T&)>
entt::entity firstWith(const entt::registry& reg, Keep keep = [](const T&) { return true; }) {
    entt::entity best = entt::null;
    uint64_t bestSeq = UINT64_MAX;
    for (const entt::entity e : reg.view<const T>()) {
        if (!keep(reg.get<T>(e))) continue;
        const auto* co = reg.try_get<CreationOrder>(e);
        const uint64_t seq = co ? co->seq : UINT64_MAX - 1;
        if (best == entt::null || seq < bestSeq) {
            best = e;
            bestSeq = seq;
        }
    }
    return best;
}

// parentWorld * local down the chain, root first: the transform system's arithmetic,
// so the result matches WorldTransform bit for bit once the system has run.
glm::mat4 worldMatrix(const entt::registry& reg, entt::entity e) {
    std::vector<entt::entity> chain;
    for (entt::entity at = e; reg.valid(at) && chain.size() < 4096;) {
        chain.push_back(at);
        const auto* p = reg.try_get<Parent>(at);
        at = p ? p->entity : entt::null;
    }
    glm::mat4 world(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (const auto* t = reg.try_get<Transform>(*it)) world = world * t->matrix();
    return world;
}

} // namespace

glm::vec2 pitchYawDegrees(const glm::vec3& direction) {
    const double x = direction.x, y = direction.y, z = direction.z;
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.0)) return {0.0f, -90.0f};
    constexpr double kDeg = 180.0 / std::numbers::pi;
    const double pitch = std::asin(std::clamp(y / len, -1.0, 1.0)) * kDeg;
    const double yaw = std::atan2(z, x) * kDeg;
    // + 0.0f turns -0 into +0: a -0 pitch would give the renderer a -0 front.y.
    return {static_cast<float>(pitch) + 0.0f, static_cast<float>(yaw) + 0.0f};
}

SceneView resolveSceneView(const World& world) {
    const entt::registry& reg = world.registry();
    SceneView view;

    // The first active camera: an inactive one (a level's, beside a player's head) never hides it.
    if (const entt::entity e = firstWith<Camera>(reg, [](const Camera& c) { return c.active; }); e != entt::null) {
        const Camera& cam = reg.get<Camera>(e);
        {
            // Where the frame draws it (render interpolation), else its step pose.
            const auto* drawn = reg.try_get<RenderPose>(e);
            const glm::mat4 w = drawn ? drawn->world : worldMatrix(reg, e);
            const glm::vec2 py = pitchYawDegrees(-glm::vec3(w[2]));
            view.camera = CameraView{glm::vec3(w[3]), py.x, py.y, cam.fovDeg, cam.nearPlane, cam.farPlane};
        }
    }
    if (const entt::entity e = firstWith<Environment>(reg); e != entt::null) {
        const Environment& env = reg.get<Environment>(e);
        view.environment = EnvironmentView{env.hdri, env.intensity};
    }
    return view;
}

} // namespace ohao::ecs
