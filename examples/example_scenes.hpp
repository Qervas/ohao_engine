#pragma once
// The worlds cornell_box and model_viewer render, built as authoring data only (Camera,
// Environment, MeshSource, Material, Light): no GPU, so --save-scene can write
// them without a renderer, and tests can build them. Instantiate turns the meshes into
// MeshRenderers; runtime/scene_view_apply.hpp applies the camera and environment.
//
// Creation order is draw order and light order, so the builders keep the order the
// examples always used (camera and environment first: they draw and light nothing).
#include "ecs/components.hpp"
#include "ecs/framing.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/spawn.hpp"
#include "ecs/world.hpp"
#include "render/camera/scene_framer.hpp"
#include "asset/model.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ohao::examples {

// --save-scene seeds the id generator with these, so the files are reproducible.
inline constexpr uint64_t kCornellBoxIdSeed = 0xc0e11b0c5ce0e001ull;
inline constexpr uint64_t kModelViewerIdSeed = 0x30de17e3e5ce0e01ull;

inline entt::entity addCamera(ecs::World& world, const glm::vec3& position, float fovDeg) {
    ecs::Transform t;
    t.position = position;
    const entt::entity e = world.create("MainCamera", t);
    ecs::Camera cam;
    cam.fovDeg = fovDeg;
    world.registry().emplace<ecs::Camera>(e, cam);
    return e;
}

inline entt::entity addEnvironment(ecs::World& world, std::optional<ecs::AssetRef> hdri) {
    const entt::entity e = world.create("Environment");
    world.registry().emplace<ecs::Environment>(e, ecs::Environment{std::move(hdri), 1.0f});
    return e;
}

inline entt::entity addMesh(ecs::World& world, std::string_view name, ecs::MeshSource source,
                            const ecs::Material& material, const ecs::Transform& t = {}) {
    const entt::entity e = world.create(name, t);
    auto& reg = world.registry();
    reg.emplace<ecs::MeshSource>(e, std::move(source));
    reg.emplace<ecs::Material>(e, material);
    return e;
}

inline ecs::Material surface(const glm::vec3& color, float roughness, float metallic = 0.0f) {
    ecs::Material m;
    m.baseColor = color;
    m.specularRoughness = roughness;
    m.baseMetalness = metallic;
    return m;
}

// The classic Cornell box: five walls, a metal and a glass sphere, twelve point lights.
inline void buildCornellBox(ecs::World& world) {
    addCamera(world, {0.0f, 0.0f, 13.0f}, 38.0f);
    addEnvironment(world, std::nullopt);

    const float S = 5.0f;
    const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
    const glm::vec3 LBB(-S, -S, -S), RBB(S, -S, -S), LTB(-S, S, -S), RTB(S, S, -S);
    const glm::vec3 LBF(-S, -S, S), RBF(S, -S, S), LTF(-S, S, S), RTF(S, S, S);
    const auto wall = [&](std::string_view name, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                          glm::vec3 normal, glm::vec3 color) {
        addMesh(world, name, ecs::MeshSource{ecs::QuadPrim{a, b, c, d, normal, color}}, surface(color, 0.95f));
    };
    wall("Back", LBB, RBB, RTB, LTB, {0, 0, 1}, white);
    wall("Left", LBB, LTB, LTF, LBF, {1, 0, 0}, red);
    wall("Right", RBB, RBF, RTF, RTB, {-1, 0, 0}, green);
    wall("Floor", LBB, LBF, RBF, RBB, {0, 1, 0}, white);
    wall("Ceiling", LTB, RTB, RTF, LTF, {0, -1, 0}, white);

    // Two unit-diameter UV spheres (32 x 16), sharing one mesh.
    ecs::Transform metal;
    metal.position = {-2.0f, -S + 2.0f, 0.0f};
    metal.scale = glm::vec3(2.0f);
    addMesh(world, "MetalSphere", ecs::MeshSource{ecs::SpherePrim{}}, surface({0.95f, 0.93f, 0.88f}, 0.05f, 1.0f),
            metal);
    ecs::Transform glass;
    glass.position = {2.5f, -S + 1.8f, 1.5f};
    glass.scale = glm::vec3(1.8f);
    addMesh(world, "GlassSphere", ecs::MeshSource{ecs::SpherePrim{}}, surface({0.9f, 0.95f, 1.0f}, 0.02f), glass);

    // Many lights: a stress test for ReSTIR.
    const glm::vec3 colors[] = {
        {1.0f, 0.3f, 0.2f}, {0.2f, 1.0f, 0.3f}, {0.3f, 0.4f, 1.0f}, {1.0f, 0.9f, 0.3f},
        {1.0f, 0.5f, 0.0f}, {0.8f, 0.2f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.5f},
        {1.0f, 1.0f, 1.0f}, {0.5f, 1.0f, 0.5f}, {1.0f, 0.7f, 0.5f}, {0.5f, 0.7f, 1.0f},
    };
    const glm::vec3 positions[] = {
        {-3, 4, -3}, {0, 4, -3}, {3, 4, -3}, {-3, 4, 0}, {0, 4, 0}, {3, 4, 0},
        {-3, 4, 3},  {0, 4, 3},  {3, 4, 3},  {-4, 0, 0}, {4, 0, 0}, {0, 0, -4},
    };
    for (int i = 0; i < 12; ++i) {
        ecs::Light light;
        light.type = ecs::LightType::Point;
        light.color = colors[i];
        light.intensity = 5.0f;
        light.radius = 0.3f;
        ecs::Transform t;
        t.position = positions[i];
        ecs::spawnLight(world, "Light" + std::to_string(i), light, t);
    }
}

// The material the viewer gives a model. Its own materials are used when it has them;
// this only feeds the legacy RT GI word (and is the material of a model without any).
// A single-material model keeps the old viewer's 0.8/0.7/0.6; a multi-material one uses
// its own material 0.
inline ecs::Material viewerModelMaterial(const Model& model) {
    const bool multiple = !model.materialPerTriangle.empty() &&
        *std::max_element(model.materialPerTriangle.begin(), model.materialPerTriangle.end()) > 0;
    if (multiple && !model.materialColors.empty())
        return surface(glm::vec3(model.materialColors[0]), model.materialColors[0].w,
                       model.materialMetallic.empty() ? 0.0f : model.materialMetallic[0]);
    return surface({0.8f, 0.7f, 0.6f}, 0.5f);
}

struct ViewerScene {
    entt::entity model{entt::null};
    ecs::Transform modelTransform;
};

// A bedroom sized to the model (SceneFramer), the model, the studio HDRI and four
// framed lights. `modelAsset` is the model's project-relative path; without one the
// model entity gets no MeshSource and the caller attaches the in-memory model.
inline ViewerScene buildModelViewerScene(ecs::World& world, const Model& model, const FrameResult& frame,
                                         std::optional<ecs::AssetRef> modelAsset, bool flipY) {
    ViewerScene out;
    addCamera(world, frame.cameraPosition, frame.cameraFov);
    addEnvironment(world, ecs::AssetRef{"assets/test_models/env_studio.hdr", {}});

    const float S = frame.roomHalfSize;
    const glm::vec3 wallColor(0.55f, 0.50f, 0.45f);    // muted warm gray walls
    const glm::vec3 floorColor(0.18f, 0.10f, 0.05f);   // dark walnut wood
    const glm::vec3 ceilColor(0.60f, 0.58f, 0.55f);    // gray ceiling
    const auto quad = [&](std::string_view name, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                          glm::vec3 normal, glm::vec3 color, float roughness) {
        addMesh(world, name, ecs::MeshSource{ecs::QuadPrim{a, b, c, d, normal, color}}, surface(color, roughness));
    };
    const auto box = [&](std::string_view name, glm::vec3 lo, glm::vec3 hi, glm::vec3 color, float roughness) {
        addMesh(world, name, ecs::MeshSource{ecs::BoxPrim{lo, hi, color}}, surface(color, roughness));
    };

    // Walls extend 0.1 past the corners so rays do not leak at the seams. No front
    // wall: the camera looks in from outside.
    const float E = 0.1f;
    quad("Back", {-S - E, -S - E, -S}, {S + E, -S - E, -S}, {S + E, S + E, -S}, {-S - E, S + E, -S}, {0, 0, 1},
         wallColor, 0.9f);
    quad("Left", {-S, -S - E, -S - E}, {-S, -S - E, S + E}, {-S, S + E, S + E}, {-S, S + E, -S - E}, {1, 0, 0},
         wallColor, 0.9f);
    quad("Right", {S, -S - E, S + E}, {S, -S - E, -S - E}, {S, S + E, -S - E}, {S, S + E, S + E}, {-1, 0, 0},
         wallColor, 0.9f);
    quad("Floor", {-S - E, -S, -S - E}, {S + E, -S, -S - E}, {S + E, -S, S + E}, {-S - E, -S, S + E}, {0, 1, 0},
         floorColor, 0.95f);
    quad("Ceiling", {-S - E, S, -S - E}, {S + E, S, -S - E}, {S + E, S, S + E}, {-S - E, S, S + E}, {0, -1, 0},
         ceilColor, 0.9f);

    // Bed against the back wall, behind the model.
    const float bedW = S * 0.8f, bedH = S * 0.25f, bedD = S * 0.6f;
    const glm::vec3 bedColor(0.85f, 0.82f, 0.78f);     // linen white sheets
    const glm::vec3 frameColor(0.20f, 0.12f, 0.06f);   // dark wood frame
    box("BedFrame", {-bedW, -S, -S}, {bedW, -S + bedH * 0.4f, -S + bedD}, frameColor, 0.5f);
    box("Mattress", {-bedW + 0.2f, -S + bedH * 0.4f, -S + 0.2f}, {bedW - 0.2f, -S + bedH, -S + bedD - 0.2f}, bedColor,
        0.95f);
    box("Headboard", {-bedW, -S, -S}, {bedW, -S + bedH * 2.5f, -S + 0.3f}, frameColor, 0.4f);
    box("Pillow", {-bedW * 0.4f, -S + bedH, -S + 0.4f}, {bedW * 0.4f, -S + bedH + 1.0f, -S + bedD * 0.4f},
        {0.92f, 0.90f, 0.88f}, 0.95f);
    // Nightstand on the right.
    const float nsX = S * 0.55f;
    box("Nightstand", {nsX, -S, -S}, {nsX + 3.0f, -S + bedH * 1.5f, -S + 3.0f}, {0.22f, 0.14f, 0.07f}, 0.45f);

    // The model: one entity, one mesh with a submesh per material run.
    out.modelTransform.position = frame.modelPosition;
    out.modelTransform.rotation = flipY ? glm::quat(glm::radians(glm::vec3(0, 180, 0))) : frame.modelRotation;
    out.modelTransform.scale = glm::vec3(frame.modelScale);
    out.model = world.create("Model", out.modelTransform);
    if (modelAsset)
        world.registry().emplace<ecs::MeshSource>(out.model, ecs::MeshSource{ecs::ModelAsset{std::move(*modelAsset)}});
    world.registry().emplace<ecs::Material>(out.model, viewerModelMaterial(model));

    ecs::spawnFramedLights(world, frame);
    return out;
}

// --save-scene: the world as a canonical pretty scene file. Returns the exit code.
inline int writeExampleScene(const ecs::World& world, const std::string& path) {
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::SaveReport report;
    const auto saved = ecs::saveSceneFile(world, types, path, {}, &report);
    if (!report.warnings.empty()) std::cerr << "warnings: " << ecs::diagnosticsJson(report.warnings) << "\n";
    if (!saved) {
        std::cerr << "FATAL: " << ecs::diagnosticsJson(std::span(&saved.error(), 1)) << "\n";
        return 1;
    }
    std::cout << "Saved scene: " << path << " (" << world.idCount() << " entities)\n";
    return 0;
}

} // namespace ohao::examples
