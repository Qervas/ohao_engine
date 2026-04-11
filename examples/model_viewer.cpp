// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples]

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
#include "render/rt/oidn_denoise.hpp"

#include <iostream>
#include <string>
#include <chrono>

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
    if (ext == "obj") loaded = model->loadFromOBJ(modelPath);
    else loaded = model->loadFromGLTF(modelPath);

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

        auto actor = scene->createActor("Model");
        if (isYUp) {
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0, 180, 0))));
        } else {
            // Z-up: check if Z is positive-up or negative-up
            float zCenter = (bmin.z + bmax.z) * 0.5f;
            float rotX = (zCenter < 0.0f) ? 90.0f : -90.0f;  // flip if Z goes negative
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(rotX, 0, 0))));
        }
        actor->getTransform()->setScale(glm::vec3(scale));

        glm::vec3 center = (bmin + bmax) * 0.5f;
        float feetOffset = isYUp ? -bmin.y * scale : 0.0f;
        actor->getTransform()->setPosition({
            -center.x * scale, -S + feetOffset,
            isYUp ? -center.z * scale : 0.0f
        });

        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = {0.8f, 0.7f, 0.6f};
        mat->getMaterial().roughness = 0.7f;

        std::cout << "Loaded: " << model->vertices.size() << " verts, scale=" << scale << std::endl;
    } else {
        std::cerr << "Failed to load: " << modelPath << std::endl;
    }

    // Environment map (HDR)
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
    bool useDeferred = (argc > 4 && std::string(argv[4]) == "deferred");
    renderer.setRenderMode(useDeferred ? RenderMode::Deferred : RenderMode::PathTraced);

    std::cout << "Rendering (" << (useDeferred ? "Deferred+RT" : "PathTraced") << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    int frames = useDeferred ? 10 : (samples + 3);
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
