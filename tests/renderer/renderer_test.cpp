// OHAO Renderer Smoke Test
// Minimal: init Vulkan, spawn a one-quad ECS scene, render 1 frame, verify output.
// For real scenes, use examples/cornell_box or examples/model_viewer.

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "render/camera/camera.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"

#include <iostream>
#include <string>

using namespace ohao;

int main(int argc, char* argv[]) {
    std::string output = argc > 1 ? argv[1] : "smoke_test.png";
    uint32_t W = 640, H = 480;

    std::cout << "=== OHAO Renderer Smoke Test ===" << std::endl;

    // 1. Initialize renderer
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FAIL: renderer init" << std::endl;
        return 1;
    }
    std::cout << "PASS: renderer init" << std::endl;

    // 2. Create minimal scene — single red quad
    ecs::World world;   // after the renderer: released first
    ecs::installRenderExtract(world, renderer.renderScene());
    Model model;
    Vertex v{};
    v.normal = {0, 0, 1}; v.color = {1, 0, 0}; v.tangent = {1, 0, 0, 1};
    v.boneIndices = glm::ivec4(0); v.boneWeights = {1, 0, 0, 0};
    v.position = {-1, -1, 0}; model.vertices.push_back(v);
    v.position = { 1, -1, 0}; model.vertices.push_back(v);
    v.position = { 1,  1, 0}; model.vertices.push_back(v);
    v.position = {-1,  1, 0}; model.vertices.push_back(v);
    model.indices = {0, 1, 2, 0, 2, 3};
    render::MaterialDesc mat;
    mat.baseColor = {1, 0, 0};
    ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), model, "TestQuad",
                    ecs::Transform{}, &mat);
    world.update(0.0f);
    if (renderer.renderScene().draws().empty()) {
        std::cerr << "FAIL: the quad entity reached no RenderScene draw" << std::endl;
        return 1;
    }
    std::cout << "PASS: scene entities" << std::endl;

    // 3. Render
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.render();
    std::cout << "PASS: render frame" << std::endl;

    // 4. Save
    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
        std::cout << "PASS: saved " << output << std::endl;
    }

    std::cout << "=== ALL PASS ===" << std::endl;
    return 0;
}
