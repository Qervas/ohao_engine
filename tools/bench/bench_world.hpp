#pragma once
// bench10k / bench100k, the scenes every benchmark measures (scene_bench, serve_bench):
// built in code from a fixed seed, so the same build always measures the same world.
#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <random>
#include <string>
#include <vector>

namespace ohao::bench {

// n entities with Name and Transform (random position, rotation and scale) and a hierarchy
// up to 8 deep (about 60% of the entities have a parent among the 64 created just before
// them). 99% are renderables (a MeshSource primitive and a Material, from
// palettes), 1% lights of every type. Then a camera and an environment entity (n + 2 in
// all).
// A primitive's material: colour, roughness and metalness, the rest OpenPBR's defaults.
inline ecs::Material surface(const glm::vec3& color, float roughness, float metalness = 0.0f) {
    ecs::Material m;
    m.baseColor = color;
    m.specularRoughness = roughness;
    m.baseMetalness = metalness;
    return m;
}

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
            const glm::vec3 color{c(), c(), c()};
            const float roughness = kRoughness[rng() % std::size(kRoughness)];
            w.registry().emplace<Material>(e, surface(color, roughness, rng() % 5 == 0 ? 1.0f : 0.0f));
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

// render_bench's bench10k_1080p / bench100k_1080p (sub-project 2b spec, "Profiling first"): n
// renderables on a square grid 400 m across, each one of 20 primitive meshes (5 shapes x 4
// sizes, so the RenderScene holds 20 meshes instanced n times) with one of 8 materials; 1,000
// point lights over the grid (40 x 25, 15 m range, 4 of them shadowed: clustered lighting's
// scene, 2b step 13) and a sun; the camera at the grid's edge, looking across it. Built from a
// fixed seed, so every build is the same world.
inline void buildRenderBenchWorld(ecs::World& w, int n, uint64_t seed) {
    using namespace ohao::ecs;
    w.seedIds(seed);
    std::mt19937_64 rng(seed);
    static constexpr float kSizes[] = {0.3f, 0.45f, 0.6f, 0.75f};
    static constexpr float kChannel[] = {0.1f, 0.25f, 0.45f, 0.65f, 0.85f};
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const float spacing = 400.0f / static_cast<float>(side);
    for (int i = 0; i < n; ++i) {
        Transform t;
        t.position = {-200.0f + spacing * (0.5f + static_cast<float>(i % side)), 0.0f,
                      -200.0f + spacing * (0.5f + static_cast<float>(i / side))};
        t.rotation = glm::angleAxis(static_cast<float>(rng() % 360) * 0.0174532925f, glm::vec3(0.0f, 1.0f, 0.0f));
        const auto e = w.create("Mesh_" + std::to_string(i), t);
        const float s = kSizes[rng() % std::size(kSizes)];
        MeshSource ms;
        switch (rng() % 5) {
        case 0: ms.source = SpherePrim{s, 16, 8}; break;
        case 1: ms.source = CubePrim{s * 2.0f}; break;
        case 2: ms.source = PlatformPrim{s * 2.0f, s, s * 2.0f}; break;
        case 3: ms.source = CylinderPrim{s, s * 2.0f, 12}; break;
        default: ms.source = ConePrim{s, s * 2.0f, 12}; break;
        }
        w.registry().emplace<MeshSource>(e, ms);
        const auto c = [&] { return kChannel[rng() % std::size(kChannel)]; };
        const glm::vec3 color{c(), c(), c()};
        w.registry().emplace<Material>(e, surface(color, 0.6f));
    }
    // The ground under the grid.
    Transform ground;
    const auto g = w.create("Ground", ground);
    w.registry().emplace<MeshSource>(g, MeshSource{PlatformPrim{420.0f, 0.2f, 420.0f}});
    w.registry().emplace<Material>(g, surface({0.5f, 0.5f, 0.5f}, 0.9f));
    static constexpr glm::vec3 kLightColors[] = {{1.0f, 0.9f, 0.8f}, {0.8f, 0.9f, 1.0f}, {1.0f, 0.7f, 0.5f}, {0.7f, 1.0f, 0.8f}};
    for (int i = 0; i < 1000; ++i) {
        Transform lt;
        lt.position = {-195.0f + 10.0f * static_cast<float>(i % 40), 6.0f, -192.0f + 16.0f * static_cast<float>(i / 40)};
        const auto l = w.create("Light_" + std::to_string(i), lt);
        Light light;
        light.color = kLightColors[i % std::size(kLightColors)];
        light.intensity = 60.0f;
        light.range = 15.0f;
        light.castShadows = i < 4;
        w.registry().emplace<Light>(l, light);
    }
    Transform sunT;
    sunT.rotation = glm::angleAxis(glm::radians(-50.0f), glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)));
    const auto sun = w.create("Sun", sunT);
    Light sunLight;
    sunLight.type = LightType::Directional;
    sunLight.intensity = 2.0f;
    sunLight.radius = 0.00465f;
    w.registry().emplace<Light>(sun, sunLight);
    Transform cam;
    cam.position = {0.0f, 25.0f, 215.0f};
    cam.rotation = glm::angleAxis(glm::radians(-12.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const auto c = w.create("MainCamera", cam);
    w.registry().emplace<ecs::Camera>(c, ecs::Camera{60.0f, 0.1f, 1000.0f, true});
    const auto env = w.create("Environment");
    w.registry().emplace<Environment>(env, Environment{});
}

// The crowd (step 4b-0): the play scene with physics_bench's 1,000-box pile and 64 enemies, the player
// firing. `play` is assets/scenes/blockout_play.scene.json's text; the crowd's entities are appended to
// it (and their types to its header). Empty when the text is not that scene's shape.
// - The player stands outside the room's south doorway, at (1.5, 0, 6), facing south (+Z), with health
//   that does not run out and a 1,000-round magazine (100 s of fire; no recoil, so its aim stays level).
// - The 64 enemies (the play scene's EnemyA, each with its body, head, rifle and muzzle) stand on an
//   8 x 8 grid 1.2 m apart south of it (x -5 to 3.4, z 11 to 19.4), facing it, with health that does not
//   run out; each walks to 4 m of the player and fires.
// - The pile: physics_bench's walled pit (a 6 x 6 m floor, walls 3 m high) centred at (7.5, 0, 15), and
//   1,000 dynamic 0.4 m boxes, 10 x 10 x 10 and 0.5 m apart, dropped from 0.5 to 5 m above its floor.
// Hosts hold the trigger with an `input.inject` of {"action": "fire", "down": true} (kCrowdFire); writeCrowd
// writes that as a script for `ohao run --script`.
constexpr int kCrowdEnemies = 64;
constexpr int kCrowdBoxes = 1000;
constexpr const char* kCrowdFire = R"({"events":[{"action":"fire","down":true}]})";

inline std::string crowdScene(std::string play) {
    const auto header = play.find("\"components\": {");
    const auto end = play.rfind("\n  ]");
    const auto player = play.find("\"PlayerController\": {},\n      \"Health\": {},\n      \"Weapon\": {}");
    if (header == std::string::npos || end == std::string::npos || player == std::string::npos) return {};
    const auto num = [](float v) {
        char s[32];
        std::snprintf(s, sizeof s, "%.4g", static_cast<double>(v));
        return std::string(s);
    };
    const auto hex = [](uint64_t v) {
        char s[17];
        std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(v));
        return std::string(s);
    };
    std::string add;
    for (int i = 0; i < kCrowdEnemies; ++i) {
        const uint64_t id = 0xc40d0e0000000000ull | (static_cast<uint64_t>(i) << 8);
        const std::string self = hex(id), gun = hex(id | 3), name = "Crowd" + std::to_string(i);
        const float x = -5.0f + 1.2f * static_cast<float>(i % 8), z = 11.0f + 1.2f * static_cast<float>(i / 8);
        add += ",\n    { \"id\": \"" + self + "\", \"Name\": { \"value\": \"" + name + "\" }, \"Transform\": { \"position\": [" +
               num(x) + ", 0, " + num(z) +
               "] }, \"PlayerController\": { \"walkSpeed\": 2.5, \"spawn\": \"\", \"input\": false }, \"Health\": { \"max\": "
               "1000000, \"current\": 1000000 }, \"Weapon\": { \"damage\": 8, \"fireRate\": 4, \"magazine\": 12, "
               "\"reloadTime\": 2, \"recoilPitchDeg\": 0, \"recoilYawDeg\": 0, \"seed\": " + std::to_string(101 + i) +
               " }, \"Enemy\": { \"attackRange\": 4 }, \"MeshSource\": { \"source\": { \"ModelAsset\": { \"asset\": { \"path\": "
               "\"assets/characters/toon_enemy/enemy.glb\" }, \"mesh\": \"Enemy\" } } }, \"SkinnedMesh\": { \"file\": { \"path\": "
               "\"assets/characters/toon_enemy/enemy.glb\" }, \"skin\": \"EnemySkin\" }, \"Animator\": { \"graph\": { \"path\": "
               "\"assets/anim/enemy.animgraph.json\" } } }";
        add += ",\n    { \"id\": \"" + hex(id | 1) + "\", \"Name\": { \"value\": \"" + name + " Body\" }, \"Parent\": { \"entity\": \"" +
               self + "\" } }";
        add += ",\n    { \"id\": \"" + hex(id | 2) + "\", \"Name\": { \"value\": \"" + name + " Head\" }, \"Parent\": { \"entity\": \"" +
               self + "\" } }";
        add += ",\n    { \"id\": \"" + gun + "\", \"Name\": { \"value\": \"" + name + " Gun\" }, \"Parent\": { \"entity\": \"" + self +
               "\" }, \"MeshSource\": { \"source\": { \"ModelAsset\": { \"asset\": { \"path\": "
               "\"assets/characters/toon_enemy/rifle.glb\" } } } }, \"JointAttach\": { \"joint\": \"Index1.R\" } }";
        add += ",\n    { \"id\": \"" + hex(id | 4) + "\", \"Name\": { \"value\": \"" + name +
               " Muzzle\" }, \"Transform\": { \"position\": [0.173, 0.945, -0.039] }, \"Parent\": { \"entity\": \"" + gun +
               "\" }, \"Light\": { \"color\": [1, 0.72, 0.38], \"intensity\": 0, \"range\": 6, \"radius\": 0.03, "
               "\"castShadows\": false }, \"Muzzle\": {} }";
    }
    const float px = 7.5f, pz = 15.0f;
    const auto box = [&](uint64_t id, const std::string& name, glm::vec3 at, glm::vec3 half, const char* color, bool dynamic) {
        add += ",\n    { \"id\": \"" + hex(id) + "\", \"Name\": { \"value\": \"" + name + "\" }, \"Transform\": { \"position\": [" +
               num(at.x) + ", " + num(at.y) + ", " + num(at.z) + "] }, \"MeshSource\": { \"source\": { \"BoxPrim\": { \"lo\": [" +
               num(-half.x) + ", " + num(-half.y) + ", " + num(-half.z) + "], \"hi\": [" + num(half.x) + ", " + num(half.y) + ", " +
               num(half.z) + "], \"color\": " + color + " } } }, \"Material\": { \"base_color\": " + color +
               ", \"specular_roughness\": 0.7 }, \"Collider\": {}" +
               (dynamic ? ", \"RigidBody\": { \"motion\": \"Dynamic\", \"mass\": 5 }" : "") + " }";
    };
    const char* grey = "[0.5, 0.5, 0.52]";
    box(0xc40da10000000001ull, "PitFloor", {px, -0.5f, pz}, {3.5f, 0.5f, 3.5f}, grey, false);
    box(0xc40da10000000002ull, "PitWallN", {px, 1.5f, pz - 3.25f}, {3.5f, 1.5f, 0.25f}, grey, false);
    box(0xc40da10000000003ull, "PitWallS", {px, 1.5f, pz + 3.25f}, {3.5f, 1.5f, 0.25f}, grey, false);
    box(0xc40da10000000004ull, "PitWallW", {px - 3.25f, 1.5f, pz}, {0.25f, 1.5f, 3.0f}, grey, false);
    box(0xc40da10000000005ull, "PitWallE", {px + 3.25f, 1.5f, pz}, {0.25f, 1.5f, 3.0f}, grey, false);
    for (int i = 0; i < kCrowdBoxes; ++i) {
        const glm::vec3 at(px - 2.25f + 0.5f * float(i % 10), 0.5f + 0.5f * float(i / 100), pz - 2.25f + 0.5f * float((i / 10) % 10));
        box(0xc40db00000000000ull | static_cast<uint64_t>(i), "PileBox" + std::to_string(i), at, {0.2f, 0.2f, 0.2f},
            i % 3 == 0 ? "[0.8, 0.45, 0.2]" : i % 3 == 1 ? "[0.25, 0.45, 0.8]" : "[0.7, 0.7, 0.65]", true);
    }
    play.insert(end, add);
    play.replace(player, std::string("\"PlayerController\": {},\n      \"Health\": {},\n      \"Weapon\": {}").size(),
                 "\"Transform\": { \"position\": [1.5, 0, 6], \"rotation\": [0, 1, 0, 0] },\n      \"PlayerController\": { "
                 "\"spawn\": \"\" },\n      \"Health\": { \"max\": 1000000, \"current\": 1000000 },\n      \"Weapon\": { "
                 "\"magazine\": 1000, \"recoilPitchDeg\": 0, \"recoilYawDeg\": 0 }");
    play.insert(header + 15, "\n    \"Collider\": 1,\n    \"RigidBody\": 1,");
    return play;
}

// build/render_bench/crowd.scene.json and crowd.fire.jsonl under `root` (the repository): the crowd, and the
// script that holds its trigger (`ohao run build/render_bench/crowd.scene.json --script
// build/render_bench/crowd.fire.jsonl --frames K --uncapped`). False when either cannot be written.
inline bool writeCrowd(const std::filesystem::path& root) {
    std::ifstream in(root / "assets/scenes/blockout_play.scene.json", std::ios::binary);
    std::stringstream play;
    play << in.rdbuf();
    const std::string text = crowdScene(play.str());
    const std::filesystem::path dir = root / "build/render_bench";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream scene(dir / "crowd.scene.json", std::ios::binary | std::ios::trunc);
    scene << text;
    std::ofstream script(dir / "crowd.fire.jsonl", std::ios::binary | std::ios::trunc);
    script << "{\"method\": \"input.inject\", \"params\": " << kCrowdFire << "}\n";
    return !text.empty() && scene && script;
}

} // namespace ohao::bench
