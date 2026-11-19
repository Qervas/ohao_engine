#include "deferred_renderer.hpp"
#include "gpu_culling.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "render/rt/emissive_lights.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "core/log.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ohao {

DeferredRenderer::~DeferredRenderer() {
    cleanup();
}

bool DeferredRenderer::initialize(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = m_outWidth = 1920;
    m_height = m_outHeight = 1080;

    OHAO_LOG(Debug, render, "DeferredRenderer: initializing...");

    // Initialize G-Buffer pass
    m_gbufferPass = std::make_unique<GBufferPass>();
    if (m_textureManager) {
        m_gbufferPass->setTextureManager(m_textureManager);
    }
    m_gbufferPass->setMeshShaderSupported(m_meshShader);
    m_gbufferPass->setAllocator(allocator);
    if (!m_gbufferPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: GBufferPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: GBufferPass OK");

    // Initialize CSM pass
    m_csmPass = std::make_unique<CSMPass>();
    m_csmPass->setAllocator(allocator);
    if (!m_csmPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: CSMPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: CSMPass OK");

    // GPU culling: the G-buffer's and CSM's indirect draw lists
    m_culling = std::make_unique<GpuCulling>();
    if (!m_culling->initialize(device, allocator, m_meshShader) ||
        !m_culling->resize(m_width, m_height, m_gbufferPass->depth())) {
        OHAO_LOG(Error, render, "DeferredRenderer: GpuCulling failed");
        return false;
    }
    m_gbufferPass->setCulling(m_culling.get());
    m_csmPass->setCulling(m_culling.get());

    // Clustered lighting: the light table and the froxel lists
    m_lightClusters = std::make_unique<LightClusters>();
    if (!m_lightClusters->initialize(device, allocator)) {
        OHAO_LOG(Error, render, "DeferredRenderer: LightClusters failed");
        return false;
    }

    // Initialize deferred lighting pass
    m_lightingPass = std::make_unique<DeferredLightingPass>();
    m_lightingPass->setAllocator(allocator);
    if (!m_lightingPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: LightingPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: LightingPass OK");

    // Connect G-Buffer to lighting
    m_lightingPass->setGBufferPass(m_gbufferPass.get());

    // Connect shadow map and cascade data to lighting
    m_lightingPass->setShadowMap(&m_csmPass->shadowMap(), m_csmPass->getShadowSampler());
    m_lightingPass->setCascadeBuffer(&m_csmPass->cascadeBuffer());

    // Initialize SSS pass (separable subsurface scattering blur)
    m_sssPass = std::make_unique<SSSPass>();
    m_sssPass->setAllocator(allocator);
    if (m_sssPass->initialize(device, physicalDevice)) {
        m_sssPass->setGBufferPass(m_gbufferPass.get());
        OHAO_LOG(Debug, render, "DeferredRenderer: SSSPass OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: SSSPass failed (non-fatal)");
        m_sssPass.reset();
    }

    // Initialize post-processing pipeline
    m_postProcessing = std::make_unique<PostProcessingPipeline>();
    m_postProcessing->setAllocator(allocator);
    if (!m_postProcessing->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: PostProcessing failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: PostProcessing OK");

    // Initialize gizmo pass (transform handles)
    m_gizmoPass = std::make_unique<GizmoPass>();
    m_gizmoPass->setAllocator(allocator);
    if (!m_gizmoPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: GizmoPass failed (non-fatal)");
        m_gizmoPass.reset();
    } else {
        OHAO_LOG(Debug, render, "DeferredRenderer: GizmoPass OK");
    }

    // The HUD over the final image (sub-project 7): drawn only on frames with a list.
    m_hudPass = std::make_unique<HudPass>();
    m_hudPass->setAllocator(allocator);
    if (!m_hudPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: HudPass failed (non-fatal: no HUD)");
        m_hudPass.reset();
    }

    // TAA reprojects with the G-buffer's depth and velocity.
    m_postProcessing->setGBufferInputs(&m_gbufferPass->depth(), &m_gbufferPass->velocity());

    // The RT traces alpha-test candidates (sub-project 12): RenderScene's tables (set 1) and the
    // bindless textures (set 2). Without them neither technique initializes.
    m_alphaTablesReady = m_rayQuery && m_textureManager && m_alphaTables.init(device, allocator);
    const VkDescriptorSetLayout alphaTables = m_alphaTablesReady ? m_alphaTables.layout() : VK_NULL_HANDLE;
    const VkDescriptorSetLayout textures = m_textureManager ? m_textureManager->getDescriptorSetLayout() : VK_NULL_HANDLE;

    // RT direct light (inline ray queries)
    m_rtDirect = std::make_unique<RTDirectTechnique>();
    m_rtDirect->setAlphaLayouts(alphaTables, textures);
    if (!m_rayQuery || !m_alphaTablesReady || !m_rtDirect->init(device, physicalDevice, allocator, m_width, m_height)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: RT direct light not available (lighting loops over the lights, CSM for the sun)");
        m_rtDirect.reset();
    }

    // Initialize RT GI technique (inline ray queries)
    m_rtGI = std::make_unique<RTGITechnique>();
    m_rtGI->setAlphaLayouts(alphaTables, textures);
    if (m_rayQuery && m_rtGI->init(device, physicalDevice, allocator, m_width, m_height)) {
        m_useRTGI = true;
        OHAO_LOG(Debug, render, "DeferredRenderer: RTGI OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: RTGI not available");
        m_rtGI.reset();
    }

    // Effects and blended surfaces (sub-project 12): the sun's shadow ray needs ray queries, the
    // flipbooks and materials the bindless textures.
    if (m_rayQuery && m_textureManager) {
        m_vfx = std::make_unique<VfxRenderer>();
        if (!m_vfx->initialize(device, physicalDevice, allocator, textures, m_gbufferPass->sceneSetLayout()) ||
            !m_vfx->resize(m_width, m_height, m_lightingPass->lit(), m_gbufferPass->depth())) {
            OHAO_LOG(Warn, render, "DeferredRenderer: VfxRenderer failed (no effects or blended surfaces drawn)");
            m_vfx.reset();
        }
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: no ray queries or bindless textures: no effects or blended surfaces drawn");
    }

    // Decals (sub-project 12, step 3): boxes over the G-buffer's albedo and normal, after gbuffer.late.
    // They sample the bindless textures; without them no decal is drawn.
    if (m_textureManager) {
        m_decals = std::make_unique<DecalPass>();
        if (!m_decals->initialize(device, allocator, textures) ||
            !m_decals->resize(m_gbufferPass->albedo(), m_gbufferPass->normal(), m_gbufferPass->position(),
                              m_gbufferPass->depth())) {
            OHAO_LOG(Warn, render, "DeferredRenderer: DecalPass failed (no decals drawn)");
            m_decals.reset();
        }
    }

    OHAO_LOG(Info, render, "DeferredRenderer: fully initialized");
    return true;
}

void DeferredRenderer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_vfx) {
        m_vfx->cleanup();
        m_vfx.reset();
    }
    if (m_decals) {
        m_decals->cleanup();
        m_decals.reset();
    }

    if (m_gizmoPass) {
        m_gizmoPass->cleanup();
        m_gizmoPass.reset();
    }
    if (m_hudPass) {
        m_hudPass->cleanup();
        m_hudPass.reset();
    }
    if (m_postProcessing) {
        m_postProcessing->cleanup();
        m_postProcessing.reset();
    }
    if (m_lightingPass) {
        m_lightingPass->cleanup();
        m_lightingPass.reset();
    }
    if (m_csmPass) {
        m_csmPass->cleanup();
        m_csmPass.reset();
    }
    if (m_culling) {
        m_culling->cleanup();
        m_culling.reset();
    }
    if (m_lightClusters) {
        m_lightClusters->cleanup();
        m_lightClusters.reset();
    }
    m_alphaTables.destroy();
    m_alphaTablesReady = false;
    if (m_gbufferPass) {
        m_gbufferPass->cleanup();
        m_gbufferPass.reset();
    }
}

void DeferredRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex) {
    // What every pass records into; each is timed by its registry name (render/frame/pass_registry.hpp).
    const render::Frame frame{cmd, m_passTimer, frameIndex};

    // TAA's jitter: the Halton sample of this frame's index since the last temporal reset (not
    // the frame-in-flight slot, which cycled 3 of the 16 samples and made a capture depend on
    // the slot it started in).
    glm::vec2 jitter = m_postProcessing ? m_postProcessing->getJitterOffset(m_framesSinceReset) : glm::vec2(0.0f);
    if (m_postProcessing) m_postProcessing->setFrameJitter(jitter);
    glm::mat4 jitteredProj = m_proj;
    jitteredProj[2][0] += jitter.x;
    jitteredProj[2][1] += jitter.y;
    if (!m_prevViewProjValid) {   // the first frame: no motion into it
        m_prevViewProj = m_proj * m_view;
        m_prevViewProjValid = true;
    }

    // Update G-Buffer pass
    if (m_gbufferPass) {
        m_gbufferPass->setRenderScene(m_renderScene);
        m_gbufferPass->setViewProjection(m_view, jitteredProj, m_prevViewProj);
    }

    // Update CSM pass
    if (m_csmPass) {
        m_csmPass->setRenderScene(m_renderScene);
        m_csmPass->setLightDirection(m_lightDirection);
        m_csmPass->setCameraData(m_view, m_proj, m_nearPlane, m_farPlane);
    }

    // Update lighting pass
    // This slot's light table (its fence was waited on): unchanged lights are not rewritten.
    if (m_lightClusters) m_lightClusters->setLights(frameIndex, m_lights, m_lightsVersion);
    if (m_lightingPass) {
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_lightingPass->setCameraData(m_cameraPos, m_view, invViewProj);
        m_lightingPass->setMaterials(m_renderScene ? &m_renderScene->buffers().materials : nullptr);
        m_lightingPass->setLights(m_lightClusters ? &m_lightClusters->lightBuffer(frameIndex) : nullptr,
                                  m_lightClusters ? m_lightClusters->lightCount(frameIndex) : 0u);
        // RT direct light and GI reach lighting only from a frame that traced them: their passes
        // below hand them over after a successful run. Until then lighting loops over the lights
        // (CSM for the sun) and has no indirect light.
        m_lightingPass->setDirectLight(nullptr, nullptr);
        m_lightingPass->setSSGITexture(nullptr, nullptr, VK_NULL_HANDLE);
    }

    // The RenderScene's TLAS, built or refit by recordUpdates earlier in this command
    // buffer. Null without RT or before anything was built.
    const VkAccelerationStructureKHR sceneTlas = m_renderScene ? m_renderScene->tlas() : VK_NULL_HANDLE;
    // The RT traces' alpha tables for this slot (its fence was waited on), and the textures.
    const bool alphaSets = m_alphaTablesReady && m_renderScene && m_alphaTables.update(frameIndex, *m_renderScene, m_smokeInGi);
    const VkDescriptorSet texturesSet = m_textureManager ? m_textureManager->getDescriptorSet() : VK_NULL_HANDLE;
    // RT direct light replaces the lighting loop's lights and CSM whenever it runs this frame (a
    // TLAS, and lights or emissive triangles to sample); CSM is the fallback's.
    const bool rtDirect = m_rtDirectWanted && m_rtDirect && m_emissiveLights && sceneTlas != VK_NULL_HANDLE &&
                          m_gbufferPass && alphaSets && texturesSet != VK_NULL_HANDLE &&
                          (m_renderScene->rtLightCount() > 0 || !m_renderScene->emissiveInstances().empty());

    // The frame's culling state (the draw table bound, a stale history cleared): CSM culls its
    // cascades and the G-buffer its two phases with it.
    if (m_culling && m_renderScene) (void)m_culling->beginFrame(frame, *m_renderScene);

    // 1. csm: the cascades' culls and draws (the fallback's shadow map).
    if (m_csmPass && !rtDirect) m_csmPass->record(frame);

    // 2. The G-buffer, GPU-culled, in the five parts it times (cull.instances, gbuffer.early,
    //    hiz.build, cull.late, gbuffer.late).
    if (m_gbufferPass) m_gbufferPass->record(frame);

    // 2.4. decals — the frame's decal boxes projected onto the G-buffer's albedo and normal, over the
    //      surfaces inside them (sub-project 12, step 3). After gbuffer.late: the clusters and the
    //      lighting read the targets it leaves. The records are this frame's (RenderScene::vfx()).
    if (m_decals && m_decalsDrawn && m_renderScene && m_gbufferPass) {
        m_decals->prepare(frameIndex, m_renderScene->vfx());
        m_decals->record(frame, {jitteredProj * m_view, m_cameraPos, 0.6f}, texturesSet);   // the gun's reach: 0.6 m
    }

    // 2.5. lights.cluster — every froxel's light list, for the camera lighting shades with (the
    //      jittered projection the G-buffer used, so a pixel's froxel holds its surface point).
    //      Without clusters (a test switch, or an orthographic projection: froxels are view
    //      frusta slices) lighting sums every light and nothing is built.
    const bool perspective = m_proj[2][3] != 0.0f;
    const bool clustered = m_clusteredLighting && perspective && m_lightClusters &&
                           m_lightClusters->clusterBuffer().buffer != VK_NULL_HANDLE;
    if (clustered) m_lightClusters->build(frame, m_view, jitteredProj, m_width, m_height, m_clusterFar, m_frameNumber);
    if (m_lightingPass) {
        m_lightingPass->setClusters(clustered ? &m_lightClusters->clusterBuffer() : nullptr,
                                    m_lightClusters ? m_lightClusters->sliceScale() : 0.0f,
                                    m_lightClusters ? m_lightClusters->sliceBias() : 0.0f);
    }

    // This frame's index since the last temporal reset: with the render seed, it seeds
    // every RT sample (and RT GI's first frame takes no history).
    const uint32_t rtFrame = m_framesSinceReset++;

    // 3.7. RT direct light: the emissive triangle table (shared with the path tracer, recorded by
    // the frame that draws), then ReSTIR DI's passes. Lighting reads their output and leaves the
    // lights they sample out of its loop.
    const auto directToLighting = [&]() {
        if (m_lightingPass && m_rtDirect->rendered())
            m_lightingPass->setDirectLight(&m_rtDirect->diffuse(), &m_rtDirect->specular());
    };
    if (rtDirect) {
        {
            render::Timed timed(frame, "emissive.lights");
            m_emissiveLights->prepare(frameIndex, *m_renderScene);
            m_emissiveLights->record(frame, *m_renderScene, texturesSet);
        }
        {
            render::Timed timed(frame, "di.sample");
            DirectInput di{};
            di.positionBuffer = &m_gbufferPass->position();
            di.normalBuffer = &m_gbufferPass->normal();
            di.albedoBuffer = &m_gbufferPass->albedo();
            di.specularBuffer = &m_gbufferPass->specular();
            di.openpbrBuffer = &m_gbufferPass->openpbr();
            di.layersBuffer = &m_gbufferPass->layers();
            di.velocityBuffer = &m_gbufferPass->velocity();
            di.width = m_width;
            di.height = m_height;
            di.frameSlot = frameIndex;
            di.frameIndex = rtFrame;
            di.renderSeed = m_renderSeed;
            di.quality = m_quality;
            di.candidates = m_directCandidates;
            di.spatial = m_directSpatial;
            di.halfResolution = !m_directFull;
            di.cameraPos = m_cameraPos;
            di.weather = m_lightingPass ? m_lightingPass->weather() : glm::vec3(0.0f);
            di.scene = m_renderScene;
            di.alphaTables = &m_alphaTables;
            di.textures = texturesSet;
            di.layers = m_renderScene->anyLayeredMaterial();
            di.emissive = m_emissiveLights;
            m_rtDirect->sample(frame, di);
        }
        {
            render::Timed timed(frame, "di.trace");
            m_rtDirect->trace(frame);
        }
        {
            render::Timed timed(frame, "di.shade");
            m_rtDirect->shade(frame);
            if (!m_rtDirect->denoises()) directToLighting();   // reference: the mean is the output
        }
        if (m_quality == RenderQuality::Realtime) {
            render::Timed timed(frame, "di.denoise");
            m_rtDirect->denoise(frame);
            directToLighting();
        }
    }

    // 3.8. RT GI — one-bounce diffuse indirect light: the trace, then (realtime) the
    // denoiser. Lighting reads it at binding 11.
    const auto giToLighting = [&]() {
        if (m_lightingPass && m_rtGI->rendered())
            m_lightingPass->setSSGITexture(m_rtGI->getOutput().indirectLight, m_rtGI->getOutput().reflection,
                                           VK_NULL_HANDLE);
    };
    const bool giTraced = m_useRTGI && m_rtgiWanted && m_rtGI && sceneTlas != VK_NULL_HANDLE && m_gbufferPass;
    if (giTraced) {
        {
            render::Timed timed(frame, "rt.gi");
            GIInput gi{};
            gi.positionBuffer = &m_gbufferPass->position();
            gi.normalBuffer = &m_gbufferPass->normal();
            gi.albedoBuffer = &m_gbufferPass->albedo();
            gi.layersBuffer = &m_gbufferPass->layers();
            gi.velocityBuffer = &m_gbufferPass->velocity();
            gi.cameraPos = m_cameraPos;
            gi.width = m_width;
            gi.height = m_height;
            gi.frameIndex = rtFrame;
            gi.renderSeed = m_renderSeed;
            gi.frameSlot = frameIndex;
            gi.quality = m_quality;
            gi.scene = m_renderScene;
            gi.alphaTables = alphaSets ? &m_alphaTables : nullptr;
            gi.textures = texturesSet;
            gi.lights = m_renderScene->lights();
            gi.envMap = m_envMap;
            gi.envIntensity = m_envIntensity;
            gi.emissionIsDirect = rtDirect;
            m_rtGI->render(frame, gi);
            if (!m_rtGI->denoises()) giToLighting();   // reference: the trace is the output
        }
        if (m_quality == RenderQuality::Realtime) {
            render::Timed timed(frame, "rt.gi.denoise");
            m_rtGI->denoise(frame);
            giToLighting();
        }
    }

    // 4. Deferred lighting: the G-buffer, the shadow map and the traced light into the lit image.
    if (m_lightingPass && m_gbufferPass) m_lightingPass->record(frame);

    // 4.6. vfx.expand and vfx.sort (sub-project 12): the effect records' particles in closed
    //      form, lit at their centres by this frame's clusters, sun ray and RT GI, then the
    //      alpha-blended batches back to front. The transparent pass draws them after SSR.
    const bool vfxOn = m_vfx && m_renderScene && m_lightingPass && m_gbufferPass;
    VfxRenderer::Lighting vfxLighting{};
    if (vfxOn) {
        m_vfx->prepare(frameIndex, *m_renderScene, {m_view, jitteredProj, m_cameraPos, m_nearPlane});
        vfxLighting.lights = m_lightClusters ? &m_lightClusters->lightBuffer(frameIndex) : nullptr;
        vfxLighting.lightCount = m_lightClusters ? m_lightClusters->lightCount(frameIndex) : 0u;
        vfxLighting.clusters = clustered ? &m_lightClusters->clusterBuffer() : nullptr;
        vfxLighting.sliceScale = m_lightClusters ? m_lightClusters->sliceScale() : 0.0f;
        vfxLighting.sliceBias = m_lightClusters ? m_lightClusters->sliceBias() : 0.0f;
        vfxLighting.tlas = sceneTlas;
        vfxLighting.giRadiance = giTraced && m_rtGI->rendered() ? m_rtGI->getOutput().indirectLight : nullptr;
        vfxLighting.envIrradiance = m_envIrradiance;
        vfxLighting.envPrefiltered = m_envPrefiltered;
        vfxLighting.envIntensity = m_envIntensity;
        if (m_vfx->hasParticles()) {
            {
                render::Timed timed(frame, "vfx.expand");
                m_vfx->expand(frame, *m_renderScene, vfxLighting);
            }
            render::Timed timed(frame, "vfx.sort");
            m_vfx->sort(frame);
        }
    }

    // 4.7. SSS — separable subsurface scattering blur, in place on the lit image, over the tiles the G-buffer pass flagged for subsurface materials
    if (m_sssPass && m_sssWanted && m_renderScene && m_lightingPass && m_lightingPass->lit()) {
        render::Timed timed(frame, "sss");
        m_sssPass->setLitImage(&m_lightingPass->lit());
        m_sssPass->setSubsurfaceDiffuse(&m_lightingPass->subsurfaceDiffuse());
        m_sssPass->setMaterials(&m_renderScene->buffers().materials);
        m_sssPass->setCameraPos(m_cameraPos);
        m_sssPass->record(frame);
    }

    // 4.9. transparent — the blended surfaces and the effects over the lit image, back to front,
    // each with its own blend state, against the G-buffer's depth (sub-project 12); their
    // coverage goes to a mask TAA lowers its history weight under. Before bloom, so it glows.
    if (vfxOn && m_vfx->hasObjects()) {
        render::Timed timed(frame, "transparent");
        m_vfx->drawTransparent(frame, *m_renderScene, vfxLighting, texturesSet, *m_gbufferPass);
    }
    if (m_postProcessing) m_postProcessing->setTransparencyMask(vfxOn ? m_vfx->transparencyMask() : nullptr);

    // 5. Post-processing (bloom, TAA, tonemapping): it times its own sub-passes.
    if (m_postProcessing && m_lightingPass) {
        // The lit image, SSS applied in place
        m_postProcessing->setHDRInput(&m_lightingPass->lit());
        m_postProcessing->setDeltaTime(m_deltaTime);
        m_postProcessing->record(frame);
    }
    gpu::Image* const finalImage = finalOutput();

    // 6. Gizmo pass (transform handles for selected objects, and the frame's lines)
    if (m_gizmoPass && (m_gizmoEnabled || m_gizmoPass->hasLines()) && finalImage) {
        m_gizmoPass->setViewProjection(m_proj * m_view);
        m_gizmoPass->record(frame, *finalImage);
    }

    // 7. The HUD, over the final RGBA8 image (a frame without a list records nothing).
    if (m_hudPass && m_hudPass->hasItems() && finalImage) m_hudPass->record(frame, *finalImage);

    // Store current view-proj for next frame's motion vectors
    m_prevViewProj = m_proj * m_view;   // unjittered: next frame's velocity carries its own jitter
}

void DeferredRenderer::setRenderScale(float scale) {
    scale = std::clamp(scale, kMinRenderScale, 1.0f);
    if (scale == m_renderScale) return;
    m_renderScale = scale;
    if (m_outWidth == 0) return;
    vkDeviceWaitIdle(m_device);   // frames in flight read the targets and descriptor sets a resize replaces
    onResize(m_outWidth, m_outHeight);
    resetTemporalState();
}

void DeferredRenderer::onResize(uint32_t outWidth, uint32_t outHeight) {
    // The scene is rendered at the output's size times the render scale, and TAA resolves it to the output's.
    const uint32_t width = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(outWidth) * m_renderScale)));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(outHeight) * m_renderScale)));
    if (width == m_width && height == m_height && outWidth == m_outWidth && outHeight == m_outHeight) return;

    m_outWidth = outWidth;
    m_outHeight = outHeight;
    m_width = width;
    m_height = height;

    if (m_gbufferPass) m_gbufferPass->onResize(width, height);
    // The Hi-Z pyramid follows the new depth buffer (the G-buffer resize idled the device).
    if (m_culling && m_gbufferPass) m_culling->resize(width, height, m_gbufferPass->depth());
    // Every screen-sized target follows the extent. A pass left at its creation size
    // is invisible to validation: the RT shadow raygen stores at gl_LaunchID into the
    // wrong texels (or past the image when the window grows) and lighting samples by UV.
    if (m_rtDirect) m_rtDirect->resize(width, height);
    if (m_rtGI) m_rtGI->resize(width, height);
    if (m_sssPass) m_sssPass->onResize(width, height);
    if (m_lightingPass) m_lightingPass->onResize(width, height);
    if (m_postProcessing) {
        m_postProcessing->setRenderSize(width, height);
        m_postProcessing->onResize(outWidth, outHeight);
    }

    // The transparent pass draws into the new lit image against the new depth.
    if (m_vfx && m_lightingPass && m_gbufferPass &&
        !m_vfx->resize(width, height, m_lightingPass->lit(), m_gbufferPass->depth()))
        OHAO_LOG(Error, render, "DeferredRenderer: the transparent pass's targets failed at {}x{}", width, height);
    if (m_decals && m_gbufferPass &&
        !m_decals->resize(m_gbufferPass->albedo(), m_gbufferPass->normal(), m_gbufferPass->position(),
                          m_gbufferPass->depth()))
        OHAO_LOG(Error, render, "DeferredRenderer: the decals' targets failed at {}x{}", width, height);

    // Reconnect after resize
    if (m_lightingPass && m_gbufferPass) {
        // RT GI and RT direct light may have been recreated above;
        // lighting still holds their old views. Drop them before the rewrite --
        // render() re-supplies them before the lighting pass executes.
        m_lightingPass->clearPerFrameInputs();
        m_lightingPass->setGBufferPass(m_gbufferPass.get());
    }
}

void DeferredRenderer::resetTemporalState() {
    m_framesSinceReset = 0;
    m_prevViewProjValid = false;   // the next frame uses its own view-projection: no motion into it
    if (m_rtDirect) m_rtDirect->resetHistory();
    if (m_rtGI) m_rtGI->resetHistory();
    if (m_postProcessing) m_postProcessing->resetHistory();
    if (m_culling) m_culling->resetHistory();   // the first frame draws in its late pass
}

void DeferredRenderer::setTextureManager(BindlessTextureManager* texManager) {
    m_textureManager = texManager;
    if (m_gbufferPass) {
        m_gbufferPass->setTextureManager(texManager);
    }
}

void DeferredRenderer::setEnvMap(gpu::Image* source, gpu::Image* prefiltered, gpu::Image* irradiance,
                                 VkSampler iblSampler, float intensity) {
    m_envMap = source;   // RT GI's misses read the environment itself
    m_envIntensity = intensity;
    m_envPrefiltered = prefiltered;   // the transparent pass lights blended surfaces as lighting does
    m_envIrradiance = irradiance;
    if (m_lightingPass) m_lightingPass->setEnvironment(prefiltered, irradiance, iblSampler, intensity);
}

void DeferredRenderer::setRenderScene(const render::RenderScene* scene) {
    m_renderScene = scene;
}

void DeferredRenderer::setCameraData(const glm::mat4& view, const glm::mat4& proj,
                                      const glm::vec3& position, float nearPlane, float farPlane) {
    m_view = view;
    m_proj = proj;
    // Flip Y for Vulkan NDC (Y-down) — GLM perspective assumes OpenGL (Y-up)
    m_proj[1][1] *= -1;
    m_cameraPos = position;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
}

void DeferredRenderer::setLights(std::span<const LightData> lights, uint64_t version) {
    m_lights = lights;
    m_lightsVersion = version;
    for (const LightData& l : lights) {
        if (l.position.w != 0.0f || glm::dot(glm::vec3(l.direction), glm::vec3(l.direction)) == 0.0f) continue;
        setSunDirection(-glm::vec3(l.direction));   // direction is where the light travels
        break;
    }
}

gpu::Image* DeferredRenderer::finalOutput() {
    return m_postProcessing && m_postProcessing->didExecute() ? &m_postProcessing->output() : nullptr;
}

gpu::Image* DeferredRenderer::litImage() {
    return m_lightingPass ? &m_lightingPass->lit() : nullptr;
}

glm::vec2 DeferredRenderer::getJitterOffset(uint32_t frameIndex) const {
    if (m_postProcessing) {
        return m_postProcessing->getJitterOffset(frameIndex);
    }
    return glm::vec2(0.0f);
}

void DeferredRenderer::setSunDirection(const glm::vec3& dir) {
    m_lightDirection = -glm::normalize(dir);   // where the CSM fallback's sun light travels
}

void DeferredRenderer::setWireframeEnabled(bool enabled) {
    m_wireframeEnabled = enabled;
    if (m_gbufferPass) {
        m_gbufferPass->setWireframeEnabled(enabled);
    }
}

void DeferredRenderer::setLines(std::span<const render::DebugLine> lines) {
    if (m_gizmoPass) m_gizmoPass->setLines(lines);
}

void DeferredRenderer::setHud(std::span<const render::HudItem> items) {
    if (m_hudPass) m_hudPass->setItems(items);
}

void DeferredRenderer::setGizmoEnabled(bool enabled) {
    m_gizmoEnabled = enabled;
    if (m_gizmoPass) {
        m_gizmoPass->setEnabled(enabled);
    }
}

void DeferredRenderer::setGizmoMode(GizmoMode mode) {
    if (m_gizmoPass) {
        m_gizmoPass->setGizmoMode(mode);
    }
}

void DeferredRenderer::setGizmoTransform(const glm::mat4& model) {
    if (m_gizmoPass) {
        m_gizmoPass->setGizmoTransform(model);
    }
}

void DeferredRenderer::setGizmoHighlightedAxis(GizmoAxis axis) {
    if (m_gizmoPass) {
        m_gizmoPass->setHighlightedAxis(axis);
    }
}

void DeferredRenderer::setRTGIRealtime(bool fullResolution, uint32_t rays) {
    if (m_rtGI) m_rtGI->setRealtime(fullResolution, rays);
    m_giFull = fullResolution;
    m_giRays = std::clamp(rays, 1u, 32u);
}

void DeferredRenderer::configureCSM(uint32_t cascades, uint32_t size) {
    if (m_csmPass) (void)m_csmPass->configure(cascades, size);   // lighting samples whichever map the pass holds
}

} // namespace ohao
