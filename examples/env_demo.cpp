// Environment Map Demo — model in HDR environment, no Cornell box
// Usage: ./env_demo <model.glb> <env.hdr> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/offscreen_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"

#include <iostream>
#include <string>
#include <chrono>

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

    std::cout << "OHAO Env Demo — " << modelPath << " + " << envPath << std::endl;

    OffscreenRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "FATAL" << std::endl; return 1; }

    // Set HDR environment
    renderer.setEnvironmentMap(envPath);

    // Scene with just the model — no walls
    auto scene = std::make_unique<Scene>("Env Demo");

    // Load model
    auto model = std::make_shared<Model>();
    bool loaded = false;
    auto dot = modelPath.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
    if (ext == "obj") loaded = model->loadFromOBJ(modelPath);
    else loaded = model->loadFromGLTF(modelPath);

    if (loaded) {
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        glm::vec3 extent = bmax - bmin;
        bool isYUp = (extent.y >= extent.z);
        float modelHeight = isYUp ? extent.y : extent.z;
        float scale = 4.0f / modelHeight;  // normalize to ~4 units tall

        auto actor = scene->createActor("Model");
        if (isYUp) {
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0, 180, 0))));
        } else {
            float zCenter = (bmin.z + bmax.z) * 0.5f;
            float rotX = (zCenter < 0.0f) ? 90.0f : -90.0f;
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(rotX, 0, 0))));
        }
        actor->getTransform()->setScale(glm::vec3(scale));

        glm::vec3 center = (bmin + bmax) * 0.5f;
        actor->getTransform()->setPosition({-center.x * scale, -center.y * scale, -center.z * scale});

        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model); mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().roughness = 0.5f;

        std::cout << "Loaded: " << model->vertices.size() << " verts" << std::endl;
    }

    // One key light
    auto keyLight = scene->createActor("Key");
    auto kl = keyLight->addComponent<LightComponent>();
    kl->setLightType(LightType::Sphere);
    kl->setColor({1.0f, 0.95f, 0.9f});
    kl->setIntensity(8.0f);
    kl->setRadius(1.0f);
    keyLight->getTransform()->setPosition({4.0f, 3.0f, 4.0f});

    renderer.setScene(scene.get());
    renderer.updateSceneBuffers();

    auto& camera = renderer.getCamera();
    camera.setPosition({0, 0.5f, 8});
    camera.setFov(40.0f);
    camera.setRotation(0.0f, -90.0f);
    renderer.setRenderMode(RenderMode::PathTraced);

    std::cout << "Rendering..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < samples + 3; i++) renderer.render();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms" << std::endl;

    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
        std::cout << "Saved: " << output << std::endl;
    }
}
