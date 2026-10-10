// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples]

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "scene/asset/model_loader.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "ecs/framing.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "example_cli.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <chrono>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

using namespace ohao;
using ohao::examples::parseRenderFlags;
using ohao::examples::parseSpp;
using ohao::examples::argOr;
using ohao::examples::applyDenoiseOverride;
using ohao::examples::resolveMode;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: model_viewer <model.glb|.obj> [output.png] [samples] "
                     "[deferred|rt_realtime|rt_offline] [flip] [video] [--denoise=…] [--print-bounds]\n";
        return 1;
    }

    const std::string modelPath{argOr(argc, argv, 1, "")};
    const std::string output{argOr(argc, argv, 2, "model_output.png")};
    const int samples = parseSpp(argc > 3 ? argv[3] : nullptr, 1024);
    // 1080p default: dual PathTracer (realtime+offline) at 4K OOMs on 8 GiB cards
    // and the golden harness only needs a stable offline frame. Override later if needed.
    const uint32_t W = 1920, H = 1080;
    auto cli = parseRenderFlags(argc, argv, 4);

    std::cout << "OHAO Model Viewer — " << modelPath << "\n";
    std::cout << W << "x" << H << " @ " << samples << " spp\n";

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    // Load model first — room size adapts to model (Result API)
    std::shared_ptr<Model> model;
    bool loaded = false;
    if (auto loadedModel = ModelLoader::loadResult(modelPath)) {
        model = std::move(loadedModel.value());
        loaded = true;
    } else {
        std::cerr << "[ModelLoader] " << loadedModel.error() << "\n";
    }

    if (!loaded) {
        // EXIT, do not carry on. Rendering an empty scene after a failed load
        // writes a perfectly valid PNG of nothing, and a caller that checks the
        // exit code and the file's existence -- which is exactly what
        // tests/golden/render_golden.py checks -- sees a successful render.
        // The metal_rough_spheres golden scene sat in that state undetected:
        // its .glb was absent, every run produced an empty frame, and the
        // corpus reported a large pixel difference rather than a missing asset.
        std::cerr << "Failed to load: " << modelPath << std::endl
                  << "model_viewer renders nothing without a model; refusing to "
                     "write an empty image that would read as a successful render."
                  << std::endl;
        return 2;
    }

    // Compute framing (room size, camera, lights) from model bounds
    const FrameResult frame = SceneFramer::computeFraming(model->vertices);
    const float S = frame.roomHalfSize;

    // Creation order is draw order and light order. Declared after the renderer: the
    // World releases its RenderScene slots first.
    ecs::World world;
    ecs::installRenderExtract(world, renderer.renderScene());
    const auto spawn = [&](const Model& m, const std::string& name, const ecs::Transform& t,
                           const render::MaterialDesc& mat) {
        return ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), m, name, t, &mat);
    };

    // Helper: create a solid quad (wall/floor/furniture face)
    auto addQuad = [&](const std::string& name, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                       glm::vec3 normal, glm::vec3 color, float roughness) {
        Model m;
        auto mkv = [&](glm::vec3 pos) {
            Vertex v{}; v.position = pos; v.normal = normal; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        m.vertices = {mkv(a), mkv(b), mkv(c), mkv(d)};
        m.indices = {0,1,2, 0,2,3};
        render::MaterialDesc mat;
        mat.baseColor = color;
        mat.roughness = roughness;
        spawn(m, name, ecs::Transform{}, mat);
    };

    // Helper: create a box (6 faces) for furniture
    auto addBox = [&](const std::string& name, glm::vec3 lo, glm::vec3 hi,
                      glm::vec3 color, float roughness, float metallic = 0.0f) {
        Model m;
        auto mkv = [&](glm::vec3 pos, glm::vec3 n) {
            Vertex v{}; v.position = pos; v.normal = n; v.color = color;
            v.texCoord = {0,0}; v.tangent = {1,0,0,1};
            v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
            return v;
        };
        glm::vec3 c[8] = {
            {lo.x,lo.y,lo.z}, {hi.x,lo.y,lo.z}, {hi.x,hi.y,lo.z}, {lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z}, {hi.x,lo.y,hi.z}, {hi.x,hi.y,hi.z}, {lo.x,hi.y,hi.z}
        };
        // 6 faces: front, back, left, right, top, bottom
        int faces[6][4] = {{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{3,7,6,2},{0,1,5,4}};
        glm::vec3 normals[6] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; f++) {
            uint32_t base = (uint32_t)m.vertices.size();
            for (int k = 0; k < 4; k++) m.vertices.push_back(mkv(c[faces[f][k]], normals[f]));
            m.indices.insert(m.indices.end(), {base,base+1,base+2, base,base+2,base+3});
        }
        render::MaterialDesc mat;
        mat.baseColor = color;
        mat.roughness = roughness;
        mat.metallic = metallic;
        spawn(m, name, ecs::Transform{}, mat);
    };

    // === BEDROOM SCENE ===
    glm::vec3 wallColor(0.55f, 0.50f, 0.45f);   // muted warm gray walls
    glm::vec3 floorColor(0.18f, 0.10f, 0.05f);  // dark walnut wood
    glm::vec3 ceilColor(0.60f, 0.58f, 0.55f);   // gray ceiling

    // Walls — extend 0.1 past corners to prevent ray leaks at seams
    float E = 0.1f;
    addQuad("Back",    {-S-E,-S-E,-S},{S+E,-S-E,-S},{S+E,S+E,-S},{-S-E,S+E,-S},  {0,0,1},  wallColor, 0.9f);
    // No front wall — camera renders from outside looking in
    addQuad("Left",    {-S,-S-E,-S-E},{-S,-S-E,S+E},{-S,S+E,S+E},{-S,S+E,-S-E},  {1,0,0},  wallColor, 0.9f);
    addQuad("Right",   {S,-S-E,S+E},{S,-S-E,-S-E},{S,S+E,-S-E},{S,S+E,S+E},      {-1,0,0}, wallColor, 0.9f);
    addQuad("Floor",   {-S-E,-S,-S-E},{S+E,-S,-S-E},{S+E,-S,S+E},{-S-E,-S,S+E},  {0,1,0},  floorColor, 0.95f);
    addQuad("Ceiling", {-S-E,S,-S-E},{S+E,S,-S-E},{S+E,S,S+E},{-S-E,S,S+E},      {0,-1,0}, ceilColor, 0.9f);

    // Bed — behind the model, against back wall
    float bedW = S * 0.8f, bedH = S * 0.25f, bedD = S * 0.6f;
    glm::vec3 bedColor(0.85f, 0.82f, 0.78f);     // linen white sheets
    glm::vec3 frameColor(0.20f, 0.12f, 0.06f);    // dark wood frame
    addBox("BedFrame", {-bedW, -S, -S}, {bedW, -S + bedH * 0.4f, -S + bedD}, frameColor, 0.5f);
    addBox("Mattress", {-bedW + 0.2f, -S + bedH * 0.4f, -S + 0.2f},
                       {bedW - 0.2f, -S + bedH, -S + bedD - 0.2f}, bedColor, 0.95f);
    // Headboard
    addBox("Headboard", {-bedW, -S, -S}, {bedW, -S + bedH * 2.5f, -S + 0.3f}, frameColor, 0.4f);
    // Pillow
    addBox("Pillow", {-bedW * 0.4f, -S + bedH, -S + 0.4f},
                     {bedW * 0.4f, -S + bedH + 1.0f, -S + bedD * 0.4f}, {0.92f, 0.90f, 0.88f}, 0.95f);

    // Nightstand — right side
    float nsX = S * 0.55f;
    glm::vec3 nsColor(0.22f, 0.14f, 0.07f);
    addBox("Nightstand", {nsX, -S, -S}, {nsX + 3.0f, -S + bedH * 1.5f, -S + 3.0f}, nsColor, 0.45f);

    // The model: one entity, one mesh with a submesh per material run. Its materials
    // come from the model; the override only feeds the legacy RT GI word (and a model
    // without materials). A single-material model keeps the old viewer's 0.8/0.7/0.6; a
    // multi-material one uses its own material 0.
    ecs::Transform modelTransform;
    modelTransform.position = frame.modelPosition;
    modelTransform.rotation = frame.modelRotation;
    modelTransform.scale = glm::vec3(frame.modelScale);
    const bool hasMultipleMaterials = !model->materialPerTriangle.empty() &&
        *std::max_element(model->materialPerTriangle.begin(), model->materialPerTriangle.end()) > 0;
    render::MaterialDesc modelMaterial;
    if (hasMultipleMaterials && !model->materialColors.empty()) {
        modelMaterial.baseColor = glm::vec3(model->materialColors[0]);
        modelMaterial.roughness = model->materialColors[0].w;
        modelMaterial.metallic = model->materialMetallic.empty() ? 0.0f : model->materialMetallic[0];
    } else {
        modelMaterial.baseColor = {0.8f, 0.7f, 0.6f};
        modelMaterial.roughness = 0.5f;
    }
    const entt::entity modelEntity = spawn(*model, "Model", modelTransform, modelMaterial);
    std::cout << "Loaded: " << model->vertices.size() << " verts, scale=" << frame.modelScale << std::endl;

    // Environment map
    renderer.setEnvironmentMap("assets/test_models/env_studio.hdr");

    // Auto-framed lights (scaled to room size)
    ecs::spawnFramedLights(world, frame);

    // Auto-framed camera
    auto& camera = renderer.getCamera();
    SceneFramer::applyCamera(camera, frame);

    // "flip" — rotate model 180° Y (models facing away from camera)
    if (cli.flipY) {
        modelTransform.rotation = glm::quat(glm::radians(glm::vec3(0, 180, 0)));
        world.setTransform(modelEntity, modelTransform);
    }

    // "--print-bounds": the model's screen-space box in the path tracer's framing (the
    // 8 corners of the model's world AABB through the camera the RT pass uses), for
    // comparing renders inside and outside the model.
    for (int a = 1; a < argc; ++a) {
        if (std::string_view(argv[a]) != "--print-bounds") continue;
        const glm::mat4 view = camera.getViewMatrix();
        const glm::mat4 proj = glm::perspective(glm::radians(camera.getFov()), float(W) / float(H), 0.1f, 1000.0f);
        glm::vec2 lo(1e30f), hi(-1e30f);
        if (!model->vertices.empty()) {
            glm::vec3 bmin(1e30f), bmax(-1e30f);
            for (const auto& v : model->vertices) { bmin = glm::min(bmin, v.position); bmax = glm::max(bmax, v.position); }
            const glm::mat4 toWorld = modelTransform.matrix();
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                const glm::vec4 clip = proj * view * toWorld * glm::vec4(corner, 1.0f);
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                // Path tracer raygen: pixel row 0 is the top (ndc.y = +1).
                const glm::vec2 px((ndc.x * 0.5f + 0.5f) * float(W), (0.5f - ndc.y * 0.5f) * float(H));
                lo = glm::min(lo, px); hi = glm::max(hi, px);
            }
        }
        std::cout << "BOUNDS " << lo.x << " " << lo.y << " " << hi.x << " " << hi.y << " (of " << W << "x" << H << ")\n";
    }

    const RenderMode mode = resolveMode(cli);
    renderer.setRenderMode(mode);
    applyDenoiseOverride(renderer, cli);
    std::cout << "Denoise mode: " << denoiseModeName(renderer.getDenoiseMode()) << "\n";

    if (cli.useDeferred && renderer.getDeferredRenderer()) {
        if (auto* pp = renderer.getDeferredRenderer()->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }

    // Headless: no Application drives the systems, so every frame updates the world.
    const auto renderFrame = [&] {
        world.update(0.0f);
        renderer.render();
    };

    std::cout << "Rendering ("
              << (cli.useDeferred ? "Deferred+RT"
                                  : (mode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline"))
              << ")...\n";
    auto start = std::chrono::high_resolution_clock::now();
    const int frames = cli.useDeferred ? 30 : (samples + 3);

    if (cli.video && cli.useDeferred) {
        constexpr int fps = 30;
        constexpr int seconds = 3;
        const int totalFrames = fps * seconds;
        const std::string videoDir = output.substr(0, output.find_last_of('.'));
        std::filesystem::create_directories(videoDir);

        for (int i = 0; i < 5; i++) renderFrame();

        std::cout << "Recording " << totalFrames << " frames to " << videoDir << "/\n";
        for (int i = 0; i < totalFrames; i++) {
            renderFrame();
            const auto px = renderer.getPixelSpan();
            if (!px.empty()) {
                char fname[256];
                snprintf(fname, sizeof(fname), "%s/frame_%04d.png", videoDir.c_str(), i);
                stbi_write_png(fname, static_cast<int>(W), static_cast<int>(H), 4,
                               px.data(), static_cast<int>(W * 4));
            }
            if (i % 30 == 0) std::cout << "  frame " << i << "/" << totalFrames << "\n";
        }

        const std::string mp4 = videoDir + ".mp4";
        const std::string cmd = "ffmpeg -y -framerate " + std::to_string(fps)
            + " -i " + videoDir + "/frame_%04d.png"
            + " -c:v libx264 -pix_fmt yuv420p -crf 18 " + mp4 + " 2>/dev/null";
        std::cout << "Encoding: " << mp4 << "\n";
        system(cmd.c_str());
        std::cout << "Video saved: " << mp4 << "\n";
        return 0;
    }

    for (int i = 0; i < frames; i++) renderFrame();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms\n";

    const auto pixels = renderer.getPixelSpan();
    if (!pixels.empty()) {
        stbi_write_png(output.c_str(), static_cast<int>(W), static_cast<int>(H), 4,
                       pixels.data(), static_cast<int>(W * 4));
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << "\n";
    }
}
