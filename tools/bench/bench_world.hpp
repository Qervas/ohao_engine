#pragma once
// bench10k / bench100k, the scenes every benchmark measures (scene_bench, serve_bench):
// built in code from a fixed seed, so the same build always measures the same world.
#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace ohao::bench {

// n entities with Name and Transform (random position, rotation and scale) and a hierarchy
// up to 8 deep (about 60% of the entities have a parent among the 64 created just before
// them). 99% are renderables (a MeshSource primitive and a MaterialOverride, from
// palettes), 1% lights of every type. Then a camera and an environment entity (n + 2 in
// all).
inline void buildBenchWorld(ecs::World& w, int n, uint64_t seed) {
    using namespace ohao::ecs;
    w.seedIds(seed);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> pos(-50.0f, 50.0f), unit(-1.0f, 1.0f), ang(0.0f, 6.2831853f),
        scale(0.25f, 4.0f), pos01(0.0f, 1.0f);
    std::vector<entt::entity> es;
    std::vector<uint8_t> depth;
    es.reserve(static_cast<std::size_t>(n));
    const int lights = n / 100;
    for (int i = 0; i < n; ++i) {
        const bool isLight = i >= n - lights;
        Transform t;
        t.position = {pos(rng), pos(rng), pos(rng)};
        t.rotation = glm::angleAxis(ang(rng), glm::normalize(glm::vec3(unit(rng), unit(rng), unit(rng)) + 1e-3f));
        t.scale = glm::vec3(scale(rng));
        const auto e = w.create((isLight ? "Light_" : "Mesh_") + std::to_string(i), t);
        uint8_t d = 0;
        if (i > 0 && rng() % 5 < 3) {
            const std::size_t p = es.size() - 1 - rng() % std::min<std::size_t>(64, es.size());
            if (depth[p] < 8 && w.setParent(e, es[p])) d = static_cast<uint8_t>(depth[p] + 1);
        }
        if (isLight) {
            Light l;
            l.type = static_cast<LightType>(rng() % 4);
            l.color = {pos01(rng), pos01(rng), pos01(rng)};
            l.intensity = pos01(rng) * 20.0f;
            l.range = 5.0f + pos01(rng) * 40.0f;
            w.registry().emplace<Light>(e, l);
        } else {
            // A renderable: a primitive and a material. Authored scenes reuse sizes and
            // materials, so both come from small palettes (the transforms stay random).
            static constexpr float kSizes[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
            const float s = kSizes[rng() % std::size(kSizes)];
            MeshSource ms;
            switch (rng() % 6) {
            case 0: ms.source = SpherePrim{s, 32, 16}; break;
            case 1: ms.source = CubePrim{s}; break;
            case 2: ms.source = PlatformPrim{s * 2.0f, 0.4f, s * 2.0f}; break;
            case 3: ms.source = CylinderPrim{s * 0.5f, s, 24}; break;
            case 4: ms.source = ConePrim{s * 0.5f, s, 24}; break;
            default: ms.source = BoxPrim{glm::vec3(-s), glm::vec3(s), glm::vec3(0.73f)}; break;
            }
            w.registry().emplace<MeshSource>(e, ms);
            static constexpr float kChannel[] = {0.05f, 0.12f, 0.25f, 0.45f, 0.65f, 0.73f, 0.85f, 0.95f};
            const auto c = [&] { return kChannel[rng() % std::size(kChannel)]; };
            static constexpr float kRoughness[] = {0.05f, 0.2f, 0.35f, 0.5f, 0.75f, 0.95f};
            w.registry().emplace<MaterialOverride>(
                e, MaterialOverride{{c(), c(), c()}, kRoughness[rng() % std::size(kRoughness)],
                                    rng() % 5 == 0 ? 1.0f : 0.0f, 0.0f});
        }
        es.push_back(e);
        depth.push_back(d);
    }
    // The spec's one camera and one environment, after the n entities (so they leave the
    // random stream above untouched).
    Transform cam;
    cam.position = {0.0f, 20.0f, 120.0f};
    cam.rotation = glm::angleAxis(glm::radians(-9.5f), glm::vec3(1.0f, 0.0f, 0.0f));
    const auto c = w.create("MainCamera", cam);
    w.registry().emplace<ecs::Camera>(c, ecs::Camera{50.0f, 0.1f, 500.0f, true});
    const auto env = w.create("Environment");
    w.registry().emplace<Environment>(env, Environment{AssetRef{"assets/env/bench_sky.hdr", {}}, 1.25f});
}

} // namespace ohao::bench
