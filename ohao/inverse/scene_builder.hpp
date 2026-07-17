#pragma once

// Product-studio / Cornell inverse scene builders + θ apply.

#include "inverse/fit_config.hpp"

#include "render/camera/camera.hpp"
#include "scene/actor/actor.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/component/light_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::inverse {

inline void addQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color,
             glm::vec2 uvA = {0, 0}, glm::vec2 uvB = {1, 0},
             glm::vec2 uvC = {1, 1}, glm::vec2 uvD = {0, 1}) {
    auto mk = [&](glm::vec3 pos, glm::vec2 uv) {
        Vertex v{};
        v.position = pos;
        v.normal = normal;
        v.color = color;
        v.texCoord = uv;
        v.tangent = {1, 0, 0, 1};
        v.boneIndices = glm::ivec4(0);
        v.boneWeights = {1, 0, 0, 0};
        return v;
    };
    verts = {mk(a, uvA), mk(b, uvB), mk(c, uvC), mk(d, uvD)};
    inds = {0, 1, 2, 0, 2, 3};
}

inline void addWall(Scene* scene, std::string_view name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color, float rough = 0.95f, float metal = 0.0f) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model);
    mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = rough;
    mat->getMaterial().metallic = metal;
}

// Uniform metal-rough PBR on one surface (not textures).
struct PbrParams {
    glm::vec3 albedo{0.7f, 0.7f, 0.7f};
    float roughness{0.5f};
    float metallic{0.0f};

    [[nodiscard]] std::vector<double> asTheta() const {
        return {albedo.r, albedo.g, albedo.b, roughness, metallic};
    }
};

inline std::string formatTheta(const std::vector<double>& th) {
    if (th.empty()) return "[]";
    std::string s = "[";
    for (size_t i = 0; i < th.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(th[i]);
    }
    s += "]";
    return s;
}

inline void writeMatPbr(MaterialComponent* mat, Actor* actor, const PbrParams& p) {
    if (!mat) return;
    mat->getMaterial().baseColor = p.albedo;
    mat->getMaterial().roughness = p.roughness;
    mat->getMaterial().metallic = p.metallic;
    if (!actor) return;
    if (auto mesh = actor->getComponent<MeshComponent>()) {
        if (auto model = mesh->getModel()) {
            for (auto& v : model->vertices) v.color = p.albedo;
        }
    }
}

// Multi-surface + light inverse scene.
struct InverseScene {
    std::unique_ptr<Scene> scene;
    // Primary PBR surface (ground / left wall) — uniform mode
    Actor* primaryActor{nullptr};
    MaterialComponent* primaryMat{nullptr};
    // Optional N×N ground albedo tiles (mapGround): each tile is RGB in θ
    std::vector<Actor*> groundTiles;
    std::vector<MaterialComponent*> groundMats;
    bool mapGround{false};
    int mapRes{2};  // N for N×N tiles when mapGround
    std::vector<glm::vec3> truthTiles;
    std::vector<glm::vec3> initTiles;
    // Secondary surface (pedestal top) — albedo only in θ
    Actor* pedestalActor{nullptr};
    MaterialComponent* pedestalMat{nullptr};
    LightComponent* keyLight{nullptr};
    LightComponent* fillLight{nullptr};
    LightComponent* rimLight{nullptr};
    Actor* keyLightActor{nullptr};
    Actor* fillLightActor{nullptr};
    Actor* rimLightActor{nullptr};
    Actor* heroActor{nullptr};
    glm::vec3 baseKeyPos{4.0f, 5.0f, 4.5f};
    glm::vec3 baseFillPos{-3.5f, 3.0f, 3.5f};
    glm::vec3 baseRimPos{-0.5f, 3.5f, -4.5f};
    std::vector<CameraView> baseViews;

    std::string envPath;
    std::string relightEnvPath;
    std::vector<CameraView> views;

    PbrParams truthPrimary{};
    PbrParams initPrimary{};
    PbrParams truthPedestal{.albedo = {0.18f, 0.19f, 0.21f}, .roughness = 0.4f, .metallic = 0.05f};
    PbrParams initPedestal{.albedo = {0.55f, 0.12f, 0.10f}, .roughness = 0.4f, .metallic = 0.05f};
    float truthKeyI{22.0f};
    float initKeyI{7.0f};
    float truthFillI{9.0f};
    float initFillI{3.0f};
    float truthRimI{10.0f};
    float initRimI{4.0f};
    float truthEnvScale{1.0f};
    float initEnvScale{0.45f};
    float currentEnvScale{1.0f};
    float currentExposure{1.0f};
    float truthExposure{1.0f};
    float initExposure{1.0f};
    // Light intensities stored as scale of kKeyIScale (Adam conditioning).
    static constexpr float kKeyIScale = 40.0f;
    bool fitPedestal{false};
    bool fitKeyLight{false};
    bool fitFillLight{false};
    bool fitRimLight{false};
    bool fitEnvScale{false};
    bool fitExposure{false};

    /// Primary block: 5 (uniform) or 14 (4×RGB + shared rough/metal).
    [[nodiscard]] int tileCount() const noexcept {
        return mapGround ? (mapRes * mapRes) : 0;
    }
    [[nodiscard]] size_t primaryDims() const noexcept {
        if (!mapGround) return 5u;
        return static_cast<size_t>(tileCount()) * 3u + 2u; // RGB×N² + rough + metal
    }

    // Layout: primary | pedestal[3]? | key? fill? rim? | env? | exposure?
    [[nodiscard]] size_t thetaDims() const {
        size_t n = primaryDims();
        if (fitPedestal && pedestalMat) n += 3;
        if (fitKeyLight && keyLight) n += 1;
        if (fitFillLight && fillLight) n += 1;
        if (fitRimLight && rimLight) n += 1;
        if (fitEnvScale) n += 1;
        if (fitExposure) n += 1;
        return n;
    }

    [[nodiscard]] bool needsLightUpdate() const {
        return (fitKeyLight && keyLight) || (fitFillLight && fillLight) ||
               (fitRimLight && rimLight) || fitEnvScale;
    }

    /// Index of first light/env/exposure param (for regularizer).
    [[nodiscard]] size_t lightBlockStart() const {
        size_t i = primaryDims();
        if (fitPedestal && pedestalMat) i += 3;
        return i;
    }

    void appendLightTheta(std::vector<double>& t, float key, float fill, float rim,
                          float env, float exposure) const {
        if (fitKeyLight && keyLight) t.push_back(key / kKeyIScale);
        if (fitFillLight && fillLight) t.push_back(fill / kKeyIScale);
        if (fitRimLight && rimLight) t.push_back(rim / kKeyIScale);
        if (fitEnvScale) t.push_back(env);
        if (fitExposure) t.push_back(exposure);
    }

    void appendPrimaryTruth(std::vector<double>& t) const {
        if (mapGround) {
            for (int k = 0; k < tileCount(); ++k) {
                t.push_back(truthTiles[static_cast<size_t>(k)].r);
                t.push_back(truthTiles[static_cast<size_t>(k)].g);
                t.push_back(truthTiles[static_cast<size_t>(k)].b);
            }
            t.push_back(truthPrimary.roughness);
            t.push_back(truthPrimary.metallic);
        } else {
            auto p = truthPrimary.asTheta();
            t.insert(t.end(), p.begin(), p.end());
        }
    }

    void appendPrimaryInit(std::vector<double>& t) const {
        if (mapGround) {
            for (int k = 0; k < tileCount(); ++k) {
                t.push_back(initTiles[static_cast<size_t>(k)].r);
                t.push_back(initTiles[static_cast<size_t>(k)].g);
                t.push_back(initTiles[static_cast<size_t>(k)].b);
            }
            t.push_back(initPrimary.roughness);
            t.push_back(initPrimary.metallic);
        } else {
            auto p = initPrimary.asTheta();
            t.insert(t.end(), p.begin(), p.end());
        }
    }

    [[nodiscard]] std::vector<double> truthTheta() const {
        std::vector<double> t;
        t.reserve(thetaDims());
        appendPrimaryTruth(t);
        if (fitPedestal && pedestalMat) {
            t.push_back(truthPedestal.albedo.r);
            t.push_back(truthPedestal.albedo.g);
            t.push_back(truthPedestal.albedo.b);
        }
        appendLightTheta(t, truthKeyI, truthFillI, truthRimI, truthEnvScale, truthExposure);
        return t;
    }

    [[nodiscard]] std::vector<double> initTheta() const {
        std::vector<double> t;
        t.reserve(thetaDims());
        appendPrimaryInit(t);
        if (fitPedestal && pedestalMat) {
            t.push_back(initPedestal.albedo.r);
            t.push_back(initPedestal.albedo.g);
            t.push_back(initPedestal.albedo.b);
        }
        appendLightTheta(t, initKeyI, initFillI, initRimI, initEnvScale, initExposure);
        return t;
    }

    /// Random θ in box bounds (for dataset export / multi-start / ML).
    [[nodiscard]] std::vector<double> sampleRandomTheta(uint32_t rng) const {
        auto u01 = [&]() -> double {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<double>(rng >> 8) / static_cast<double>(1u << 24);
        };
        std::vector<double> t;
        t.reserve(thetaDims());
        if (mapGround) {
            for (int k = 0; k < tileCount() * 3; ++k) t.push_back(u01());
            t.push_back(0.04 + 0.96 * u01());
            t.push_back(u01());
        } else {
            for (int i = 0; i < 3; ++i) t.push_back(u01());
            t.push_back(0.04 + 0.96 * u01());
            t.push_back(u01());
        }
        if (fitPedestal && pedestalMat) {
            t.push_back(u01());
            t.push_back(u01());
            t.push_back(u01());
        }
        // Tighter light ranges match ParamSpace bounds (reduce multi-light blowout).
        if (fitKeyLight && keyLight) t.push_back(0.15 + 0.75 * u01());
        if (fitFillLight && fillLight) t.push_back(0.08 + 0.45 * u01());
        if (fitRimLight && rimLight) t.push_back(0.08 + 0.45 * u01());
        if (fitEnvScale) t.push_back(0.35 + 1.0 * u01());
        if (fitExposure) t.push_back(0.4 + 1.6 * u01());
        return t;
    }

    void applyPrimaryFromTheta(const std::vector<double>& th) {
        const size_t pd = primaryDims();
        if (th.size() < pd) return;
        if (mapGround && !groundMats.empty() &&
            groundMats.size() == static_cast<size_t>(tileCount())) {
            const size_t nT = static_cast<size_t>(tileCount());
            const float rough = static_cast<float>(th[nT * 3]);
            const float metal = static_cast<float>(th[nT * 3 + 1]);
            for (size_t k = 0; k < nT; ++k) {
                const size_t o = k * 3;
                writeMatPbr(groundMats[k], groundTiles[k],
                            PbrParams{.albedo = {static_cast<float>(th[o]), static_cast<float>(th[o + 1]),
                                                 static_cast<float>(th[o + 2])},
                                      .roughness = rough,
                                      .metallic = metal});
            }
            if (primaryMat) {
                writeMatPbr(primaryMat, primaryActor,
                            PbrParams{.albedo = {static_cast<float>(th[0]), static_cast<float>(th[1]),
                                                 static_cast<float>(th[2])},
                                      .roughness = rough,
                                      .metallic = metal});
            }
        } else {
            writeMatPbr(primaryMat, primaryActor,
                        PbrParams{.albedo = {static_cast<float>(th[0]), static_cast<float>(th[1]),
                                             static_cast<float>(th[2])},
                                  .roughness = static_cast<float>(th[3]),
                                  .metallic = static_cast<float>(th[4])});
        }
    }

    void applyTheta(const std::vector<double>& th) {
        if (th.size() < primaryDims()) return;
        applyPrimaryFromTheta(th);
        size_t i = primaryDims();
        if (fitPedestal && pedestalMat && th.size() >= i + 3) {
            PbrParams pp = truthPedestal;
            pp.albedo = {static_cast<float>(th[i]), static_cast<float>(th[i + 1]),
                         static_cast<float>(th[i + 2])};
            writeMatPbr(pedestalMat, pedestalActor, pp);
            i += 3;
        }
        if (fitKeyLight && keyLight && th.size() > i) {
            keyLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitFillLight && fillLight && th.size() > i) {
            fillLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitRimLight && rimLight && th.size() > i) {
            rimLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitEnvScale && th.size() > i) {
            currentEnvScale = static_cast<float>(th[i]);
            ++i;
        }
        if (fitExposure && th.size() > i) {
            currentExposure = static_cast<float>(th[i]);
        }
    }

    void applyTruth() { applyTheta(truthTheta()); }

    void applyCamera(Camera& cam, int viewIndex) const {
        const CameraView& v = views[static_cast<size_t>(viewIndex) % views.size()];
        cam.setPosition(v.position);
        cam.setRotation(v.pitchDeg, v.yawDeg);
        cam.setFov(40.0f);
    }

    static InverseScene buildCornell(const FitConfig& cfg) {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Cornell");
        s.fitPedestal = false;
        s.fitKeyLight = cfg.fitKeyLight;
        s.fitFillLight = false;
        s.fitRimLight = false;
        s.fitEnvScale = false;
        s.fitExposure = false;
        s.mapGround = false;
        s.truthPrimary = {.albedo = {0.65f, 0.05f, 0.05f}, .roughness = 0.35f, .metallic = 0.0f};
        s.initPrimary = {.albedo = {0.2f, 0.5f, 0.7f}, .roughness = 0.85f, .metallic = 0.55f};
        s.truthKeyI = 40.0f;
        s.initKeyI = 12.0f;
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red = s.truthPrimary.albedo, green(0.12f, 0.45f, 0.15f);
        glm::vec3 LBB(-S, -S, -S), RBB(S, -S, -S), LTB(-S, S, -S), RTB(S, S, -S);
        glm::vec3 LBF(-S, -S, S), RBF(S, -S, S), LTF(-S, S, S), RTF(S, S, S);
        addWall(s.scene.get(), "Back", LBB, RBB, RTB, LTB, {0, 0, 1}, white);
        addWall(s.scene.get(), "Left", LBB, LTB, LTF, LBF, {1, 0, 0}, red, s.truthPrimary.roughness,
                s.truthPrimary.metallic);
        addWall(s.scene.get(), "Right", RBB, RBF, RTF, RTB, {-1, 0, 0}, green);
        addWall(s.scene.get(), "Floor", LBB, LBF, RBF, RBB, {0, 1, 0}, white, 0.7f);
        addWall(s.scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0, -1, 0}, white);

        auto sphere = s.scene->createActorWithComponents("Sphere", PrimitiveType::Sphere);
        sphere->getTransform()->setPosition({0.0f, -S + 1.5f, 0.0f});
        sphere->getTransform()->setScale(glm::vec3(1.5f));
        auto sm = sphere->getComponent<MaterialComponent>();
        sm->getMaterial().baseColor = {0.8f, 0.8f, 0.8f};
        sm->getMaterial().roughness = 0.4f;
        sm->getMaterial().metallic = 0.0f;

        auto light = s.scene->createActor("KeyLight");
        auto lc = light->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor({1.0f, 0.98f, 0.95f});
        lc->setIntensity(s.truthKeyI);
        lc->setRadius(0.6f);
        light->getTransform()->setPosition({0.0f, S - 0.5f, 0.0f});
        s.keyLight = lc.get();

        s.scene->forEachActor([&](Actor& a) {
            if (a.getName() == "Left") {
                s.primaryActor = &a;
                s.primaryMat = a.getComponent<MaterialComponent>().get();
            }
        });
        s.views = {
            {"front", {0.0f, 0.0f, 13.0f}, 0.0f, -90.0f},
            {"leftish", {-4.0f, 0.5f, 12.0f}, -2.0f, -110.0f},
            {"rightish", {4.0f, 0.5f, 12.0f}, -2.0f, -70.0f},
        };
        return s;
    }

    static InverseScene buildStudio(const FitConfig& cfg) {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Product Studio");
        s.envPath = cfg.envPath;
        s.relightEnvPath = cfg.relightEnvPath;
        s.fitPedestal = cfg.fitPedestal;
        s.fitKeyLight = cfg.fitKeyLight;
        s.fitFillLight = cfg.fitFillLight && cfg.fitKeyLight;
        s.fitRimLight = cfg.fitRimLight && cfg.fitKeyLight;
        s.fitEnvScale = cfg.fitEnvScale;
        s.fitExposure = cfg.fitExposure && !cfg.targetImage.empty();
        s.mapGround = cfg.mapGround || cfg.mapRes >= 2;
        s.mapRes = s.mapGround ? std::max(2, cfg.mapRes) : 1;
        // Floor PBR from preset / CLI defaults (tricky presets override metal/rough).
        s.truthPrimary = {.albedo = {cfg.truthR, cfg.truthG, cfg.truthB},
                          .roughness = cfg.truthRough,
                          .metallic = cfg.truthMetal};
        s.initPrimary = {.albedo = {cfg.initR, cfg.initG, cfg.initB},
                         .roughness = cfg.initRough,
                         .metallic = cfg.initMetal};
        // Spatial tile truth/init so map recovery is non-trivial.
        {
            const int nT = s.mapGround ? (s.mapRes * s.mapRes) : 0;
            s.truthTiles.resize(static_cast<size_t>(nT));
            s.initTiles.resize(static_cast<size_t>(nT));
            for (int k = 0; k < nT; ++k) {
                const float u = static_cast<float>(k % s.mapRes) / static_cast<float>(std::max(1, s.mapRes - 1));
                const float v = static_cast<float>(k / s.mapRes) / static_cast<float>(std::max(1, s.mapRes - 1));
                // Checker + hue drift for spatial structure
                const float checker = ((k % s.mapRes) + (k / s.mapRes)) % 2 == 0 ? 1.0f : 0.72f;
                s.truthTiles[static_cast<size_t>(k)] = {
                    std::clamp(cfg.truthR * checker * (0.85f + 0.25f * u), 0.05f, 1.0f),
                    std::clamp(cfg.truthG * checker * (0.85f + 0.25f * v), 0.05f, 1.0f),
                    std::clamp(cfg.truthB * checker * (0.90f + 0.15f * (1.0f - u)), 0.05f, 1.0f)};
                // Init: wrong uniform-ish with per-tile shuffle
                s.initTiles[static_cast<size_t>(k)] = {
                    std::clamp(cfg.initR * (0.7f + 0.3f * v), 0.05f, 1.0f),
                    std::clamp(cfg.initG * (0.7f + 0.3f * u), 0.05f, 1.0f),
                    std::clamp(cfg.initB * (0.6f + 0.4f * (1.0f - v)), 0.05f, 1.0f)};
            }
        }
        s.truthPedestal = {.albedo = {0.18f, 0.19f, 0.21f}, .roughness = 0.40f, .metallic = 0.05f};
        s.initPedestal = {.albedo = {0.55f, 0.12f, 0.10f}, .roughness = 0.40f, .metallic = 0.05f};
        s.truthKeyI = 22.0f;
        s.initKeyI = 7.0f;
        s.truthFillI = 9.0f;
        s.initFillI = 3.0f;
        s.truthRimI = 10.0f;
        s.initRimI = 4.0f;
        s.truthEnvScale = 1.0f;
        s.initEnvScale = 0.45f;
        s.currentEnvScale = 1.0f;
        s.truthExposure = 1.0f;
        s.initExposure = cfg.exposure > 0.0f ? cfg.exposure : 1.0f;
        s.currentExposure = s.initExposure;

        const float groundY = 0.0f;
        const float pedestalH = 0.35f;
        const float pedestalHalf = 1.35f;

        // ── Optimizable ground (uniform or 2×2 albedo tiles) ──────────
        {
            const float half = 14.0f;
            // Atlas UVs so a dense ground albedo map is Deferred-sampled (beauty SoT).
            auto makeGroundTile = [&](const char* name, float x0, float z0, float x1, float z1,
                                      const glm::vec3& col, glm::vec2 uv0, glm::vec2 uv1,
                                      glm::vec2 uv2, glm::vec2 uv3) {
                auto ground = s.scene->createActor(name);
                auto gm = std::make_shared<Model>();
                addQuad(gm->vertices, gm->indices,
                        {x0, groundY, z0}, {x1, groundY, z0},
                        {x1, groundY, z1}, {x0, groundY, z1},
                        {0, 1, 0}, col, uv0, uv1, uv2, uv3);
                auto gmesh = ground->addComponent<MeshComponent>();
                gmesh->setModel(gm);
                gmesh->setVisible(true);
                auto gmat = ground->addComponent<MaterialComponent>();
                gmat->getMaterial().baseColor = col;
                gmat->getMaterial().roughness = s.truthPrimary.roughness;
                gmat->getMaterial().metallic = s.truthPrimary.metallic;
                return std::pair{ground.get(), gmat.get()};
            };

            if (s.mapGround) {
                const int N = s.mapRes;
                const float cell = (2.0f * half) / static_cast<float>(N);
                const float invN = 1.0f / static_cast<float>(N);
                for (int iz = 0; iz < N; ++iz) {
                    for (int ix = 0; ix < N; ++ix) {
                        const int idx = iz * N + ix;
                        const float x0 = -half + static_cast<float>(ix) * cell;
                        const float z0 = -half + static_cast<float>(iz) * cell;
                        const float x1 = x0 + cell;
                        const float z1 = z0 + cell;
                        const float u0 = static_cast<float>(ix) * invN;
                        const float v0 = static_cast<float>(iz) * invN;
                        const float u1 = u0 + invN;
                        const float v1 = v0 + invN;
                        char name[32];
                        std::snprintf(name, sizeof(name), "Ground_%d_%d", ix, iz);
                        auto [actor, mat] = makeGroundTile(
                            name, x0, z0, x1, z1, s.truthTiles[static_cast<size_t>(idx)],
                            {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1});
                        s.groundTiles.push_back(actor);
                        s.groundMats.push_back(mat);
                    }
                }
                s.primaryActor = s.groundTiles[0];
                s.primaryMat = s.groundMats[0];
            } else {
                auto [actor, mat] = makeGroundTile(
                    "Ground", -half, -half, half, half, s.truthPrimary.albedo,
                    {0, 0}, {1, 0}, {1, 1}, {0, 1});
                s.primaryActor = actor;
                s.primaryMat = mat;
            }
        }

        // ── Soft vertical backdrop (product-shot cyclorama) ────────────
        {
            const float halfW = 14.0f;
            const float h = 8.0f;
            const float z = -6.5f;
            const glm::vec3 backdropCol(0.82f, 0.84f, 0.88f);
            auto wall = s.scene->createActor("Backdrop");
            auto wm = std::make_shared<Model>();
            addQuad(wm->vertices, wm->indices,
                    {-halfW, groundY, z}, {halfW, groundY, z},
                    {halfW, groundY + h, z}, {-halfW, groundY + h, z},
                    {0, 0, 1}, backdropCol);
            auto mesh = wall->addComponent<MeshComponent>();
            mesh->setModel(wm);
            mesh->setVisible(true);
            auto mat = wall->addComponent<MaterialComponent>();
            mat->getMaterial().baseColor = backdropCol;
            mat->getMaterial().roughness = 0.85f;
            mat->getMaterial().metallic = 0.0f;
        }

        // ── Pedestal (top albedo is multi-surface θ; sides fixed slate) ─
        {
            const float y0 = groundY + 0.002f;
            const float y1 = groundY + pedestalH;
            const float h = pedestalHalf;
            const glm::vec3 slate = s.truthPedestal.albedo;
            auto makeFace = [&](const char* name, glm::vec3 a, glm::vec3 b, glm::vec3 c,
                                glm::vec3 d, glm::vec3 n, bool isTop) {
                auto actor = s.scene->createActor(name);
                auto m = std::make_shared<Model>();
                addQuad(m->vertices, m->indices, a, b, c, d, n, slate);
                auto mesh = actor->addComponent<MeshComponent>();
                mesh->setModel(m);
                mesh->setVisible(true);
                auto mat = actor->addComponent<MaterialComponent>();
                mat->getMaterial().baseColor = slate;
                mat->getMaterial().roughness = s.truthPedestal.roughness;
                mat->getMaterial().metallic = s.truthPedestal.metallic;
                if (isTop) {
                    s.pedestalActor = actor.get();
                    s.pedestalMat = mat.get();
                }
            };
            makeFace("PedestalTop", {-h, y1, -h}, {h, y1, -h}, {h, y1, h}, {-h, y1, h},
                     {0, 1, 0}, true);
            makeFace("PedestalFront", {-h, y0, h}, {h, y0, h}, {h, y1, h}, {-h, y1, h},
                     {0, 0, 1}, false);
            makeFace("PedestalBack", {h, y0, -h}, {-h, y0, -h}, {-h, y1, -h}, {h, y1, -h},
                     {0, 0, -1}, false);
            makeFace("PedestalLeft", {-h, y0, -h}, {-h, y0, h}, {-h, y1, h}, {-h, y1, -h},
                     {-1, 0, 0}, false);
            makeFace("PedestalRight", {h, y0, h}, {h, y0, -h}, {h, y1, -h}, {h, y1, h},
                     {1, 0, 0}, false);
        }

        // ── Hero model on pedestal ─────────────────────────────────────
        auto model = ModelLoader::load(cfg.modelPath);
        if (model && !model->vertices.empty()) {
            glm::vec3 bmin(1e30f), bmax(-1e30f);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            const glm::vec3 extent = bmax - bmin;
            const glm::vec3 centerXZ{(bmin.x + bmax.x) * 0.5f, 0.0f, (bmin.z + bmax.z) * 0.5f};
            for (auto& v : model->vertices) {
                v.position.x -= centerXZ.x;
                v.position.y -= bmin.y;
                v.position.z -= centerXZ.z;
            }
            const float maxExtent = std::max({extent.x, extent.y, extent.z});
            const float scale =
                (2.0f * cfg.heroScaleMul) / std::max(extent.y > 0.01f ? extent.y : maxExtent, 0.001f);

            auto actor = s.scene->createActor("Hero");
            actor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, 22.0f, 0.0f))));
            actor->getTransform()->setScale(glm::vec3(scale));
            actor->getTransform()->setPosition({0.0f, groundY + pedestalH + 0.002f, 0.0f});
            s.heroActor = actor.get();

            auto mesh = actor->addComponent<MeshComponent>();
            mesh->setModel(model);
            mesh->setVisible(true);
            auto mat = actor->addComponent<MaterialComponent>();
            mat->getMaterial().roughness = 0.35f;
            std::cout << "  loaded hero: " << cfg.modelPath << " (" << model->vertices.size()
                      << " verts, scale=" << scale << ", h=" << (extent.y * scale) << ")\n";
        } else {
            std::cerr << "  WARN: could not load " << cfg.modelPath
                      << " — product studio without hero\n";
        }

        {
            auto a = s.scene->createActor("Key");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({1.0f, 0.97f, 0.92f});
            lc->setIntensity(s.truthKeyI);
            lc->setRadius(1.0f);
            a->getTransform()->setPosition(s.baseKeyPos);
            s.keyLight = lc.get();
            s.keyLightActor = a.get();
        }
        {
            auto a = s.scene->createActor("Fill");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({0.75f, 0.85f, 1.0f});
            lc->setIntensity(s.truthFillI);
            lc->setRadius(1.4f);
            a->getTransform()->setPosition(s.baseFillPos);
            s.fillLight = lc.get();
            s.fillLightActor = a.get();
        }
        {
            auto a = s.scene->createActor("Rim");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({1.0f, 0.95f, 0.9f});
            lc->setIntensity(s.truthRimI);
            lc->setRadius(0.7f);
            a->getTransform()->setPosition(s.baseRimPos);
            s.rimLight = lc.get();
            s.rimLightActor = a.get();
        }

        const float d = cfg.camDistMul;
        s.views = {
            {"front", {0.0f, 1.35f * d, 7.2f * d}, -8.0f, -90.0f},
            {"three_quarter", {5.0f * d, 1.55f * d, 5.6f * d}, -10.0f, -48.0f},
            {"opposite", {-4.8f * d, 1.35f * d, 5.4f * d}, -8.0f, -128.0f},
        };
        s.baseViews = s.views;
        return s;
    }

    /// L2 domain randomization for export (θ dims unchanged).
    int applyDomainRandomization(uint32_t& rng) {
        auto u01 = [&]() -> float {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / static_cast<float>(1u << 24);
        };
        auto n11 = [&]() -> float { return u01() * 2.0f - 1.0f; };

        auto jitterPos = [&](Actor* a, const glm::vec3& base, float amp) {
            if (!a) return;
            a->getTransform()->setPosition(
                base + glm::vec3(n11() * amp, n11() * amp * 0.45f, n11() * amp));
        };
        jitterPos(keyLightActor, baseKeyPos, 1.25f);
        jitterPos(fillLightActor, baseFillPos, 1.05f);
        jitterPos(rimLightActor, baseRimPos, 1.05f);

        if (heroActor) {
            const float yaw = 8.0f + u01() * 55.0f;
            heroActor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, yaw, 0.0f))));
        }

        if (baseViews.empty()) baseViews = views;
        const float distScale = 1.0f + n11() * 0.14f;
        const float yaw = -90.0f + n11() * 58.0f;
        const float pitch = -9.0f + n11() * 7.0f;
        const float elev = 1.25f + n11() * 0.4f;
        const float dist = 6.9f * distScale;
        const float yawRad = glm::radians(yaw + 90.0f);
        CameraView orbit{"orbit",
                         {std::cos(yawRad) * dist, elev, std::sin(yawRad) * dist},
                         pitch,
                         yaw};
        views.clear();
        views.push_back(orbit);
        for (const auto& b : baseViews) views.push_back(b);
        return 0;
    }

    void resetDomainDefaults() {
        if (keyLightActor) keyLightActor->getTransform()->setPosition(baseKeyPos);
        if (fillLightActor) fillLightActor->getTransform()->setPosition(baseFillPos);
        if (rimLightActor) rimLightActor->getTransform()->setPosition(baseRimPos);
        if (heroActor) {
            heroActor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, 22.0f, 0.0f))));
        }
        if (!baseViews.empty()) views = baseViews;
    }
};


} // namespace ohao::inverse
