// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/offscreen_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "render/camera/camera.hpp"

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
    uint32_t W = 1920, H = 1080;

    std::cout << "OHAO Model Viewer — " << modelPath << std::endl;
    std::cout << W << "x" << H << " @ " << samples << " spp" << std::endl;

    OffscreenRenderer renderer(W, H);
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
        glm::vec3 extent = bmax - bmin;
        bool isYUp = (extent.y >= extent.z);
        float modelHeight = isYUp ? extent.y : extent.z;
        float scale = (S * 1.6f) / modelHeight;

        auto actor = scene->createActor("Model");
        if (isYUp)
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0, 180, 0))));
        else
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(-90, 0, 0))));
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

    renderer.setScene(scene.get());
    renderer.updateSceneBuffers();

    auto& camera = renderer.getCamera();
    camera.setPosition({-3.0f, -0.5f, 6.0f});  // side angle, see armor detail
    camera.setFov(50.0f);
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
