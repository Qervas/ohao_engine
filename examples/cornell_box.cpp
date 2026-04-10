// Cornell Box — classic path tracer reference scene
// Usage: ./cornell_box [output.png] [samples]

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

// Create a single quad (2 triangles)
void addQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    uint32_t base = static_cast<uint32_t>(verts.size());
    auto makeVert = [&](glm::vec3 pos) {
        Vertex v{};
        v.position = pos;
        v.normal = normal;
        v.color = color;
        v.texCoord = glm::vec2(0);
        v.tangent = glm::vec4(1,0,0,1);
        v.boneIndices = glm::ivec4(0);
        v.boneWeights = glm::vec4(1,0,0,0);
        return v;
    };
    verts.push_back(makeVert(a));
    verts.push_back(makeVert(b));
    verts.push_back(makeVert(c));
    verts.push_back(makeVert(d));
    inds.push_back(base+0); inds.push_back(base+1); inds.push_back(base+2);
    inds.push_back(base+0); inds.push_back(base+2); inds.push_back(base+3);
}

void addWall(Scene* scene, const std::string& name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model);
    mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = 0.95f;
}

int main(int argc, char* argv[]) {
    std::string output = argc > 1 ? argv[1] : "cornell_box.png";
    int samples = argc > 2 ? std::atoi(argv[2]) : 1024;
    uint32_t W = 1920, H = 1080;

    std::cout << "OHAO Cornell Box — " << W << "x" << H << " @ " << samples << " spp" << std::endl;

    OffscreenRenderer renderer(W, H);
    if (!renderer.initialize()) { std::cerr << "FATAL: renderer init failed" << std::endl; return 1; }

    // Build the classic Cornell box
    auto scene = std::make_unique<Scene>("Cornell Box");
    const float S = 5.0f;
    const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);

    glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
    glm::vec3 LBF(-S,-S,S),  RBF(S,-S,S),  LTF(-S,S,S),  RTF(S,S,S);

    addWall(scene.get(), "Back",    LBB, RBB, RTB, LTB, {0,0,1},  white);
    addWall(scene.get(), "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
    addWall(scene.get(), "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
    addWall(scene.get(), "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
    addWall(scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);

    // Metal sphere (left)
    auto metalSphere = scene->createActorWithComponents("MetalSphere", PrimitiveType::Sphere);
    metalSphere->getTransform()->setPosition({-2.0f, -S + 2.0f, 0.0f});
    metalSphere->getTransform()->setScale(glm::vec3(2.0f));
    auto m1 = metalSphere->getComponent<MaterialComponent>();
    m1->getMaterial().baseColor = {0.95f, 0.93f, 0.88f};
    m1->getMaterial().roughness = 0.05f;
    m1->getMaterial().metallic = 1.0f;

    // Dielectric sphere (right)
    auto glassSphere = scene->createActorWithComponents("GlassSphere", PrimitiveType::Sphere);
    glassSphere->getTransform()->setPosition({2.5f, -S + 1.8f, 1.5f});
    glassSphere->getTransform()->setScale(glm::vec3(1.8f));
    auto m2 = glassSphere->getComponent<MaterialComponent>();
    m2->getMaterial().baseColor = {0.9f, 0.95f, 1.0f};
    m2->getMaterial().roughness = 0.02f;

    // === Scene lights ===
    // Key light — warm white from above
    auto keyLight = scene->createActor("KeyLight");
    auto kl = keyLight->addComponent<LightComponent>();
    kl->setLightType(LightType::Sphere);
    kl->setColor({1.0f, 0.95f, 0.9f});
    kl->setIntensity(30.0f);
    kl->setRadius(1.0f);
    keyLight->getTransform()->setPosition({0.0f, 4.0f, 0.0f});

    renderer.setScene(scene.get());
    renderer.updateSceneBuffers();

    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
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
