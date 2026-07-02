// Environment Map Demo — model in HDR environment, no Cornell box
// Usage: ./env_demo <model.glb> <env.hdr> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <chrono>
#include <cstring>
#include <algorithm>
#include "render/rt/denoise/denoise_types.hpp"

using namespace ohao;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: env_demo <model.glb> <env.hdr> [output.png] [samples]" << std::endl;
        return 1;
    }

    std::string modelPath = argv[1];
    std::string envPath = argv[2];
    std::string output = argc > 3 ? argv[3] : "env_demo.png";
    int samples = argc > 4 ? std::atoi(argv[4]) : 1024;
    uint32_t W = 1920, H = 1080;
    RenderMode rtMode = RenderMode::RTOffline;
    std::optional<ohao::DenoiseMode> denoiseOverride;
    // Sub-plan 4.H: scene composition flags.
    // --ground=on  → add procedural grey 50m floor under loaded model so it
    //                no longer floats in void (contact shadow + visible floor)
    // --lighting=studio  → 3-point key/fill/rim area lights (default)
    // --lighting=hdr-only → preserve original env-only behavior (single warm key)
    // --lighting=none    → no explicit lights, env IBL only (pure HDR studio)
    bool groundEnabled = true;
    enum class LightingMode { Studio, HdrOnly, None, Cinema };
    LightingMode lightingMode = LightingMode::Studio;
    // Sub-plan 4.K: global anisotropic specular override. 0 = isotropic;
    // 0.5–0.85 gives visible brushed-metal/chrome streaks. Rotation in
    // radians orients the tangent frame around N. Both 0 → no change.
    float anisoStrength = 0.0f;
    float anisoRotation = 0.0f;
    // Sub-plan 4.L: SSS strength for skin. 0.5-0.7 = realistic skin.
    float sssStrength = 0.0f;
    // --crop=face: tight portrait framing on subject head for SSS close-up.
    bool faceCrop = false;
    // --flip: rotate model 180° about Y for assets whose front faces away from
    // the camera (e.g. DamagedHelmet). glTF has no facing convention, so this is
    // per-asset. Matches model_viewer's "flip" arg.
    bool flipModel = false;
    std::string dumpMvPath;
    std::string dumpDepthPath;
    std::string dumpRoughnessPath;
    std::string dumpDiffusePath;
    std::string dumpSpecularPath;
    std::string dumpNrdDiffusePath;
    std::string dumpNrdSpecularPath;
    std::string dumpNrdComposedPath;
    std::string dumpDiffAlbedoPath;
    std::string dumpSpecColorPath;
    std::string dumpHitDistDiffusePath;
    std::string dumpHitDistSpecularPath;
    float panX = 0.0f;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "rt_realtime") rtMode = RenderMode::RTRealtime;
        else if (arg == "rt_offline") rtMode = RenderMode::RTOffline;
        else if (arg.rfind("--denoise=", 0) == 0) {
            denoiseOverride = ohao::parseDenoiseMode(arg.substr(10));
        } else if (arg.rfind("--dump-mv=", 0) == 0) {
            dumpMvPath = arg.substr(10);
        } else if (arg.rfind("--dump-depth=", 0) == 0) {
            dumpDepthPath = arg.substr(13);
        } else if (arg.rfind("--dump-roughness=", 0) == 0) {
            dumpRoughnessPath = arg.substr(17);
        } else if (arg.rfind("--dump-diffuse=", 0) == 0) {
            dumpDiffusePath = arg.substr(15);
        } else if (arg.rfind("--dump-specular=", 0) == 0) {
            dumpSpecularPath = arg.substr(16);
        } else if (arg.rfind("--dump-nrd-diffuse=", 0) == 0) {
            dumpNrdDiffusePath = arg.substr(19);
        } else if (arg.rfind("--dump-nrd-specular=", 0) == 0) {
            dumpNrdSpecularPath = arg.substr(20);
        } else if (arg.rfind("--dump-nrd-composed=", 0) == 0) {
            dumpNrdComposedPath = arg.substr(20);
        } else if (arg.rfind("--dump-diff-albedo=", 0) == 0) {
            dumpDiffAlbedoPath = arg.substr(19);
        } else if (arg.rfind("--dump-spec-color=", 0) == 0) {
            dumpSpecColorPath = arg.substr(18);
        } else if (arg.rfind("--dump-hit-dist-diffuse=", 0) == 0) {
            dumpHitDistDiffusePath = arg.substr(24);
        } else if (arg.rfind("--dump-hit-dist-specular=", 0) == 0) {
            dumpHitDistSpecularPath = arg.substr(25);
        } else if (arg.rfind("--pan-x=", 0) == 0) {
            panX = std::stof(arg.substr(8));
        } else if (arg.rfind("--ground=", 0) == 0) {
            std::string v = arg.substr(9);
            groundEnabled = (v == "on" || v == "true" || v == "1");
        } else if (arg.rfind("--lighting=", 0) == 0) {
            std::string v = arg.substr(11);
            if (v == "studio")       lightingMode = LightingMode::Studio;
            else if (v == "hdr-only" || v == "hdr") lightingMode = LightingMode::HdrOnly;
            else if (v == "none")    lightingMode = LightingMode::None;
            else if (v == "cinema")  lightingMode = LightingMode::Cinema;
        } else if (arg.rfind("--aniso=", 0) == 0) {
            anisoStrength = std::clamp(std::stof(arg.substr(8)), 0.0f, 0.95f);
        } else if (arg.rfind("--aniso-rot=", 0) == 0) {
            anisoRotation = std::stof(arg.substr(12));
        } else if (arg.rfind("--sss=", 0) == 0) {
            sssStrength = std::clamp(std::stof(arg.substr(6)), 0.0f, 1.0f);
        } else if (arg == "--crop=face") {
            faceCrop = true;
        } else if (arg == "--flip") {
            flipModel = true;
        }
    }

    // IEEE 754 half → float (shared by MV + RGBA16F radiance dumps)
    auto half2float = [](uint16_t h) -> float {
        uint32_t sign = (h >> 15) & 0x1;
        uint32_t exp  = (h >> 10) & 0x1f;
        uint32_t mant = h & 0x3ff;
        uint32_t f;
        if (exp == 0) {
            if (mant == 0) {
                f = sign << 31;
            } else {
                exp = 1;
                while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
                mant &= 0x3ff;
                f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
            }
        } else if (exp == 0x1f) {
            f = (sign << 31) | (0xff << 23) | (mant << 13);
        } else {
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
        float out;
        std::memcpy(&out, &f, 4);
        return out;
    };

    // Decode RGBA32F → Reinhard-tonemapped 8-bit RGB PNG (reports max channel to stdout).
    auto dumpRGBA32FStream = [&](const std::string& path, const std::vector<float>& data,
                                  uint32_t w, uint32_t h) {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        float maxC = 0.0f;
        for (uint32_t i = 0; i < w * h; i++) {
            float r = data[i * 4 + 0];
            float g = data[i * 4 + 1];
            float b = data[i * 4 + 2];
            maxC = std::max({maxC, r, g, b});
            float rT = r / (r + 1.0f);
            float gT = g / (g + 1.0f);
            float bT = b / (b + 1.0f);
            rgb[i * 3 + 0] = static_cast<uint8_t>(std::max(0, std::min(255, int(rT * 255.0f))));
            rgb[i * 3 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, int(gT * 255.0f))));
            rgb[i * 3 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, int(bT * 255.0f))));
        }
        stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
        std::cout << "Saved " << path << " (max channel = " << maxC << ")" << std::endl;
    };

    // RGBA8 → RGB PNG passthrough. No tonemap/decode — data is already in [0, 255] per channel.
    auto dumpRGBA8ToRGB = [&](const std::string& path, const std::vector<uint8_t>& rgba,
                               uint32_t w, uint32_t h) {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        for (uint32_t i = 0; i < w * h; i++) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
        stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
        std::cout << "Saved " << path << std::endl;
    };

    auto dumpHitDistance = [&](const std::string& path, const std::vector<float>& data,
                               uint32_t w, uint32_t h) {
        // Alpha channel of RGBA32F carries raw world-space hit-distance.
        // Normalize to max finite for 8-bit grayscale preview.
        float maxFinite = 0.0f;
        for (uint32_t i = 0; i < w * h; i++) {
            float d = data[i * 4 + 3];  // alpha channel
            if (d > maxFinite && d < 1e20f) maxFinite = d;
        }
        if (maxFinite <= 0.0f) maxFinite = 1.0f;

        std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 0);
        for (uint32_t i = 0; i < w * h; i++) {
            float d = data[i * 4 + 3];
            float norm = std::max(0.0f, std::min(1.0f, d / maxFinite));
            gray[i] = static_cast<uint8_t>(norm * 255.0f);
        }
        stbi_write_png(path.c_str(), w, h, 1, gray.data(), w);
        std::cout << "Saved " << path << " (max hit-dist = " << maxFinite << " world units)" << std::endl;
    };

    std::cout << "OHAO Env Demo — " << modelPath << " + " << envPath << std::endl;

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "FATAL" << std::endl; return 1; }

    // Set HDR environment — but skip when --lighting=studio because env
    // contribution overwhelms the 3-point studio lights and tints surfaces
    // with the env's color (the pink-ground problem in 4.H v1-v6). True
    // product photography is shot in a closed studio: only explicit lights,
    // black background. That's what studio mode now does.
    if (lightingMode != LightingMode::Studio && lightingMode != LightingMode::Cinema) {
        renderer.setEnvironmentMap(envPath);
    } else {
        std::cout << "[4.I/L] " << (lightingMode == LightingMode::Cinema ? "Cinema" : "Studio")
                  << " mode: skipping HDR env load (explicit lighting only)" << std::endl;
    }

    // Scene with just the model — no walls
    auto scene = std::make_unique<Scene>("Env Demo");

    // Load model
    auto model = std::make_shared<Model>();
    bool loaded = false;
    auto dot = modelPath.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
    if (ext == "obj") loaded = model->loadFromOBJ(modelPath);
    else if (ext == "fbx" || ext == "FBX" || ext == "dae" || ext == "blend") loaded = model->loadFromFBX(modelPath);
    else loaded = model->loadFromGLTF(modelPath);

    // Sub-plan 4.H: track normalized-space model bounds so we can drop the
    // ground plane right under the loaded model (bottom Y) and size studio
    // lights relative to model height. Default to 4-unit-tall (model normalize
    // scale below) centered at origin, bottom at y=-2.
    float groundY = -2.0f;
    float modelHeightWorld = 4.0f;

    if (loaded) {
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        glm::vec3 extent = bmax - bmin;
        glm::vec3 center = (bmin + bmax) * 0.5f;
        // Orientation — mirror SceneFramer's proven up-axis logic (model_viewer uses it).
        // env_demo previously reinvented this with the same heuristic PLUS a hardcoded 180°
        // Y-flip, which rendered Y-up models (MetalRoughSpheres) mirrored/backwards. The
        // 180° flip was the bug — SceneFramer applies NO rotation for Y-up models.
        bool isYUp = (extent.y >= extent.z);
        // Scale by the LARGEST extent so wide/flat models don't overflow the frame
        // (height-only scaling blew up the chess board and the sphere grid).
        float maxExtent = std::max({extent.x, extent.y, extent.z});
        float scale = 4.0f / std::max(maxExtent, 0.001f);

        auto actor = scene->createActor("Model");
        if (isYUp) {
            // Y-up faces +Z toward the camera natively; --flip for back-facing assets.
            actor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0, flipModel ? 180.0f : 0.0f, 0))));
        } else {
            // Z-up: rotate onto Y. +Z-up → -90° X; -Z-up → +90° X.
            bool posZIsUp = (bmax.z > 0.01f);
            float rotX = posZIsUp ? -90.0f : 90.0f;
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(rotX, 0, 0))));
        }
        actor->getTransform()->setScale(glm::vec3(scale));
        actor->getTransform()->setPosition({-center.x * scale, -center.y * scale, -center.z * scale});

        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().roughness = 0.5f;

        // After centering at origin: model height = ~4 world units, bottom
        // at y = -2 (Y-up) or after Z-up rotation at the equivalent Y. Use
        // the normalized half-extent (2.0) as the ground offset.
        modelHeightWorld = 4.0f;
        groundY = -modelHeightWorld * 0.5f - 0.001f;  // tiny epsilon below feet

        std::cout << "Loaded: " << model->vertices.size() << " verts" << std::endl;
    }

    // Sub-plan 4.H Phase 1: procedural ground plane.
    // Two triangles, ~50m × 50m, grey PBR (albedo 0.5, roughness 0.6, metallic
    // 0). Placed at groundY just under the loaded model. Picked up by the
    // standard scene-upload + BLAS-build path via the Actor/MeshComponent/
    // MaterialComponent triplet — no engine-side API change needed.
    if (groundEnabled) {
        const float half = 25.0f;  // 50m × 50m total
        auto groundActor = scene->createActor("Ground");
        auto gm = std::make_shared<Model>();
        auto mkv = [&](glm::vec3 pos) {
            Vertex v{};
            v.position = pos;
            v.normal   = {0.0f, 1.0f, 0.0f};
            v.color    = {0.5f, 0.5f, 0.5f};
            v.texCoord = {0.0f, 0.0f};
            v.tangent  = {1.0f, 0.0f, 0.0f, 1.0f};
            v.boneIndices = glm::ivec4(0);
            v.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            return v;
        };
        gm->vertices = {
            mkv({-half, groundY, -half}),
            mkv({ half, groundY, -half}),
            mkv({ half, groundY,  half}),
            mkv({-half, groundY,  half}),
        };
        gm->indices = {0, 1, 2, 0, 2, 3};
        auto gmesh = groundActor->addComponent<MeshComponent>();
        gmesh->setModel(gm);
        gmesh->setVisible(true);
        auto gmat = groundActor->addComponent<MaterialComponent>();
        gmat->getMaterial().baseColor = {0.5f, 0.5f, 0.5f};
        gmat->getMaterial().roughness = 0.6f;
        gmat->getMaterial().metallic  = 0.0f;
        std::cout << "[4.H] Ground plane: " << (half * 2.0f) << "m at y=" << groundY << std::endl;
    }

    // Sub-plan 4.H Phase 2: lighting.
    if (lightingMode == LightingMode::Studio) {
        // 3-point lighting around model normalized to ~4 units tall, centered
        // at origin. Implemented as soft sphere lights (well-tested shader
        // path) rather than rect-area lights, which currently have a
        // sphere-area / rect-area unit mismatch in pt_raygen. Distances ~2× model
        // half-height. Color temps follow product photography: warm key, cool
        // fill, neutral rim.
        auto addSphere = [&](const std::string& name, glm::vec3 pos,
                             glm::vec3 color, float intensity, float radius) {
            auto a = scene->createActor(name);
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor(color);
            lc->setIntensity(intensity);
            lc->setRadius(radius);
            a->getTransform()->setPosition(pos);
        };

        // v8: D65 neutral white lights (5600K, daylight studio). Warm key was
        // pink-tinting the grey ground. Real product photography uses
        // color-corrected daylight strobes; everything reads neutral. Apply
        // subtle warm/cool only as soft accent.
        // Key light: camera-right, ~30° elevation, near-neutral daylight, strong
        addSphere("KeyLight",  { 4.0f, 4.0f,  4.0f}, {1.0f, 0.98f, 0.96f}, 400.0f, 1.5f);
        // Fill light: camera-left, lower, slight cool tint (~6500K)
        addSphere("FillLight", {-4.0f, 2.5f,  2.0f}, {0.95f, 0.98f, 1.0f}, 150.0f, 2.0f);
        // Rim light: behind subject + above, pure neutral, edge-defining
        addSphere("RimLight",  { 0.0f, 4.5f, -4.0f}, {1.0f, 1.0f, 1.0f}, 300.0f, 1.0f);
        // Bottom-front bounce fill — opens up under-shadow regions
        addSphere("BounceLight", { 0.0f, 0.5f,  4.5f}, {0.98f, 1.0f, 1.0f}, 100.0f, 2.5f);

        std::cout << "[4.H] Studio 3-point lighting injected (3 sphere lights)" << std::endl;
    } else if (lightingMode == LightingMode::Cinema) {
        // 4.L: cinematic Rembrandt setup — single strong side key 45° camera-right,
        // tight dim fill, soft rim. Creates the harsh terminator that SSS softens.
        // Subject in deep shadow on the camera-left side; key lights the right cheek.
        // This is what makes skin look like skin instead of marble.
        auto addSphere = [&](const std::string& name, glm::vec3 pos,
                             glm::vec3 color, float intensity, float radius) {
            auto a = scene->createActor(name);
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor(color);
            lc->setIntensity(intensity);
            lc->setRadius(radius);
            a->getTransform()->setPosition(pos);
        };
        // Key: 45° camera-right, slight warm (tungsten-ish, 4500K). Dimmer
        // than 4-point studio so close-up subjects don't clip and texture
        // detail survives. SSS does the visual work, not brightness.
        addSphere("KeyLight",  { 5.0f, 3.5f,  2.5f}, {1.0f, 0.95f, 0.88f}, 80.0f, 1.0f);
        // Fill: 1/8 the key intensity, slight cool (8:1 contrast ratio = cinema standard)
        addSphere("FillLight", {-3.0f, 1.5f,  2.0f}, {0.85f, 0.92f, 1.0f}, 10.0f, 2.5f);
        // Rim: behind + above, cooler, sharper highlight
        addSphere("RimLight",  {-1.5f, 4.0f, -3.5f}, {0.9f, 0.95f, 1.0f}, 30.0f, 0.6f);
        std::cout << "[4.L] Cinema Rembrandt lighting injected (strong side key, 8:1 ratio)" << std::endl;
    } else if (lightingMode == LightingMode::HdrOnly) {
        // Preserve pre-4.H behavior: single warm key sphere light + HDR env
        auto keyLight = scene->createActor("Key");
        auto kl = keyLight->addComponent<LightComponent>();
        kl->setLightType(LightType::Sphere);
        kl->setColor({1.0f, 0.95f, 0.9f});
        kl->setIntensity(8.0f);
        kl->setRadius(1.0f);
        keyLight->getTransform()->setPosition({4.0f, 3.0f, 4.0f});
    }
    // LightingMode::None → no lights, HDR env IBL only (pure environment)

    renderer.setScene(scene.get());

    auto& camera = renderer.getCamera();
    if (lightingMode == LightingMode::Cinema) {
        if (faceCrop) {
            // 85mm portrait: head fills frame. Model is normalized to 4-units
            // tall, centered at origin → head spans y=[1, 2], face center
            // y=1.5. Camera level with face, no tilt; tight FOV puts the head
            // + shoulders in frame.
            camera.setPosition({0.0f, 1.5f, 3.0f});
            camera.setFov(22.0f);
            camera.setRotation(0.0f, -90.0f);
        } else {
            // 50mm full-figure framing.
            camera.setPosition({0.0f, 2.0f, 7.0f});
            camera.setFov(40.0f);
            camera.setRotation(-3.0f, -90.0f);
        }
    } else {
        camera.setPosition({0, 0.5f, 8});
        camera.setFov(40.0f);
        camera.setRotation(0.0f, -90.0f);
    }
    renderer.setRenderMode(rtMode);

    if (denoiseOverride.has_value()) {
        renderer.setDenoiseMode(*denoiseOverride);
        std::cout << "Denoise mode (CLI override): "
                  << ohao::denoiseModeName(*denoiseOverride) << std::endl;
    } else {
        std::cout << "Denoise mode (preset): "
                  << ohao::denoiseModeName(renderer.getDenoiseMode()) << std::endl;
    }

    if (anisoStrength > 0.0f || sssStrength > 0.0f) {
        auto settings = renderer.getRTRenderSettings();
        settings.anisotropyStrength = anisoStrength;
        settings.anisotropyRotation = anisoRotation;
        settings.subsurfaceStrength = sssStrength;
        renderer.setRTRenderSettings(settings);
        if (anisoStrength > 0.0f)
            std::cout << "Anisotropic specular: strength=" << anisoStrength
                      << " rotation=" << anisoRotation << " rad" << std::endl;
        if (sssStrength > 0.0f)
            std::cout << "Subsurface scattering: strength=" << sssStrength << std::endl;
    }

    std::cout << "Rendering (" << (rtMode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline") << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    // If --pan-x is set, render samples+2 frames, then translate camera +X
    // and render one final frame — that last frame's MV reflects the pan.
    int preCount = samples + 3;
    if (panX != 0.0f) preCount = samples + 2;
    for (int i = 0; i < preCount; i++) renderer.render();
    if (panX != 0.0f) {
        auto pos = camera.getPosition();
        camera.setPosition({pos.x + panX, pos.y, pos.z});
        renderer.render();
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms" << std::endl;

    // getPixels() handles OIDN transparently if denoiseMode != None
    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == ohao::DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << std::endl;
    }

    if (!dumpMvPath.empty()) {
        std::vector<uint16_t> mvRaw;
        uint32_t mw = 0, mh = 0;
        if (!renderer.readbackMotionVector(mvRaw, mw, mh)) {
            std::cerr << "[MV dump] readback failed or no RT profile active\n";
        } else {
            // Encode: +X motion → red, +Y motion → green, neutral = 128
            std::vector<uint8_t> rgb(static_cast<size_t>(mw) * mh * 3, 128);
            const float scale = 0.02f;  // ~50px motion saturates to full red/green
            for (uint32_t i = 0; i < mw * mh; i++) {
                float dx = half2float(mvRaw[i * 2 + 0]);
                float dy = half2float(mvRaw[i * 2 + 1]);
                float r01 = std::max(-1.0f, std::min(1.0f, dx * scale));
                float g01 = std::max(-1.0f, std::min(1.0f, dy * scale));
                int r = static_cast<int>(128.0f + r01 * 127.0f);
                int g = static_cast<int>(128.0f + g01 * 127.0f);
                rgb[i * 3 + 0] = static_cast<uint8_t>(r);
                rgb[i * 3 + 1] = static_cast<uint8_t>(g);
                rgb[i * 3 + 2] = 128;
            }
            stbi_write_png(dumpMvPath.c_str(), mw, mh, 3, rgb.data(), mw * 3);
            std::cout << "Saved MV debug: " << dumpMvPath << std::endl;
        }
    }

    if (!dumpDepthPath.empty()) {
        std::vector<float> depthData;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDepthAOV(depthData, dw, dh)) {
            std::cerr << "[Depth dump] readback failed\n";
        } else {
            float maxFinite = 0.0f;
            for (float d : depthData) if (d < 1e20f && d > maxFinite) maxFinite = d;
            if (maxFinite <= 0.0f) maxFinite = 1.0f;

            std::vector<uint8_t> gray(static_cast<size_t>(dw) * dh, 255);
            for (uint32_t i = 0; i < dw * dh; i++) {
                float d = depthData[i];
                if (d < 1e20f) {
                    int v = static_cast<int>((d / maxFinite) * 255.0f);
                    gray[i] = static_cast<uint8_t>(std::max(0, std::min(255, v)));
                }
            }
            stbi_write_png(dumpDepthPath.c_str(), dw, dh, 1, gray.data(), dw);
            std::cout << "Saved depth debug: " << dumpDepthPath
                      << " (max finite = " << maxFinite << ")" << std::endl;
        }
    }

    if (!dumpRoughnessPath.empty()) {
        std::vector<float> roughData;
        uint32_t rw = 0, rh = 0;
        if (!renderer.readbackRoughnessAOV(roughData, rw, rh)) {
            std::cerr << "[Roughness dump] readback failed\n";
        } else {
            // R16F → 8-bit grayscale PNG. Roughness is [0, 1], clamp + scale.
            std::vector<uint8_t> gray(static_cast<size_t>(rw) * rh, 0);
            for (uint32_t i = 0; i < rw * rh; i++) {
                float r = std::max(0.0f, std::min(1.0f, roughData[i]));
                gray[i] = static_cast<uint8_t>(r * 255.0f);
            }
            stbi_write_png(dumpRoughnessPath.c_str(), rw, rh, 1, gray.data(), rw);
            std::cout << "Saved roughness debug: " << dumpRoughnessPath << std::endl;
        }
    }

    if (!dumpDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDiffuseRadiance(data, dw, dh)) {
            std::cerr << "[Diffuse dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpDiffusePath, data, dw, dh);
        }
    }

    if (!dumpSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackSpecularRadiance(data, sw, sh)) {
            std::cerr << "[Specular dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpSpecularPath, data, sw, sh);
        }
    }

    if (!dumpNrdDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDenoisedDiffuse(data, dw, dh)) {
            std::cerr << "[NRD diffuse dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdDiffusePath, data, dw, dh);
        }
    }

    if (!dumpNrdSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackDenoisedSpecular(data, sw, sh)) {
            std::cerr << "[NRD specular dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdSpecularPath, data, sw, sh);
        }
    }

    if (!dumpNrdComposedPath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackNrdComposed(data, w, h)) {
            std::cerr << "[NRD composed dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdComposedPath, data, w, h);
        }
    }

    if (!dumpHitDistDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackDiffuseRadiance(data, w, h)) {
            std::cerr << "[Hit-dist diffuse dump] readback failed\n";
        } else {
            dumpHitDistance(dumpHitDistDiffusePath, data, w, h);
        }
    }

    if (!dumpHitDistSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackSpecularRadiance(data, w, h)) {
            std::cerr << "[Hit-dist specular dump] readback failed\n";
        } else {
            dumpHitDistance(dumpHitDistSpecularPath, data, w, h);
        }
    }

    if (!dumpDiffAlbedoPath.empty()) {
        std::vector<uint8_t> rgba;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDiffAlbedoAOV(rgba, dw, dh)) {
            std::cerr << "[Diff albedo dump] readback failed\n";
        } else {
            dumpRGBA8ToRGB(dumpDiffAlbedoPath, rgba, dw, dh);
        }
    }

    if (!dumpSpecColorPath.empty()) {
        std::vector<uint8_t> rgba;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackSpecColorAOV(rgba, sw, sh)) {
            std::cerr << "[Spec color dump] readback failed\n";
        } else {
            dumpRGBA8ToRGB(dumpSpecColorPath, rgba, sw, sh);
        }
    }
}
