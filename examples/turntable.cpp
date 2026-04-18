// Turntable Camera Recording — renders orbit around model, saves frame sequence
// Usage: ./turntable <model.glb> <mode> [spp] [frames]
//   mode: cornell | dark | env
// Output: renders/turntable_NNNN.png

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/oidn_denoise.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "render/camera/camera.hpp"

#include <iostream>
#include <string>
#include <cmath>
#include <cstdio>

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
    if (argc < 3) {
        std::cout << "Usage: turntable <model.glb> <cornell|dark|env> [spp] [frames]" << std::endl;
        return 1;
    }

    std::string modelPath = argv[1];
    std::string mode = argv[2];
    int spp = argc > 3 ? std::atoi(argv[3]) : 64;
    int totalFrames = argc > 4 ? std::atoi(argv[4]) : 120;  // 4 sec at 30fps
    RenderMode rtMode = RenderMode::RTOffline;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "rt_realtime") rtMode = RenderMode::RTRealtime;
        else if (arg == "rt_offline") rtMode = RenderMode::RTOffline;
    }

    uint32_t W = 1280, H = 720;

    std::cout << "OHAO Turntable — " << mode << " mode, " << spp << " spp, " << totalFrames
              << " frames, " << (rtMode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline") << std::endl;

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) return 1;

    auto scene = std::make_unique<Scene>("Turntable");

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
        addWall(scene.get(), "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
        addWall(scene.get(), "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
        addWall(scene.get(), "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
        addWall(scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);
        // Front + back walls — MIRROR (low roughness, high metallic)
        addWall(scene.get(), "BackMirror",  LBB, RBB, RTB, LTB, {0,0,1},  mirror);
        addWall(scene.get(), "FrontMirror", LBF, RBF, RTF, LTF, {0,0,-1}, mirror);
        // Override material to mirror
        for (auto name : {"BackMirror", "FrontMirror"}) {
            for (auto& [id, actor] : scene->getAllActors()) {
                if (actor->getName() == name) {
                    auto mat = actor->getComponent<MaterialComponent>();
                    if (mat) { mat->getMaterial().roughness = 0.02f; mat->getMaterial().metallic = 1.0f; }
                }
            }
        }
    } else {
        // Cornell box or dark room — add walls (open front)
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
        glm::vec3 LBB(-S,-S,-S), RBB(S,-S,-S), LTB(-S,S,-S), RTB(S,S,-S);
        glm::vec3 LBF(-S,-S,S), RBF(S,-S,S), LTF(-S,S,S), RTF(S,S,S);
        addWall(scene.get(), "Back",    LBB, RBB, RTB, LTB, {0,0,1},  white);
        addWall(scene.get(), "Left",    LBB, LTB, LTF, LBF, {1,0,0},  red);
        addWall(scene.get(), "Right",   RBB, RBF, RTF, RTB, {-1,0,0}, green);
        addWall(scene.get(), "Floor",   LBB, LBF, RBF, RBB, {0,1,0},  white);
        addWall(scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0,-1,0}, white);
    }

    // Load model
    auto model = std::make_shared<Model>();
    auto dot = modelPath.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? modelPath.substr(dot + 1) : "";
    bool loaded = (ext == "obj") ? model->loadFromOBJ(modelPath) : model->loadFromGLTF(modelPath);

    if (loaded) {
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model->vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        float modelHeight = std::max(bmax.y - bmin.y, bmax.z - bmin.z);
        float targetHeight = (mode == "mirror") ? 5.5f : 4.0f;
        float scale = targetHeight / modelHeight;

        auto actor = scene->createActor("Model");
        bool isYUp = ((bmax.y - bmin.y) >= (bmax.z - bmin.z));
        if (isYUp) {
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(0, 180, 0))));
        } else {
            float zCenter = (bmin.z + bmax.z) * 0.5f;
            float rotX = (zCenter < 0.0f) ? 90.0f : -90.0f;
            actor->getTransform()->setRotation(glm::quat(glm::radians(glm::vec3(rotX, 0, 0))));
        }
        actor->getTransform()->setScale(glm::vec3(scale));

        // Center model at origin, feet on floor (Y = -5 for Cornell, Y = 0 for env)
        glm::vec3 center = (bmin + bmax) * 0.5f;
        float floorY = (mode == "env") ? 0.0f : -5.0f;
        if (isYUp) {
            actor->getTransform()->setPosition({-center.x*scale, floorY - bmin.y*scale, -center.z*scale});
        } else {
            // Z-up model: after rotation, Z maps to Y
            float footZ = (bmin.z + bmax.z > 0) ? bmin.z : bmax.z;  // whichever is the "bottom"
            actor->getTransform()->setPosition({-center.x*scale, floorY + std::abs(footZ)*scale, -center.y*scale});
        }

        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model); mesh->setVisible(true);
        actor->addComponent<MaterialComponent>();
    }

    // Lighting based on mode
    if (mode == "dark") {
        // Very dim fill — emissive dominates
        auto fill = scene->createActor("Fill");
        auto fl = fill->addComponent<LightComponent>();
        fl->setLightType(LightType::Sphere);
        fl->setColor({1, 1, 1});
        fl->setIntensity(2.0f);
        fl->setRadius(0.3f);
        fill->getTransform()->setPosition({4, 4, 4});
    } else {
        // Key + fill
        auto key = scene->createActor("Key");
        auto kl = key->addComponent<LightComponent>();
        kl->setLightType(LightType::Sphere);
        kl->setColor({1, 0.95f, 0.9f});
        kl->setIntensity((mode == "env") ? 8.0f : (mode == "mirror") ? 20.0f : 25.0f);
        kl->setRadius(1.0f);
        key->getTransform()->setPosition({3, 4, 3});

        if (mode == "cornell") {
            auto area = scene->createActor("CeilingPanel");
            auto al = area->addComponent<LightComponent>();
            al->setLightType(LightType::AreaRect);
            al->setColor({1, 0.98f, 0.92f});
            al->setIntensity(12.0f);
            al->setAreaEdges({3, 0, 0}, {0, 0, 3});
            area->getTransform()->setPosition({-1.5f, 4.99f, -1.5f});
        }
    }

    renderer.setScene(scene.get());
    renderer.setRenderMode(rtMode);

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

        for (int s = 0; s < spp + 3; s++)
            renderer.render();

        // OIDN denoise per frame
        std::vector<float> beautyBuf, albedoBuf, normalBuf;
        uint32_t rw, rh;
        char filename[256];
        snprintf(filename, sizeof(filename), "renders/turntable/%s_%04d.png", mode.c_str(), frame);

        if (renderer.readbackHDRBuffers(beautyBuf, albedoBuf, normalBuf, rw, rh)) {
            auto beauty3 = ohao::rgba32fToFloat3(beautyBuf.data(), rw, rh);
            auto albedo3 = ohao::rgba32fToFloat3(albedoBuf.data(), rw, rh);
            auto normal3 = ohao::rgba32fToFloat3(normalBuf.data(), rw, rh);
            ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(), rw, rh, true);
            auto rgba8 = ohao::float3ToRGBA8(beauty3.data(), rw, rh, 0.5f);
            stbi_write_png(filename, rw, rh, 4, rgba8.data(), rw * 4);
        } else {
            const uint8_t* pixels = renderer.getPixels();
            if (pixels) stbi_write_png(filename, W, H, 4, pixels, W * 4);
        }

        printf("\rFrame %d/%d (%.0f%%)", frame + 1, totalFrames, (frame + 1) * 100.0f / totalFrames);
        fflush(stdout);
    }

    printf("\nDone! Frames saved to renders/turntable/\n");
    printf("To make video: ffmpeg -framerate 30 -i renders/turntable/%s_%%04d.png -c:v libx264 -pix_fmt yuv420p renders/%s_turntable.mp4\n", mode.c_str(), mode.c_str());
}
