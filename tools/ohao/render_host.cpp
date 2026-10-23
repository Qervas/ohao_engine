#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "ohao/render_host.hpp"
#include "ohao/scene_host.hpp"

#include "ecs/components.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_view.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace ohao::host {

namespace {

reflect::Diagnostic renderError(std::string message) {
    reflect::Diagnostic d;
    d.code = "E_RENDER";
    d.message = std::move(message);
    return d;
}

std::string hexId(uint64_t id) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, id >>= 4) s[static_cast<std::size_t>(i)] = kDigits[id & 15u];
    return s;
}

} // namespace

RenderHost::RenderHost(ecs::AssetResolver& assets) : m_assets(assets) {}

// The World's systems hold references into the renderer's RenderScene; the World is
// destroyed first (see the header), so the renderer can go now.
RenderHost::~RenderHost() = default;

api::RenderOutcome RenderHost::capture(const api::RenderRequest& r) {
    api::RenderOutcome out;
    const auto start = std::chrono::steady_clock::now();
    if (!m_world) {
        out.errors.push_back(renderError("the render host has no world"));
        return out;
    }
    ecs::World& world = *m_world;
    if (!m_renderer) {
        if (m_failed) {
            out.errors.push_back(renderError("the renderer could not be started (see the log)"));
            return out;
        }
        auto renderer = std::make_unique<VulkanRenderer>(r.width, r.height);
        if (!renderer->initialize()) {
            m_failed = true;
            out.errors.push_back(renderError("no usable GPU: the renderer could not be initialized"));
            return out;
        }
        m_renderer = std::move(renderer);
        if (auto* deferred = m_renderer->getDeferredRenderer())
            if (auto* pp = deferred->getPostProcessing()) m_defaultExposure = pp->getExposure();
        ecs::installRenderExtract(world, m_renderer->renderScene());
        ecs::installInstantiate(world, m_renderer->renderScene(), m_renderer->getTextureManager(), m_assets);
    } else {
        m_renderer->resize(r.width, r.height);   // a no-op at the same size
    }
    VulkanRenderer& renderer = *m_renderer;

    // As the examples: the view first, then the mode, the denoiser and post effects.
    for (const auto& w : applySceneView(renderer, ecs::resolveSceneView(world), m_assets).warnings)
        std::cerr << "[ohao] warning: " << w << "\n";
    const RenderMode mode = r.mode == "rt_realtime" ? RenderMode::RTRealtime
                            : r.mode == "rt_offline" ? RenderMode::RTOffline
                                                     : RenderMode::Deferred;
    renderer.setRenderMode(mode);
    renderer.setDenoiseMode(parseDenoiseMode(r.denoise));
    if (mode == RenderMode::Deferred && renderer.getDeferredRenderer()) {
        if (auto* pp = renderer.getDeferredRenderer()->getPostProcessing()) {
            const auto on = [&](std::string_view e) { return std::find(r.post.begin(), r.post.end(), e) != r.post.end(); };
            pp->setBloomEnabled(on("bloom"));
            pp->setTAAEnabled(on("taa"));
            pp->setSSAOEnabled(on("ssao"));
            pp->setExposure(r.exposure.value_or(m_defaultExposure));
        }
    }
    // A later capture starts its own accumulation; the first one renders exactly as a
    // fresh renderer does (the examples), so the goldens hold.
    if (m_captures > 0) renderer.resetAccumulation();
    ++m_captures;

    const int frames = r.frames > 0 ? r.frames : (mode == RenderMode::Deferred ? 10 : r.spp + 3);
    for (int i = 0; i < frames; ++i) {
        world.sync();   // derived state only: rendering never steps the simulation
        if (i == 0) {
            const auto failed = ecs::instantiateErrors(world);
            for (const entt::entity e : failed) {
                const auto& err = world.registry().get<ecs::InstantiateError>(e);
                reflect::Diagnostic d;
                d.code = err.code;
                d.entity = world.idOf(e);
                d.message = "entity " + hexId(d.entity) + ": " + err.message;
                out.errors.push_back(std::move(d));
            }
            if (!out.errors.empty()) return out;
        }
        renderer.render();
    }
    const auto pixels = renderer.getPixelSpan();
    if (pixels.empty()) {
        out.errors.push_back(renderError("the renderer produced no image"));
        return out;
    }
    if (!stbi_write_png(r.path.c_str(), static_cast<int>(r.width), static_cast<int>(r.height), 4, pixels.data(),
                        static_cast<int>(r.width * 4))) {
        out.errors.push_back(renderError("cannot write " + r.path));
        return out;
    }
    out.frames = frames;
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (renderer.validationEnabled()) out.validationMessages = renderer.validationMessageCount();
    return out;
}

} // namespace ohao::host
