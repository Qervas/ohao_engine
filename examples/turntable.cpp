// Turntable Camera Recording — renders orbit around model, saves frame sequence
// Usage: ./turntable <model.glb> <mode> [spp] [frames]
//   mode: cornell | dark | env
// Output: renders/turntable_NNNN.png

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "example_cli.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "scene/asset/model.hpp"
#include "render/camera/camera.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <cmath>
#include <cstdio>

using namespace ohao;
using ohao::examples::parseRenderFlags;
using ohao::examples::parseSpp;
using ohao::examples::argOr;
using ohao::examples::applyDenoiseOverride;
using ohao::examples::resolveMode;

// A quad wall. Rough by default; `metallic`/`roughness` make the mirror walls.
void addWall(ecs::World& world, VulkanRenderer& renderer, const std::string& name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color, float roughness = 0.95f, float metallic = 0.0f) {
    Model model;
    auto mkv = [&](glm::vec3 pos) {
        Vertex v{}; v.position = pos; v.normal = normal; v.color = color;
        v.texCoord = {0,0}; v.tangent = {1,0,0,1};
        v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
        return v;
    };
    model.vertices = {mkv(a), mkv(b), mkv(c), mkv(d)};
    model.indices = {0,1,2, 0,2,3};
    render::MaterialDesc mat;
    mat.baseColor = color;
    mat.roughness = roughness;
    mat.metallic = metallic;
    ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), model, name,
                    ecs::Transform{}, &mat);
}

// A light with the radius the old scene system defaulted to (0.5).
void addLight(ecs::World& world, const std::string& name, ecs::LightType type, glm::vec3 position,
              glm::vec3 color, float intensity, float radius = 0.5f) {
    ecs::Light light;
    light.type = type;
    light.color = color;
    light.intensity = intensity;
    light.radius = radius;
    ecs::Transform t;
    t.position = position;
    ecs::spawnLight(world, name, light, t);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: turntable <model.glb> <cornell|dark|env> [spp] [frames] [--denoise=…]\n";
        return 1;
    }

    const std::string modelPath{argOr(argc, argv, 1, "")};
    const std::string mode{argOr(argc, argv, 2, "cornell")};
    const int spp = parseSpp(argc > 3 ? argv[3] : nullptr, 64);
    const int totalFrames = parseSpp(argc > 4 ? argv[4] : nullptr, 120);
    const auto cli = parseRenderFlags(argc, argv, 5);
    const RenderMode rtMode = resolveMode(cli);

    const uint32_t W = 1280, H = 720;

    std::cout << "OHAO Turntable — " << mode << " mode, " << spp << " spp, " << totalFrames
              << " frames, "
              << (rtMode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline") << "\n";

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) return 1;

    // Creation order is draw order and light order. Declared after the renderer: the
    // World releases its RenderScene slots first.
    ecs::World world;
    ecs::installRenderExtract(world, renderer.renderScene());

    // Set up environment based on mode
    if (mode == "env") {
        renderer.setEnvironmentMap("assets/test_models/env_outdoor.hdr");
    } else if (mode == "mirror") {
        // Mirror room — closed box with mirror front + back walls
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
        const glm::vec3 mirror(0.95f, 0.95f, 0.95f);
        glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
        glm::vec3 LBF(-S,-S,S), RBF(S,-S,S), LTF(-S,S,S), RTF(S,S,S);
        // Side walls — red/green
        addWall(world, renderer, "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
        addWall(world, renderer, "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
        addWall(world, renderer, "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
        addWall(world, renderer, "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);
        // Front + back walls — MIRROR (low roughness, high metallic)
        addWall(world, renderer, "BackMirror",  LBB, RBB, RTB, LTB, {0,0,1},  mirror, 0.02f, 1.0f);
        addWall(world, renderer, "FrontMirror", LBF, RBF, RTF, LTF, {0,0,-1}, mirror, 0.02f, 1.0f);
    } else {
        // Cornell box or dark room — add walls (open front)
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
        glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
        glm::vec3 LBF(-S,-S,S), RBF(S,-S,S), LTF(-S,S,S), RTF(S,S,S);
        addWall(world, renderer, "Back",    LBB, RBB, RTB, LTB, {0,0,1},  white);
        addWall(world, renderer, "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
        addWall(world, renderer, "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
        addWall(world, renderer, "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
        addWall(world, renderer, "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);
    }

    // Load model
    auto model = std::make_shared<Model>();
    auto dot = modelPath.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
    bool loaded = (ext == "obj") ? model->loadFromOBJ(modelPath) : model->loadFromGLTF(modelPath);

    // Refuse rather than turntable an empty box. Without this the run
    // still exits 0 and still writes frames, so any caller checking the
    // exit code sees a success and gets pictures of nothing -- the exact
    // failure that hid a missing model from the golden corpus.
    if (!loaded) {
        std::cerr << "Failed to load: " << modelPath << std::endl
                  << "turntable has nothing to turn; refusing to render an empty scene."
                  << std::endl;
        return 2;
    }

    if (loaded) {
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        float modelHeight = std::max(bmax.y - bmin.y, bmax.z - bmin.z);
        float targetHeight = (mode == "mirror") ? 5.5f : 4.0f;
        float scale = targetHeight / modelHeight;

        ecs::Transform t;
        bool isYUp = ((bmax.y - bmin.y) >= (bmax.z - bmin.z));
        if (isYUp) {
            t.rotation = glm::quat(glm::radians(glm::vec3(0, 180, 0)));
        } else {
            float zCenter = (bmin.z + bmax.z) * 0.5f;
            float rotX = (zCenter < 0.0f) ? 90.0f : -90.0f;
            t.rotation = glm::quat(glm::radians(glm::vec3(rotX, 0, 0)));
        }
        t.scale = glm::vec3(scale);

        // Center model at origin, feet on floor (Y = -5 for Cornell, Y = 0 for env)
        glm::vec3 center = (bmin + bmax) * 0.5f;
        float floorY = (mode == "env") ? 0.0f : -5.0f;
        if (isYUp) {
            t.position = {-center.x*scale, floorY - bmin.y*scale, -center.z*scale};
        } else {
            // Z-up model: after rotation, Z maps to Y
            float footZ = (bmin.z + bmax.z > 0) ? bmin.z : bmax.z;  // whichever is the "bottom"
            t.position = {-center.x*scale, floorY + std::abs(footZ)*scale, -center.y*scale};
        }

        const render::MaterialDesc mat{};   // the default material
        ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), *model, "Model", t, &mat);
    }

    // Lighting based on mode
    if (mode == "dark") {
        // Very dim fill — emissive dominates
        addLight(world, "Fill", ecs::LightType::Point, {4, 4, 4}, {1, 1, 1}, 2.0f, 0.3f);
    } else {
        // Key + fill
        addLight(world, "Key", ecs::LightType::Point, {3, 4, 3}, {1, 0.95f, 0.9f},
                 (mode == "env") ? 8.0f : (mode == "mirror") ? 20.0f : 25.0f, 1.0f);

        if (mode == "cornell") {
            // Facing down (-Z onto -Y: the raster pass reads the direction), with the
            // edges given in local space so the world edges stay (3,0,0)/(0,0,3) (to ~1e-6).
            ecs::Transform t;
            t.position = {-1.5f, 4.99f, -1.5f};
            t.rotation = ecs::lookRotation(glm::vec3(0, -1, 0));
            ecs::Light area;
            area.type = ecs::LightType::AreaRect;
            area.color = {1, 0.98f, 0.92f};
            area.intensity = 12.0f;
            area.radius = 0.5f;
            area.areaEdge1 = glm::inverse(t.rotation) * glm::vec3(3, 0, 0);
            area.areaEdge2 = glm::inverse(t.rotation) * glm::vec3(0, 0, 3);
            ecs::spawnLight(world, "CeilingPanel", area, t);
        }
    }

    renderer.setRenderMode(rtMode);
    applyDenoiseOverride(renderer, cli);

    std::cout << "Denoise mode: " << denoiseModeName(renderer.getDenoiseMode()) << "\n";

    // Render turntable orbit — adjust radius per mode
    float orbitRadius = (mode == "env") ? 8.0f : (mode == "mirror") ? 4.5f : 4.2f;
    float orbitHeight = (mode == "env") ? 1.0f : -3.5f;

    system("mkdir -p renders/turntable");

    for (int frame = 0; frame < totalFrames; frame++) {
        float t = float(frame) / float(totalFrames);
        float angle = t * 2.0f * 3.14159f;  // full 360° orbit

        float cx = orbitRadius * cos(angle);
        float cz = orbitRadius * sin(angle);

        auto& camera = renderer.getCamera();
        camera.setPosition({cx, orbitHeight, cz});
        // Look at origin
        float yaw = glm::degrees(atan2(-cz, -cx));
        camera.setRotation(15.0f, yaw);  // tilt up from floor level
        camera.setFov(70.0f);

        renderer.resetAccumulation();

        for (int s = 0; s < spp; s++) {   // one frame per sample; the last is written
            world.sync();   // headless: no Application drives the systems
            if (s == spp - 1) renderer.requestReadback();
            renderer.render();
        }

        char filename[256];
        snprintf(filename, sizeof(filename), "renders/turntable/%s_%04d.png", mode.c_str(), frame);

        const auto pixels = renderer.readback();
        if (!pixels.empty()) {
            stbi_write_png(filename, static_cast<int>(W), static_cast<int>(H), 4,
                           pixels.data(), static_cast<int>(W * 4));
        }

        printf("\rFrame %d/%d (%.0f%%)", frame + 1, totalFrames, (frame + 1) * 100.0f / totalFrames);
        fflush(stdout);
    }

    printf("\nDone! Frames saved to renders/turntable/\n");
    printf("To make video: ffmpeg -framerate 30 -i renders/turntable/%s_%%04d.png -c:v libx264 -pix_fmt yuv420p renders/%s_turntable.mp4\n", mode.c_str(), mode.c_str());
}
