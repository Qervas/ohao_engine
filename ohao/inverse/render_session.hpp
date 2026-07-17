#pragma once

// Cached render session: material/light updates without BLAS thrash.

#include "inverse/image_loss.hpp"
#include "inverse/quality.hpp"
#include "inverse/scene_builder.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <filesystem>
#include <string>

namespace ohao::inverse {

inline void applyEnv(VulkanRenderer& renderer, const std::string& path) {
    if (!path.empty() && std::filesystem::exists(path)) {
        renderer.setEnvironmentMap(path);
    }
}

// Avoid re-uploading the full scene/HDR every eval (was OOM/segfault under FD).
struct RenderSession {
    VulkanRenderer& renderer;
    InverseScene& inv;
    bool bound{false};

    ImageRGBA8 render(int viewIndex, const RenderBudget& budget, uint32_t seed,
                      DenoiseMode denoise) {
        // Prefer fixed size (set at construct). Resize only if explicitly needed
        // and sizes differ (cornell dual-res); studio keeps one size.
        const bool needResize =
            (renderer.getWidth() != budget.width || renderer.getHeight() != budget.height) &&
            budget.width > 0 && budget.height > 0;
        if (needResize) {
            renderer.resize(budget.width, budget.height);
            // RT images/pipelines recreated — force full scene rebind once.
            bound = false;
        }
        inv.applyCamera(renderer.getCamera(), viewIndex);
        renderer.setDenoiseMode(denoise);
        renderer.setRenderSeed(seed + static_cast<uint32_t>(viewIndex) * 9973u);
        renderer.setEnvIntensityScale(inv.currentEnvScale);
        if (!bound) {
            renderer.setScene(inv.scene.get());
            bound = true;
        } else {
            // Material + light/env-scale edits only — never rebuild BLAS / reload HDR.
            const bool matsOk = renderer.updateRTMaterialParams();
            const bool lightsOk =
                inv.needsLightUpdate() ? renderer.updateRTLightParams() : true;
            if (!matsOk || (inv.needsLightUpdate() && !lightsOk)) {
                (void)renderer.updateSceneBuffers();
            }
        }
        renderer.resetAccumulation();
        const int frames = budget.spp + 3;
        for (int i = 0; i < frames; ++i) renderer.render();
        return ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(),
                                    renderer.getPixelSpan());
    }

    void rebindEnv(const std::string& path) {
        applyEnv(renderer, path);
        bound = false; // next render will setScene and pick up new env once
    }
};


} // namespace ohao::inverse
