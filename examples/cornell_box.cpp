// Cornell Box — classic path tracer reference scene
// Usage: ./cornell_box [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"
#include "render/camera/camera.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "example_cli.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <chrono>

using namespace ohao;
using ohao::examples::parseRenderFlags;
using ohao::examples::parseSpp;
using ohao::examples::argOr;
using ohao::examples::applyDenoiseOverride;
using ohao::examples::resolveMode;

void addWall(ecs::World& world, VulkanRenderer& renderer, std::string_view name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    Model model;
    PrimitiveMeshGenerator::generateQuad(model, a, b, c, d, normal, color);
    render::MaterialDesc mat;
    mat.baseColor = color;
    mat.roughness = 0.95f;
    ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), model, name,
                    ecs::Transform{}, &mat);
}

int main(int argc, char* argv[]) {
    const std::string output{argOr(argc, argv, 1, "cornell_box.png")};
    const int samples = parseSpp(argc > 2 ? argv[2] : nullptr, 1024);
    const uint32_t W = 1920, H = 1080;
    const auto cli = parseRenderFlags(argc, argv, 3);

    std::cout << "OHAO Cornell Box — " << W << "x" << H << " @ " << samples << " spp\n";

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    // Build the classic Cornell box. Creation order is draw order and light order.
    // Declared after the renderer: the World releases its RenderScene slots first.
    ecs::World world;
    ecs::installRenderExtract(world, renderer.renderScene());
    const float S = 5.0f;
    const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);

    glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
    glm::vec3 LBF(-S,-S,S),  RBF(S,-S,S),  LTF(-S,S,S),  RTF(S,S,S);

    addWall(world, renderer, "Back",    LBB, RBB, RTB, LTB, {0,0,1},  white);
    addWall(world, renderer, "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
    addWall(world, renderer, "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
    addWall(world, renderer, "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
    addWall(world, renderer, "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);

    // Two unit-diameter UV spheres (32 x 16)
    const auto sphere = PrimitiveMeshGenerator::makeSphere();

    // Metal sphere (left)
    {
        render::MaterialDesc mat;
        mat.baseColor = {0.95f, 0.93f, 0.88f};
        mat.roughness = 0.05f;
        mat.metallic = 1.0f;
        ecs::Transform t;
        t.position = {-2.0f, -S + 2.0f, 0.0f};
        t.scale = glm::vec3(2.0f);
        ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), *sphere, "MetalSphere", t, &mat);
    }

    // Dielectric sphere (right)
    {
        render::MaterialDesc mat;
        mat.baseColor = {0.9f, 0.95f, 1.0f};
        mat.roughness = 0.02f;
        ecs::Transform t;
        t.position = {2.5f, -S + 1.8f, 1.5f};
        t.scale = glm::vec3(1.8f);
        ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), *sphere, "GlassSphere", t, &mat);
    }

    // === Many lights — stress test for ReSTIR ===
    glm::vec3 lightColors[] = {
        {1.0f, 0.3f, 0.2f},  // red
        {0.2f, 1.0f, 0.3f},  // green
        {0.3f, 0.4f, 1.0f},  // blue
        {1.0f, 0.9f, 0.3f},  // yellow
        {1.0f, 0.5f, 0.0f},  // orange
        {0.8f, 0.2f, 1.0f},  // purple
        {0.0f, 1.0f, 1.0f},  // cyan
        {1.0f, 0.0f, 0.5f},  // magenta
        {1.0f, 1.0f, 1.0f},  // white
        {0.5f, 1.0f, 0.5f},  // light green
        {1.0f, 0.7f, 0.5f},  // warm
        {0.5f, 0.7f, 1.0f},  // cool
    };
    float lightPositions[][3] = {
        {-3, 4, -3}, {0, 4, -3}, {3, 4, -3},
        {-3, 4,  0}, {0, 4,  0}, {3, 4,  0},
        {-3, 4,  3}, {0, 4,  3}, {3, 4,  3},
        {-4, 0,  0}, {4, 0,  0}, {0, 0, -4},
    };
    for (int i = 0; i < 12; i++) {
        ecs::Light light;
        light.type = ecs::LightType::Point;
        light.color = lightColors[i];
        light.intensity = 5.0f;
        light.radius = 0.3f;
        ecs::Transform t;
        t.position = {lightPositions[i][0], lightPositions[i][1], lightPositions[i][2]};
        ecs::spawnLight(world, "Light" + std::to_string(i), light, t);
    }

    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);

    const RenderMode mode = resolveMode(cli);
    renderer.setRenderMode(mode);
    applyDenoiseOverride(renderer, cli);

    std::cout << "Denoise mode: " << denoiseModeName(renderer.getDenoiseMode()) << "\n";
    std::cout << "Rendering ("
              << (isRTRenderMode(mode) ? (mode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline")
                                       : "Deferred")
              << ")...\n";

    auto start = std::chrono::high_resolution_clock::now();
    const int frames = cli.useDeferred ? 10 : (samples + 3);
    for (int i = 0; i < frames; i++) {
        world.update(0.0f);   // headless: no Application drives the systems
        renderer.render();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms\n";

    // getPixelSpan() — same buffer as getPixels(); OIDN applied when denoise != None
    const auto pixels = renderer.getPixelSpan();
    if (!pixels.empty()) {
        stbi_write_png(output.c_str(), static_cast<int>(W), static_cast<int>(H), 4,
                       pixels.data(), static_cast<int>(W * 4));
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << "\n";
    }

    return 0;
}
