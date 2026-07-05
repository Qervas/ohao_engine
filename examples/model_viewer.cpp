// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "scene/asset/model_loader.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <chrono>
#include <filesystem>

using namespace ohao;

void addWall(Scene* scene, const std::string& name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    uint32_t base = 0;
    auto mkv = [&](glm::vec3 pos) {
        Vertex v{}; v.position = pos; v.normal = normal; v.color = color;
        v.texCoord = {0,0}; v.tangent = {1,0,0,1};
        v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
        return v;
    };
    model->vertices = {mkv(a), mkv(b), mkv(c), mkv(d)};
    model->indices = {0,1,2, 0,2,3};
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model); mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = 0.95f;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: model_viewer <model.glb|.obj> [output.png] [samples] [deferred|rt_realtime|rt_offline] [flip] [video]" << std::endl;
        return 1;
    }

    std::string modelPath = argv[1];
    std::string output = argc > 2 ? argv[2] : "model_output.png";
    int samples = argc > 3 ? std::atoi(argv[3]) : 1024;
    uint32_t W = 3840, H = 2160;

    std::cout << "OHAO Model Viewer — " << modelPath << std::endl;
    std::cout << W << "x" << H << " @ " << samples << " spp" << std::endl;

    bool useDeferred = false;
    RenderMode rtMode = RenderMode::RTOffline;
    std::optional<ohao::DenoiseMode> denoiseOverride;
    for (int i = 4; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "deferred") useDeferred = true;
        else if (arg == "rt_realtime") rtMode = RenderMode::RTRealtime;
        else if (arg == "rt_offline") rtMode = RenderMode::RTOffline;
        else if (arg.rfind("--denoise=", 0) == 0) {
            denoiseOverride = ohao::parseDenoiseMode(arg.substr(10));
        }
    }

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "FATAL: renderer init failed" << std::endl; return 1; }

    // Load model first — room size adapts to model
    auto model = ModelLoader::load(modelPath);
    bool loaded = (model != nullptr);

    // Compute framing (room size, camera, lights) from model bounds
    FrameResult frame;
    if (loaded) {
        frame = SceneFramer::computeFraming(model->vertices);
    } else {
        frame.roomHalfSize = 15.0f;
        frame.modelScale = 1.0f;
        frame.modelPosition = glm::vec3(0);
        frame.modelRotation = glm::quat(1, 0, 0, 0);
        frame.cameraPosition = {0, 0, 25};
        frame.cameraFov = 45.0f;
    }
    const float S = frame.roomHalfSize;

    auto scene = std::make_unique<Scene>("Model Viewer");

    // Helper: create a solid quad (wall/floor/furniture face)
    auto addQuad = [&](const std::string& name, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                       glm::vec3 normal, glm::vec3 color, float roughness) {
        auto actor = scene->createActor(name);
        auto m = std::make_shared<Model>();
        auto mkv = [&](glm::vec3 pos) {
            Vertex v{}; v.position = pos; v.normal = normal; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        m->vertices = {mkv(a), mkv(b), mkv(c), mkv(d)};
        m->indices = {0,1,2, 0,2,3};
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(m); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = color;
        mat->getMaterial().roughness = roughness;
    };

    // Helper: create a box (6 faces) for furniture
    auto addBox = [&](const std::string& name, glm::vec3 lo, glm::vec3 hi,
                      glm::vec3 color, float roughness, float metallic = 0.0f) {
        auto actor = scene->createActor(name);
        auto m = std::make_shared<Model>();
        auto mkv = [&](glm::vec3 pos, glm::vec3 n) {
            Vertex v{}; v.position = pos; v.normal = n; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        glm::vec3 c[8] = {
            {lo.x,lo.y,lo.z}, {hi.x,lo.y,lo.z}, {hi.x,hi.y,lo.z}, {lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z}, {hi.x,lo.y,hi.z}, {hi.x,hi.y,hi.z}, {lo.x,hi.y,hi.z}
        };
        // 6 faces: front, back, left, right, top, bottom
        int faces[6][4] = {{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{3,7,6,2},{0,1,5,4}};
        glm::vec3 normals[6] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; f++) {
            uint32_t base = (uint32_t)m->vertices.size();
            for (int k = 0; k < 4; k++) m->vertices.push_back(mkv(c[faces[f][k]], normals[f]));
            m->indices.insert(m->indices.end(), {base,base+1,base+2, base,base+2,base+3});
        }
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(m); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = color;
        mat->getMaterial().roughness = roughness;
        mat->getMaterial().metallic = metallic;
    };

    // === BEDROOM SCENE ===
    glm::vec3 wallColor(0.55f, 0.50f, 0.45f);   // muted warm gray walls
    glm::vec3 floorColor(0.18f, 0.10f, 0.05f);  // dark walnut wood
    glm::vec3 ceilColor(0.60f, 0.58f, 0.55f);   // gray ceiling

    // Walls — extend 0.1 past corners to prevent ray leaks at seams
    float E = 0.1f;
    addQuad("Back",    {-S-E,-S-E,-S},{S+E,-S-E,-S},{S+E,S+E,-S},{-S-E,S+E,-S},  {0,0,1},  wallColor, 0.9f);
    // No front wall — camera renders from outside looking in
    addQuad("Left",    {-S,-S-E,-S-E},{-S,-S-E,S+E},{-S,S+E,S+E},{-S,S+E,-S-E},  {1,0,0},  wallColor, 0.9f);
    addQuad("Right",   {S,-S-E,S+E},{S,-S-E,-S-E},{S,S+E,-S-E},{S,S+E,S+E},      {-1,0,0}, wallColor, 0.9f);
    addQuad("Floor",   {-S-E,-S,-S-E},{S+E,-S,-S-E},{S+E,-S,S+E},{-S-E,-S,S+E},  {0,1,0},  floorColor, 0.95f);
    addQuad("Ceiling", {-S-E,S,-S-E},{S+E,S,-S-E},{S+E,S,S+E},{-S-E,S,S+E},      {0,-1,0}, ceilColor, 0.9f);

    // Bed — behind the model, against back wall
    float bedW = S * 0.8f, bedH = S * 0.25f, bedD = S * 0.6f;
    glm::vec3 bedColor(0.85f, 0.82f, 0.78f);     // linen white sheets
    glm::vec3 frameColor(0.20f, 0.12f, 0.06f);    // dark wood frame
    addBox("BedFrame", {-bedW, -S, -S}, {bedW, -S + bedH * 0.4f, -S + bedD}, frameColor, 0.5f);
    addBox("Mattress", {-bedW + 0.2f, -S + bedH * 0.4f, -S + 0.2f},
                       {bedW - 0.2f, -S + bedH, -S + bedD - 0.2f}, bedColor, 0.95f);
    // Headboard
    addBox("Headboard", {-bedW, -S, -S}, {bedW, -S + bedH * 2.5f, -S + 0.3f}, frameColor, 0.4f);
    // Pillow
    addBox("Pillow", {-bedW * 0.4f, -S + bedH, -S + 0.4f},
                     {bedW * 0.4f, -S + bedH + 1.0f, -S + bedD * 0.4f}, {0.92f, 0.90f, 0.88f}, 0.95f);

    // Nightstand — right side
    float nsX = S * 0.55f;
    glm::vec3 nsColor(0.22f, 0.14f, 0.07f);
    addBox("Nightstand", {nsX, -S, -S}, {nsX + 3.0f, -S + bedH * 1.5f, -S + 3.0f}, nsColor, 0.45f);

    if (loaded) {
        float scale = frame.modelScale;
        glm::quat rotation = frame.modelRotation;
        glm::vec3 position = frame.modelPosition;

        // Split model into per-material sub-models for correct multi-material rendering
        bool hasMultipleMaterials = model->materialPerTriangle.size() > 0 &&
            *std::max_element(model->materialPerTriangle.begin(), model->materialPerTriangle.end()) > 0;

        if (hasMultipleMaterials) {
            // Group triangles by material
            uint32_t maxMat = *std::max_element(model->materialPerTriangle.begin(), model->materialPerTriangle.end());
            for (uint32_t matIdx = 0; matIdx <= maxMat; matIdx++) {
                auto subModel = std::make_shared<Model>();

                // Collect triangles for this material
                for (size_t tri = 0; tri < model->materialPerTriangle.size(); tri++) {
                    if (model->materialPerTriangle[tri] != matIdx) continue;
                    for (int k = 0; k < 3; k++) {
                        uint32_t oldIdx = model->indices[tri * 3 + k];
                        uint32_t newIdx = static_cast<uint32_t>(subModel->vertices.size());
                        subModel->vertices.push_back(model->vertices[oldIdx]);
                        subModel->indices.push_back(newIdx);
                    }
                    subModel->materialPerTriangle.push_back(0);  // local: sub-model has 1 material at index 0
                }
                if (subModel->vertices.empty()) continue;

                // Copy material data for this material index
                if (matIdx < model->materialColors.size())
                    subModel->materialColors.push_back(model->materialColors[matIdx]);
                if (matIdx < model->materialMetallic.size())
                    subModel->materialMetallic.push_back(model->materialMetallic[matIdx]);

                // Copy only THIS material's textures
                int albedoIdx = (matIdx < model->materialTextureIndex.size()) ? model->materialTextureIndex[matIdx] : -1;
                int normalIdx = (matIdx < model->materialNormalTexIndex.size()) ? model->materialNormalTexIndex[matIdx] : -1;
                int roughMetalIdx = (matIdx < model->materialRoughMetalTexIndex.size()) ? model->materialRoughMetalTexIndex[matIdx] : -1;
                int emissiveIdx = (matIdx < model->materialEmissiveTexIndex.size()) ? model->materialEmissiveTexIndex[matIdx] : -1;
                if (albedoIdx >= 0 && albedoIdx < static_cast<int>(model->albedoTextures.size())) {
                    subModel->albedoTextures.push_back(model->albedoTextures[albedoIdx]);
                    subModel->materialTextureIndex.push_back(0);
                } else {
                    subModel->materialTextureIndex.push_back(-1);
                }
                if (normalIdx >= 0 && normalIdx < static_cast<int>(model->normalTextures.size())) {
                    subModel->normalTextures.push_back(model->normalTextures[normalIdx]);
                    subModel->materialNormalTexIndex.push_back(0);
                } else {
                    subModel->materialNormalTexIndex.push_back(-1);
                }
                if (roughMetalIdx >= 0 && roughMetalIdx < static_cast<int>(model->roughMetalTextures.size())) {
                    subModel->roughMetalTextures.push_back(model->roughMetalTextures[roughMetalIdx]);
                    subModel->materialRoughMetalTexIndex.push_back(0);
                } else {
                    subModel->materialRoughMetalTexIndex.push_back(-1);
                }
                if (emissiveIdx >= 0 && emissiveIdx < static_cast<int>(model->emissiveTextures.size())) {
                    subModel->emissiveTextures.push_back(model->emissiveTextures[emissiveIdx]);
                    subModel->materialEmissiveTexIndex.push_back(0);
                } else {
                    subModel->materialEmissiveTexIndex.push_back(-1);
                }

                auto actor = scene->createActor("Mesh_" + std::to_string(matIdx));
                actor->getTransform()->setRotation(rotation);
                actor->getTransform()->setScale(glm::vec3(scale));
                actor->getTransform()->setPosition(position);

                auto mesh = actor->addComponent<MeshComponent>();
                mesh->setModel(subModel); mesh->setVisible(true);

                auto mat = actor->addComponent<MaterialComponent>();
                glm::vec3 baseColor = matIdx < model->materialColors.size()
                    ? glm::vec3(model->materialColors[matIdx]) : glm::vec3(0.8f);
                float roughness = matIdx < model->materialColors.size()
                    ? model->materialColors[matIdx].w : 0.5f;
                float metallic = matIdx < model->materialMetallic.size()
                    ? model->materialMetallic[matIdx] : 0.0f;
                mat->getMaterial().baseColor = baseColor;
                mat->getMaterial().roughness = roughness;
                mat->getMaterial().metallic = metallic;
            }
            std::cout << "Split into " << (maxMat + 1) << " material groups" << std::endl;
        } else {
            // Single-material model
            auto actor = scene->createActor("Model");
            actor->getTransform()->setRotation(rotation);
            actor->getTransform()->setScale(glm::vec3(scale));
            actor->getTransform()->setPosition(position);

            auto mesh = actor->addComponent<MeshComponent>();
            mesh->setModel(model); mesh->setVisible(true);
            auto mat = actor->addComponent<MaterialComponent>();
            mat->getMaterial().baseColor = {0.8f, 0.7f, 0.6f};
            mat->getMaterial().roughness = 0.5f;
        }

        std::cout << "Loaded: " << model->vertices.size() << " verts, scale=" << scale << std::endl;
    } else {
        std::cerr << "Failed to load: " << modelPath << std::endl;
    }

    // Environment map
    renderer.setEnvironmentMap("assets/test_models/env_studio.hdr");

    // Auto-framed lights (scaled to room size)
    SceneFramer::applyLights(scene.get(), frame);

    renderer.setScene(scene.get());
    // Note: setScene() calls updateSceneBuffers() internally — no explicit call needed

    // Auto-framed camera
    auto& camera = renderer.getCamera();
    SceneFramer::applyCamera(camera, frame);

    // Check any arg for "flip" — rotates model 180° Y (for models facing away)
    for (int i = 4; i < argc; i++) {
        if (std::string(argv[i]) == "flip") {
            glm::quat flipRot = glm::quat(glm::radians(glm::vec3(0, 180, 0)));
            for (const auto& [id, actor] : scene->getAllActors()) {
                if (actor->getName().find("Mesh_") == 0 || actor->getName() == "Model")
                    actor->getTransform()->setRotation(flipRot);
            }
            break;
        }
    }
    // Mode: "deferred" for hybrid RT, anything else for path traced
    renderer.setRenderMode(useDeferred ? RenderMode::Deferred : rtMode);

    if (denoiseOverride.has_value()) {
        renderer.setDenoiseMode(*denoiseOverride);
        std::cout << "Denoise mode (CLI override): "
                  << ohao::denoiseModeName(*denoiseOverride) << std::endl;
    } else {
        std::cout << "Denoise mode (preset): "
                  << ohao::denoiseModeName(renderer.getDenoiseMode()) << std::endl;
    }

    // Enable all deferred quality features
    if (useDeferred && renderer.getDeferredRenderer()) {
        auto* pp = renderer.getDeferredRenderer()->getPostProcessing();
        if (pp) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }

    const char* rtLabel = (rtMode == RenderMode::RTRealtime) ? "RTRealtime" : "RTOffline";
    std::cout << "Rendering (" << (useDeferred ? "Deferred+RT" : rtLabel) << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    int frames = useDeferred ? 30 : (samples + 3);

    // If 5th arg is "video", render a frame sequence for ffmpeg
    bool renderVideo = false;
    for (int i = 4; i < argc; i++) {
        if (std::string(argv[i]) == "video") {
            renderVideo = true;
            break;
        }
    }
    if (renderVideo && useDeferred) {
        int fps = 30;
        int seconds = 3;
        int totalFrames = fps * seconds;
        std::string videoDir = output.substr(0, output.find_last_of('.'));
        std::filesystem::create_directories(videoDir);

        // Warm up the pipeline (triple-buffer flush)
        for (int i = 0; i < 5; i++) renderer.render();

        std::cout << "Recording " << totalFrames << " frames to " << videoDir << "/" << std::endl;
        for (int i = 0; i < totalFrames; i++) {
            renderer.render();
            const uint8_t* px = renderer.getPixels();
            if (px) {
                char fname[256];
                snprintf(fname, sizeof(fname), "%s/frame_%04d.png", videoDir.c_str(), i);
                stbi_write_png(fname, W, H, 4, px, W * 4);
            }
            if (i % 30 == 0) std::cout << "  frame " << i << "/" << totalFrames << std::endl;
        }

        // Encode with ffmpeg
        std::string mp4 = videoDir + ".mp4";
        std::string cmd = "ffmpeg -y -framerate " + std::to_string(fps)
            + " -i " + videoDir + "/frame_%04d.png"
            + " -c:v libx264 -pix_fmt yuv420p -crf 18 " + mp4 + " 2>/dev/null";
        std::cout << "Encoding: " << mp4 << std::endl;
        system(cmd.c_str());
        std::cout << "Video saved: " << mp4 << std::endl;
        scene.reset();
        return 0;
    }

    for (int i = 0; i < frames; i++) renderer.render();
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

    scene.reset();
}
