// Cornell Box — classic path tracer reference scene
// Usage: ./cornell_box [output.png] [samples]

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

#include <iostream>
#include <optional>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "render/rt/denoise/denoise_types.hpp"

using namespace ohao;

// ── ReSTIR GI measurement probe ──────────────────────────────────────────────
// Renders the realtime Cornell box and reads back the diffuse-radiance AOV each
// frame. With OHAO_RESTIRGI_GIONLY=1 that AOV holds ONLY the ReSTIR diffuse-GI
// term, so we can measure (a) the frame-to-frame temporal variance on the diffuse
// walls (the "boil") and (b) the converged host-averaged mean (the unbiased check).
// Run it once ReSTIR-GI ON (default) and once OFF (OHAO_RESTIRGI_OFF=1), both with
// GIONLY=1, and diff the printed numbers.
static int runRestirProbe(VulkanRenderer& renderer, uint32_t W, uint32_t H) {
    const int warmup = std::getenv("OHAO_PROBE_WARMUP") ? std::atoi(std::getenv("OHAO_PROBE_WARMUP")) : 48;
    const int meanFrames = std::getenv("OHAO_PROBE_MEAN") ? std::atoi(std::getenv("OHAO_PROBE_MEAN")) : 256;

    // Wall region: upper-center of the frame = back wall + a little ceiling.
    // Excludes the two spheres (lower-center) and the frame edges.
    const uint32_t x0 = (uint32_t)(0.15f * W), x1 = (uint32_t)(0.85f * W);
    const uint32_t y0 = (uint32_t)(0.10f * H), y1 = (uint32_t)(0.45f * H);
    auto lum = [](const float* p) { return 0.2126f*p[0] + 0.7152f*p[1] + 0.0722f*p[2]; };
    auto inWall = [&](uint32_t x, uint32_t y) { return x>=x0 && x<x1 && y>=y0 && y<y1; };

    std::vector<float> buf;
    uint32_t rw = 0, rh = 0;

    std::cout << "[probe] warmup " << warmup << " frames..." << std::endl;
    for (int i = 0; i < warmup; ++i) renderer.render();

    // --- Boil: frame-to-frame temporal variance of the GI on the walls ---
    // Averaged over many consecutive frame-deltas so the metric is not sensitive
    // to a single lucky/unlucky jittered frame pair.
    const int boilPairs = std::getenv("OHAO_PROBE_BOIL") ? std::atoi(std::getenv("OHAO_PROBE_BOIL")) : 16;
    std::vector<float> lumPrev, lumCur;
    renderer.render();
    if (!renderer.readbackDiffuseRadiance(buf, rw, rh)) { std::cerr << "readback failed\n"; return 1; }
    lumPrev.resize((size_t)rw*rh);
    for (size_t i = 0; i < (size_t)rw*rh; ++i) lumPrev[i] = lum(&buf[i*4]);

    double wallSumSqDelta = 0.0, wallSumLum = 0.0; size_t wallCnt = 0;
    double allSumSqDelta = 0.0; size_t allCnt = 0;
    for (int pr = 0; pr < boilPairs; ++pr) {
        renderer.render();
        if (!renderer.readbackDiffuseRadiance(buf, rw, rh)) { std::cerr << "readback failed\n"; return 1; }
        lumCur.resize((size_t)rw*rh);
        for (size_t i = 0; i < (size_t)rw*rh; ++i) lumCur[i] = lum(&buf[i*4]);
        for (uint32_t y = 0; y < rh; ++y)
            for (uint32_t x = 0; x < rw; ++x) {
                size_t i = (size_t)y*rw + x;
                double d = (double)lumCur[i] - (double)lumPrev[i];
                allSumSqDelta += d*d; allCnt++;
                if (inWall(x,y)) { wallSumSqDelta += d*d; wallSumLum += lumCur[i]; wallCnt++; }
            }
        lumPrev.swap(lumCur);
    }
    size_t wallN = wallCnt / std::max(boilPairs,1);
    double wallBoilRMS = std::sqrt(wallSumSqDelta / std::max<size_t>(wallCnt,1));
    double wallMeanLum = wallSumLum / std::max<size_t>(wallCnt,1);
    double allBoilRMS  = std::sqrt(allSumSqDelta / std::max<size_t>(allCnt,1));

    // --- Unbiased mean: host-average the GI-only AOV over many frames ---
    // acc holds the running sum of RGB per pixel (buf is RGBA32F, stride 4).
    std::vector<double> acc((size_t)rw*rh*3, 0.0);
    for (int f = 0; f < meanFrames; ++f) {
        renderer.render();
        if (!renderer.readbackDiffuseRadiance(buf, rw, rh)) { std::cerr << "readback failed\n"; return 1; }
        for (size_t p = 0; p < (size_t)rw*rh; ++p) {
            acc[p*3+0] += buf[p*4+0];
            acc[p*3+1] += buf[p*4+1];
            acc[p*3+2] += buf[p*4+2];
        }
    }
    const double inv = 1.0 / (double)meanFrames;
    double wallMean = 0.0; size_t wmN = 0;
    double allMean = 0.0;
    for (uint32_t y = 0; y < rh; ++y)
        for (uint32_t x = 0; x < rw; ++x) {
            size_t p = (size_t)y*rw + x;
            float rgb[3] = { (float)(acc[p*3+0]*inv), (float)(acc[p*3+1]*inv), (float)(acc[p*3+2]*inv) };
            double L = lum(rgb);
            allMean += L;
            if (inWall(x,y)) { wallMean += L; wmN++; }
        }
    wallMean /= std::max<size_t>(wmN,1);
    allMean  /= std::max<size_t>((size_t)rw*rh,1);

    const bool off = (std::getenv("OHAO_RESTIRGI_OFF") != nullptr);
    std::cout << "\n===== ReSTIR GI PROBE (" << (off ? "OFF / M=1 anchor" : "ON / temporal") << ") =====\n";
    std::cout << "  resolution              : " << rw << "x" << rh << "\n";
    std::cout << "  wall region             : x[" << x0 << "," << x1 << ") y[" << y0 << "," << y1 << ")  (" << wallN << " px)\n";
    std::cout << "  BOIL wall RMS delta     : " << wallBoilRMS << "   (frame-to-frame GI luminance jump)\n";
    std::cout << "  BOIL wall rel-RMS       : " << (wallBoilRMS / std::max(wallMeanLum,1e-6)) << "   (RMS / wall mean lum)\n";
    std::cout << "  BOIL whole-image RMS    : " << allBoilRMS << "\n";
    std::cout << "  MEAN wall GI luminance  : " << wallMean << "   (converged, " << meanFrames << " frames)\n";
    std::cout << "  MEAN whole-image GI lum : " << allMean << "\n";
    std::cout << "=================================================\n";
    return 0;
}

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
    RenderMode rtMode = RenderMode::RTOffline;
    bool useDeferred = false;
    bool probeMode = false;
    std::optional<ohao::DenoiseMode> denoiseOverride;
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "deferred") useDeferred = true;
        else if (arg == "rt_realtime") rtMode = RenderMode::RTRealtime;
        else if (arg == "rt_offline") rtMode = RenderMode::RTOffline;
        else if (arg == "restir_probe") { rtMode = RenderMode::RTRealtime; probeMode = true; }
        else if (arg.rfind("--denoise=", 0) == 0) {
            denoiseOverride = ohao::parseDenoiseMode(arg.substr(10));
        }
    }

    std::cout << "OHAO Cornell Box — " << W << "x" << H << " @ " << samples << " spp" << std::endl;

    VulkanRenderer renderer(W, H);
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
        auto l = scene->createActor("Light" + std::to_string(i));
        auto lc = l->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor(lightColors[i]);
        lc->setIntensity(5.0f);
        lc->setRadius(0.3f);
        l->getTransform()->setPosition({lightPositions[i][0], lightPositions[i][1], lightPositions[i][2]});
    }

    renderer.setScene(scene.get());

    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);

    renderer.setRenderMode(useDeferred ? RenderMode::Deferred : rtMode);

    if (denoiseOverride.has_value()) {
        renderer.setDenoiseMode(*denoiseOverride);
        std::cout << "Denoise mode (CLI override): "
                  << ohao::denoiseModeName(*denoiseOverride) << std::endl;
    } else {
        std::cout << "Denoise mode (preset): "
                  << ohao::denoiseModeName(renderer.getDenoiseMode()) << std::endl;
    }

    if (probeMode) {
        int rc = runRestirProbe(renderer, W, H);
        scene.reset();
        return rc;
    }

    const char* rtLabel = (rtMode == RenderMode::RTRealtime) ? "RTRealtime" : "RTOffline";
    std::cout << "Rendering (" << (useDeferred ? "Deferred+RT" : rtLabel) << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    int frames = useDeferred ? 10 : (samples + 3);
    for (int i = 0; i < frames; i++) renderer.render();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms" << std::endl;

    // getPixels() handles OIDN transparently if denoiseMode != None
    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == ohao::DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << std::endl;
    }

    // Destroy scene before renderer to avoid Vulkan/Jolt cleanup order crash
    scene.reset();
}
