// OHAO Renderer Smoke Test
// Minimal: init Vulkan, spawn a one-quad ECS scene, render 1 frame, verify output.
// For real scenes, use examples/cornell_box or examples/model_viewer.
//
//   renderer_test [out.png]    default: <build>/renderer_test.png; last line is a JSON summary

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "render/renderer/vulkan_renderer.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "render/camera/camera.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/model.hpp"

#include "common/harness.hpp"   // the JSON summary line (ohao::test::finish)

#include <iostream>
#include <string>
#include <vector>

using namespace ohao;

namespace {

int g_passed = 0, g_failed = 0;
std::vector<std::string> g_failures;

bool check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << std::endl;
    if (ok) ++g_passed;
    else {
        ++g_failed;
        g_failures.emplace_back(what);
    }
    return ok;
}

int finish() { return ohao::test::finish("renderer_test", g_passed, g_failed, g_failures); }

} // namespace

int main(int argc, char* argv[]) {
    const std::string output = argc > 1 ? argv[1] : std::string(OHAO_BINARY_DIR) + "/renderer_test.png";
    uint32_t W = 640, H = 480;

    std::cout << "=== OHAO Renderer Smoke Test ===" << std::endl;

    // 1. Initialize renderer
    VulkanRenderer renderer(W, H);
    if (!check(renderer.initialize(), "renderer init")) return finish();

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
    if (!check(!renderer.renderScene().draws().empty(), "the quad entity reaches a RenderScene draw")) return finish();

    // 3. Render
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.requestReadback();
    renderer.render();
    const auto pixels = renderer.readback();
    check(pixels.size() == std::size_t{W} * H * 4, "a frame renders to pixels");

    // 4. Save
    if (!pixels.empty())
        check(stbi_write_png(output.c_str(), W, H, 4, pixels.data(), W * 4) != 0, "the frame is written as a PNG");
    return finish();
}
