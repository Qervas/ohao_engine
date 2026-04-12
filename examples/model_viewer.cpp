// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "animation/animation_component.hpp"
#include "animation/animation_clip.hpp"
#include "render/camera/camera.hpp"
#include "render/rt/oidn_denoise.hpp"

#include <iostream>
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
        std::cout << "Usage: model_viewer <model.glb|.obj> [output.png] [samples]" << std::endl;
        return 1;
    }

    std::string modelPath = argv[1];
    std::string output = argc > 2 ? argv[2] : "model_output.png";
    int samples = argc > 3 ? std::atoi(argv[3]) : 1024;
    uint32_t W = 3840, H = 2160;

    std::cout << "OHAO Model Viewer — " << modelPath << std::endl;
    std::cout << W << "x" << H << " @ " << samples << " spp" << std::endl;

    bool useDeferred = (argc > 4 && std::string(argv[4]) == "deferred");

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "FATAL: renderer init failed" << std::endl; return 1; }

    // Cornell box backdrop
    auto scene = std::make_unique<Scene>("Model Viewer");
    const float S = 5.0f;
    const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
    glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
    glm::vec3 LBF(-S,-S,S),  RBF(S,-S,S),  LTF(-S,S,S),  RTF(S,S,S);
    addWall(scene.get(), "Back",    LBB, RBB, RTB, LTB, {0,0,1},  white);
    addWall(scene.get(), "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
    addWall(scene.get(), "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
    addWall(scene.get(), "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
    addWall(scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);

    // Load model
    auto model = std::make_shared<Model>();
    bool loaded = false;
    auto dot = modelPath.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
    // Use Assimp loader for formats with animation (FBX, GLTF with skeleton)
    // Fall back to native GLTF/OBJ loaders for static models
    if (ext == "obj") {
        loaded = model->loadFromOBJ(modelPath);
    } else if (ext == "fbx" || ext == "FBX" || ext == "dae" || ext == "DAE") {
        loaded = model->loadFromFBX(modelPath);
    } else {
        // Try GLTF first, then fall back to Assimp if it has a skeleton
        loaded = model->loadFromGLTF(modelPath);
        if (loaded && model->hasSkeleton()) {
            // Re-load through Assimp for correct animation node tree
            auto assimpModel = std::make_shared<Model>();
            if (assimpModel->loadFromFBX(modelPath)) {
                model = assimpModel;
            }
        }
    }

    if (loaded) {
        // Auto-frame: compute bounds, scale to fit box
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        {
            glm::vec3 ext = bmax - bmin;
            std::cout << "Model bounds: (" << bmin.x << "," << bmin.y << "," << bmin.z
                      << ") - (" << bmax.x << "," << bmax.y << "," << bmax.z
                      << ") extent=(" << ext.x << "," << ext.y << "," << ext.z << ")" << std::endl;
        }
        glm::vec3 extent = bmax - bmin;
        bool isYUp = (extent.y >= extent.z);
        float modelHeight = isYUp ? extent.y : extent.z;
        float scale = (S * 1.6f) / modelHeight;

        // Compute shared transform
        glm::quat rotation = isYUp
            ? glm::quat(glm::radians(glm::vec3(0, 180, 0)))
            : glm::quat(glm::radians(glm::vec3(((bmin.z + bmax.z) * 0.5f < 0.0f) ? 90.0f : -90.0f, 0, 0)));
        glm::vec3 center = (bmin + bmax) * 0.5f;
        float feetOffset = isYUp ? -bmin.y * scale : 0.0f;
        glm::vec3 position = {-center.x * scale, -S + feetOffset, isYUp ? -center.z * scale : 0.0f};

        // Split model into per-material sub-models for correct multi-material rendering
        bool hasMultipleMaterials = model->materialPerTriangle.size() > 0 &&
            *std::max_element(model->materialPerTriangle.begin(), model->materialPerTriangle.end()) > 0;

        // Create shared animation component (if skeleton exists)
        std::shared_ptr<AnimationComponent> sharedAnimComp;

        if (hasMultipleMaterials) {
            // Group triangles by material
            uint32_t maxMat = *std::max_element(model->materialPerTriangle.begin(), model->materialPerTriangle.end());
            for (uint32_t matIdx = 0; matIdx <= maxMat; matIdx++) {
                auto subModel = std::make_shared<Model>();
                subModel->skeleton = model->skeleton;

                // Collect triangles for this material
                for (size_t tri = 0; tri < model->materialPerTriangle.size(); tri++) {
                    if (model->materialPerTriangle[tri] != matIdx) continue;
                    for (int k = 0; k < 3; k++) {
                        uint32_t oldIdx = model->indices[tri * 3 + k];
                        uint32_t newIdx = static_cast<uint32_t>(subModel->vertices.size());
                        subModel->vertices.push_back(model->vertices[oldIdx]);
                        subModel->indices.push_back(newIdx);
                    }
                    subModel->materialPerTriangle.push_back(matIdx);
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

                // Attach animation to each sub-actor
                if (model->hasSkeleton()) {
                    auto animComp = actor->addComponent<AnimationComponent>();
                    animComp->setSkeleton(model->skeleton);
                    if (model->skeleton->ufbxScene) {
                        animComp->initialize();
                        animComp->play("ufbx");
                    }
                }
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

            if (model->hasSkeleton()) {
                auto animComp = actor->addComponent<AnimationComponent>();
                animComp->setSkeleton(model->skeleton);
                if (model->skeleton->ufbxScene) {
                    animComp->initialize();
                    animComp->play("ufbx");
                    std::cout << "Animation: ufbx (" << model->skeleton->joints.size()
                              << " joints, " << model->skeleton->ufbxAnimDuration << "s)" << std::endl;
                } else if (!model->animations.empty()) {
                    for (const auto& clip : model->animations)
                        animComp->addAnimation(clip->name, clip);
                    animComp->initialize();
                    animComp->play(model->animations[0]->name);
                }
            }
        }

        std::cout << "Loaded: " << model->vertices.size() << " verts, scale=" << scale << std::endl;
    } else {
        std::cerr << "Failed to load: " << modelPath << std::endl;
    }

    // Environment map
    renderer.setEnvironmentMap("assets/test_models/env_studio.hdr");

    // Key light
    auto keyLight = scene->createActor("KeyLight");
    auto kl = keyLight->addComponent<LightComponent>();
    kl->setLightType(LightType::Sphere);
    kl->setColor({1.0f, 0.95f, 0.9f});
    kl->setIntensity(30.0f);
    kl->setRadius(1.0f);
    keyLight->getTransform()->setPosition({3.0f, 3.0f, 3.0f});

    // Fill light
    auto fillLight = scene->createActor("FillLight");
    auto fl = fillLight->addComponent<LightComponent>();
    fl->setLightType(LightType::Sphere);
    fl->setColor({0.6f, 0.7f, 1.0f});
    fl->setIntensity(10.0f);
    fl->setRadius(0.5f);
    fillLight->getTransform()->setPosition({-3.0f, 1.0f, 2.0f});

    renderer.setScene(scene.get());
    renderer.updateSceneBuffers();

    auto& camera = renderer.getCamera();
    camera.setPosition({0, 0, 13});
    camera.setFov(35.0f);
    camera.setRotation(0.0f, -90.0f);
    // Mode: "deferred" for hybrid RT, anything else for path traced
    renderer.setRenderMode(useDeferred ? RenderMode::Deferred : RenderMode::PathTraced);

    // Enable all deferred quality features
    if (useDeferred && renderer.getDeferredRenderer()) {
        auto* pp = renderer.getDeferredRenderer()->getPostProcessing();
        if (pp) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.2f);   // slightly brighter than default
        }
    }

    std::cout << "Rendering (" << (useDeferred ? "Deferred+RT" : "PathTraced") << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    int frames = useDeferred ? 30 : (samples + 3);

    // If 5th arg is "video", render a frame sequence for ffmpeg
    bool renderVideo = (argc > 5 && std::string(argv[5]) == "video");
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

    // OIDN denoising — only for path traced mode (deferred doesn't use accumulation buffer)
    std::vector<float> beautyRGBA, albedoRGBA, normalRGBA;
    uint32_t rw, rh;
    if (!useDeferred && renderer.readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {
        // Convert RGBA32F → float3 (OIDN needs float3)
        auto beauty3 = ohao::rgba32fToFloat3(beautyRGBA.data(), rw, rh);
        auto albedo3 = ohao::rgba32fToFloat3(albedoRGBA.data(), rw, rh);
        auto normal3 = ohao::rgba32fToFloat3(normalRGBA.data(), rw, rh);

        // Denoise
        ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(), rw, rh, true);

        // Tonemap + save
        auto rgba8 = ohao::float3ToRGBA8(beauty3.data(), rw, rh, 0.5f);
        stbi_write_png(output.c_str(), rw, rh, 4, rgba8.data(), rw * 4);
        std::cout << "Saved (OIDN denoised): " << output << std::endl;
    } else {
        // Fallback: save without denoising
        const uint8_t* pixels = renderer.getPixels();
        if (pixels) {
            stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
            std::cout << "Saved: " << output << std::endl;
        }
    }

    scene.reset();
}
