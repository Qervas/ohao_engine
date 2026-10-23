// Cornell Box — classic path tracer reference scene
// Usage: ./cornell_box [output.png] [samples] [deferred|rt_realtime|rt_offline] [--denoise=X]
//        ./cornell_box --save-scene <path.scene.json>   (writes the scene, renders nothing)

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_view.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "example_cli.hpp"
#include "example_scenes.hpp"
#include "ohao/scene_host.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <chrono>

using namespace ohao;
using ohao::examples::parseRenderFlags;
using ohao::examples::parseSpp;
using ohao::examples::argOr;
using ohao::examples::applyDenoiseOverride;
using ohao::examples::resolveMode;

int main(int rawArgc, char* rawArgv[]) {
    auto args = examples::extractSaveScene(rawArgc, rawArgv);
    if (args.missingValue) {
        std::cerr << "--save-scene needs a path\n";
        return 1;
    }
    const int argc = args.argc();
    char** argv = args.argv.data();

    // The world is authoring data only; --save-scene writes it without a renderer.
    if (args.path) {
        ecs::World world;
        world.seedIds(examples::kCornellBoxIdSeed);
        examples::buildCornellBox(world);
        return examples::writeExampleScene(world, *args.path);
    }

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

    // Declared after the renderer: the World releases its RenderScene slots (and
    // instantiate its meshes) first. The resolver outlives the World.
    ecs::FileAssetResolver assets(std::filesystem::current_path());
    ecs::World world;
    world.seedIds(examples::kCornellBoxIdSeed);
    ecs::installRenderExtract(world, renderer.renderScene());
    ecs::installInstantiate(world, renderer.renderScene(), renderer.getTextureManager(), assets);
    examples::buildCornellBox(world);
    for (const auto& w : host::applySceneView(renderer, ecs::resolveSceneView(world), assets).warnings)
        std::cerr << "warning: " << w << "\n";

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
        world.sync();   // headless: no Application drives the systems
        if (i == 0 && !ecs::instantiateErrors(world).empty()) {
            std::cerr << "FATAL: meshes could not be built (see the instantiate errors above)\n";
            return 2;
        }
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
