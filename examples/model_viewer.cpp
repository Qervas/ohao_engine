// Model Viewer — load any GLTF/OBJ and render in a Cornell box
// Usage: ./model_viewer <model.glb> [output.png] [samples] [deferred [reference]|rt_realtime|rt_offline] [flip] [video]
//                       [--denoise=X] [--print-bounds]
//        ./model_viewer <model.glb> [flip] --save-scene <path.scene.json>   (writes the scene, renders nothing)
//
// A model inside the project (the working directory) becomes a MeshSource asset
// reference; one outside it is attached as an in-memory mesh, which a saved scene cannot
// describe (W_UNSAVEABLE_MESH).

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "scene/asset/model_loader.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/spawn.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "example_cli.hpp"
#include "example_scenes.hpp"
#include "runtime/scene_view_apply.hpp"

#include <iostream>
#include <memory>
#include <optional>
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

namespace {

// The model's path relative to the project root, '/'-separated; none when it lies
// outside the project (or on another drive).
std::optional<ecs::AssetRef> projectAsset(const std::filesystem::path& modelPath, const std::filesystem::path& root) {
    std::error_code ec;
    const auto rel = std::filesystem::weakly_canonical(std::filesystem::absolute(modelPath), ec).lexically_relative(
        std::filesystem::weakly_canonical(root, ec));
    if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == "..") return std::nullopt;
    return ecs::AssetRef{rel.generic_string(), {}};
}

} // namespace

int main(int rawArgc, char* rawArgv[]) {
    auto args = examples::extractSaveScene(rawArgc, rawArgv);
    if (args.missingValue) {
        std::cerr << "--save-scene needs a path\n";
        return 1;
    }
    const int argc = args.argc();
    char** argv = args.argv.data();
    if (argc < 2) {
        std::cout << "Usage: model_viewer <model.glb|.obj> [output.png] [samples] "
                     "[deferred [reference]|rt_realtime|rt_offline] [flip] [video] [--denoise=…] [--print-bounds] "
                     "[--save-scene <path>]\n";
        return 1;
    }

    const std::string modelPath{argOr(argc, argv, 1, "")};
    const std::string output{argOr(argc, argv, 2, "model_output.png")};
    const int samples = parseSpp(argc > 3 ? argv[3] : nullptr, 1024);
    // 1080p default: dual PathTracer (realtime+offline) at 4K OOMs on 8 GiB cards
    // and the golden harness only needs a stable offline frame. Override later if needed.
    const uint32_t W = 1920, H = 1080;
    // --save-scene takes no output/samples, so its flags may follow the model directly.
    auto cli = parseRenderFlags(argc, argv, args.path ? 2 : 4);

    std::cout << "OHAO Model Viewer — " << modelPath << "\n";

    // Load the model first: the room adapts to its size. A model inside the project
    // loads through the resolver, whose cache instantiate then reuses.
    ecs::FileAssetResolver assets(std::filesystem::current_path());
    std::optional<ecs::AssetRef> asset = projectAsset(modelPath, assets.root());
    std::shared_ptr<const Model> model;
    if (asset) {
        if (auto loaded = assets.loadModel(*asset)) model = std::move(loaded.value());
        else std::cerr << "[ModelLoader] " << loaded.error().code << ": " << loaded.error().message << "\n";
    } else if (auto loaded = ModelLoader::loadResult(modelPath)) {
        model = std::move(loaded.value());
    } else {
        std::cerr << "[ModelLoader] " << loaded.error() << "\n";
    }

    if (!model) {
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

    // Framing (room size, camera, lights) from the model bounds; glTF is Y up.
    const FrameResult frame = SceneFramer::computeFraming(*model);

    if (args.path) {
        ecs::World world;
        world.seedIds(examples::kModelViewerIdSeed);
        examples::buildModelViewerScene(world, *model, frame, asset, cli.flipY);
        return examples::writeExampleScene(world, *args.path);
    }

    std::cout << W << "x" << H << " @ " << samples << " spp\n";
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    // Declared after the renderer: the World releases its RenderScene slots (and
    // instantiate its meshes) first.
    ecs::World world;
    world.seedIds(examples::kModelViewerIdSeed);
    ecs::installRenderExtract(world, renderer.renderScene());
    ecs::installInstantiate(world, renderer.renderScene(), renderer.getTextureManager(), assets);
    const examples::ViewerScene scene = examples::buildModelViewerScene(world, *model, frame, asset, cli.flipY);
    if (!asset) {
        const ecs::Material o = examples::viewerModelMaterial(*model);
        render::MaterialDesc mat;
        mat.baseColor = o.baseColor;
        mat.roughness = o.specularRoughness;
        mat.metallic = o.baseMetalness;
        ecs::attachModel(world, scene.model, renderer.renderScene(), renderer.getTextureManager(), *model, "Model", &mat);
    }
    std::cout << "Loaded: " << model->vertices.size() << " verts, scale=" << frame.modelScale << std::endl;
    for (const auto& w : runtime::applySceneView(renderer, ecs::resolveSceneView(world), assets).warnings)
        std::cerr << "warning: " << w << "\n";
    auto& camera = renderer.getCamera();

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
            const glm::mat4 toWorld = scene.modelTransform.matrix();
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
    applyQuality(renderer, cli);
    std::cout << "Denoise mode: " << denoiseModeName(renderer.getDenoiseMode()) << "\n";

    if (cli.useDeferred && renderer.getDeferredRenderer()) {
        if (auto* pp = renderer.getDeferredRenderer()->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setEv100(0.0f);
        }
    }

    // Headless: no Application drives the systems, so every frame syncs the world.
    // `keep`: read this frame back (renderer.readback() after it).
    bool firstFrame = true;
    const auto renderFrame = [&](bool keep = false) {
        world.sync();
        if (firstFrame && !ecs::instantiateErrors(world).empty()) {
            std::cerr << "FATAL: meshes could not be built (see the instantiate errors above)\n";
            std::exit(2);
        }
        firstFrame = false;
        if (keep) renderer.requestReadback();
        renderer.render();
    };

    std::cout << "Rendering ("
              << (cli.useDeferred ? "Deferred+RT"
                                  : (mode == RenderMode::RTRealtime ? "RTRealtime" : "RTOffline"))
              << ")...\n";
    auto start = std::chrono::high_resolution_clock::now();
    // Deferred: 30 frames (TAA and RT GI histories converge); path traced: one frame per
    // sample. The last frame is the one written.
    const int frames = cli.useDeferred ? 30 : samples;

    if (cli.video && cli.useDeferred) {
        constexpr int fps = 30;
        constexpr int seconds = 3;
        const int totalFrames = fps * seconds;
        const std::string videoDir = output.substr(0, output.find_last_of('.'));
        std::filesystem::create_directories(videoDir);

        for (int i = 0; i < 5; i++) renderFrame();

        std::cout << "Recording " << totalFrames << " frames to " << videoDir << "/\n";
        for (int i = 0; i < totalFrames; i++) {
            renderFrame(true);
            const auto px = renderer.readback();
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

    for (int i = 0; i < frames; i++) renderFrame(i == frames - 1);
    const auto pixels = renderer.readback();   // denoised when a CPU denoiser is on
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "Done: " << ms << " ms\n";

    if (!pixels.empty()) {
        stbi_write_png(output.c_str(), static_cast<int>(W), static_cast<int>(H), 4,
                       pixels.data(), static_cast<int>(W * 4));
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << "\n";
    }
}
