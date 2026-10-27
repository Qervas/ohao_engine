#include "deferred_renderer.hpp"
#include "gpu_culling.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "render/frame/gpu_pass_timer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "core/log.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace ohao {

DeferredRenderer::~DeferredRenderer() {
    cleanup();
}

bool DeferredRenderer::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    OHAO_LOG(Debug, render, "DeferredRenderer: initializing...");

    // Initialize G-Buffer pass
    m_gbufferPass = std::make_unique<GBufferPass>();
    if (m_textureManager) {
        m_gbufferPass->setTextureManager(m_textureManager);
    }
    m_gbufferPass->setMeshShaderSupported(m_meshShader);
    if (!m_gbufferPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: GBufferPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: GBufferPass OK");

    // Initialize CSM pass
    m_csmPass = std::make_unique<CSMPass>();
    if (!m_csmPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: CSMPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: CSMPass OK");

    // GPU culling: the G-buffer's and CSM's indirect draw lists
    m_culling = std::make_unique<GpuCulling>();
    if (!m_culling->initialize(device, physicalDevice, m_meshShader) ||
        !m_culling->resize(m_width, m_height, m_gbufferPass->getDepthView())) {
        OHAO_LOG(Error, render, "DeferredRenderer: GpuCulling failed");
        return false;
    }
    m_gbufferPass->setCulling(m_culling.get());
    m_csmPass->setCulling(m_culling.get());

    // Clustered lighting: the light table and the froxel lists
    m_lightClusters = std::make_unique<LightClusters>();
    if (!m_lightClusters->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: LightClusters failed");
        return false;
    }

    // Initialize deferred lighting pass
    m_lightingPass = std::make_unique<DeferredLightingPass>();
    if (!m_lightingPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: LightingPass failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: LightingPass OK");

    // Connect G-Buffer to lighting
    m_lightingPass->setGBufferPass(m_gbufferPass.get());

    // Connect shadow map and cascade data to lighting
    m_lightingPass->setShadowMap(m_csmPass->getShadowMapArrayView(),
                                  m_csmPass->getShadowSampler());
    m_lightingPass->setCascadeBuffer(m_csmPass->getCascadeBuffer());

    // Initialize SSS pass (separable subsurface scattering blur)
    m_sssPass = std::make_unique<SSSPass>();
    if (m_sssPass->initialize(device, physicalDevice)) {
        m_sssPass->setGBufferPass(m_gbufferPass.get());
        OHAO_LOG(Debug, render, "DeferredRenderer: SSSPass OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: SSSPass failed (non-fatal)");
        m_sssPass.reset();
    }

    // Initialize SSR pass
    m_ssrPass = std::make_unique<SSRPass>();
    if (m_ssrPass->initialize(device, physicalDevice)) {
        m_ssrPass->setGBufferPass(m_gbufferPass.get());
        OHAO_LOG(Debug, render, "DeferredRenderer: SSRPass OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: SSRPass failed (non-fatal)");
        m_ssrPass.reset();
    }

    // Initialize post-processing pipeline
    m_postProcessing = std::make_unique<PostProcessingPipeline>();
    if (!m_postProcessing->initialize(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredRenderer: PostProcessing failed");
        return false;
    }
    OHAO_LOG(Debug, render, "DeferredRenderer: PostProcessing OK");

    // Initialize gizmo pass (transform handles)
    m_gizmoPass = std::make_unique<GizmoPass>();
    if (!m_gizmoPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: GizmoPass failed (non-fatal)");
        m_gizmoPass.reset();
    } else {
        OHAO_LOG(Debug, render, "DeferredRenderer: GizmoPass OK");
    }

    // The HUD over the final image (sub-project 7): drawn only on frames with a list.
    m_hudPass = std::make_unique<HudPass>();
    if (!m_hudPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: HudPass failed (non-fatal: no HUD)");
        m_hudPass.reset();
    }

    // Connect depth/normal/velocity/albedo/position for post-processing
    m_postProcessing->setDepthBuffer(m_gbufferPass->getDepthView());
    m_postProcessing->setNormalBuffer(m_gbufferPass->getNormalView());
    m_postProcessing->setVelocityBuffer(m_gbufferPass->getVelocityView());

    // Initialize particle system
    m_particleSystem = std::make_unique<ParticleSystem>();
    if (!m_particleSystem->initialize(device, physicalDevice, 65536)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: ParticleSystem failed (non-fatal)");
        m_particleSystem.reset();
    } else {
        // Create render pass, framebuffer, and render pipeline for particle rendering
        if (createParticleRenderPass() && createParticleFramebuffer()) {
            if (m_particleSystem->initRenderPipeline(m_particleRenderPass)) {
                OHAO_LOG(Debug, render, "DeferredRenderer: ParticleSystem OK");
            } else {
                OHAO_LOG(Warn, render, "DeferredRenderer: Particle render pipeline failed (non-fatal)");
            }
        } else {
            OHAO_LOG(Warn, render, "DeferredRenderer: Particle render pass/framebuffer failed (non-fatal)");
        }
    }

    // Initialize sky pass (Preetham analytical sky)
    m_skyPass = std::make_unique<SkyPass>();
    if (!m_skyPass->initialize(device, physicalDevice)) {
        OHAO_LOG(Warn, render, "DeferredRenderer: SkyPass failed (non-fatal)");
        m_skyPass.reset();
    } else {
        m_skyPass->setHDROutput(m_lightingPass->getOutputView(),
                                m_lightingPass->getOutputImage());
        m_skyPass->setDepthBuffer(m_gbufferPass->getDepthView());
        OHAO_LOG(Debug, render, "DeferredRenderer: SkyPass OK");
    }

    // Initialize render graph and import all pass outputs
    importGraphTextures();

    // Initialize RT shadow technique (inline ray queries)
    m_rtShadow = std::make_unique<RTShadowTechnique>();
    if (m_rayQuery && m_rtShadow->init(device, physicalDevice, m_width, m_height)) {
        m_useRTShadows = true;
        OHAO_LOG(Debug, render, "DeferredRenderer: RTShadow OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: RTShadow not available (using CSM fallback)");
        m_rtShadow.reset();
    }

    // Initialize RT GI technique (inline ray queries)
    m_rtGI = std::make_unique<RTGITechnique>();
    if (m_rayQuery && m_rtGI->init(device, physicalDevice, m_width, m_height)) {
        m_useRTGI = true;
        OHAO_LOG(Debug, render, "DeferredRenderer: RTGI OK");
    } else {
        OHAO_LOG(Warn, render, "DeferredRenderer: RTGI not available");
        m_rtGI.reset();
    }

    OHAO_LOG(Info, render, "DeferredRenderer: fully initialized");
    return true;
}

void DeferredRenderer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    m_renderGraph.shutdown();

    if (m_particleSystem) {
        m_particleSystem->cleanup();
        m_particleSystem.reset();
    }
    if (m_particleFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_particleFramebuffer, nullptr);
        m_particleFramebuffer = VK_NULL_HANDLE;
    }
    if (m_particleRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_particleRenderPass, nullptr);
        m_particleRenderPass = VK_NULL_HANDLE;
    }

    if (m_skyPass) {
        m_skyPass->cleanup();
        m_skyPass.reset();
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
    if (m_gbufferPass) {
        m_gbufferPass->cleanup();
        m_gbufferPass.reset();
    }
}

void DeferredRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex) {
    // TAA's jitter: the Halton sample of this frame's index since the last temporal reset (not
    // the frame-in-flight slot, which cycled 3 of the 16 samples and made a capture depend on
    // the slot it started in).
    glm::vec2 jitter = m_postProcessing ? m_postProcessing->getJitterOffset(m_framesSinceReset) : glm::vec2(0.0f);
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
        m_lightingPass->setLights(m_lightClusters ? m_lightClusters->lightBuffer(frameIndex) : VK_NULL_HANDLE,
                                  m_lightClusters ? m_lightClusters->lightCount(frameIndex) : 0u, m_ambient);

        // SSAO texture is set after executeSSAO() call below
    }

    // SSAO's inputs: the G-buffer and the unjittered camera
    if (m_postProcessing && m_gbufferPass) {
        m_postProcessing->setSSAOInputs(m_gbufferPass->getPositionView(), m_gbufferPass->getNormalView(),
                                        m_proj * m_view, m_cameraPos);
    }

    // --- Render Graph: barrier-tracked passes (CSM → GBuffer → SSAO → Lighting) ---
    // The graph computes inter-pass image layout barriers centrally.
    // Each pass still manages its own VkRenderPass/VkFramebuffer internally.

    m_renderGraph.reset();

    // The RenderScene's TLAS, built or refit by recordUpdates earlier in this command
    // buffer. Null without RT or before anything was built.
    const VkAccelerationStructureKHR sceneTlas = m_renderScene ? m_renderScene->tlas() : VK_NULL_HANDLE;
    // RT shadows replace CSM whenever they trace this frame; CSM is the fallback.
    const bool rtShadows = m_useRTShadows && m_rtShadow && sceneTlas != VK_NULL_HANDLE && m_gbufferPass &&
                           !m_shadowLights.empty();

    // 1. Shadow pass (CSM) — writes shadow map, leaves at DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    if (m_csmPass && !rtShadows) {
        m_renderGraph.addPass("csm",
            [&](PassBuilder& builder) {
                if (m_graphShadowHandle.isValid())
                    builder.declareDepthWrite(m_graphShadowHandle,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            },
            [&](VkCommandBuffer c) { m_csmPass->execute(c, frameIndex); });
    }

    // 2. G-Buffer pass — GPU-culled, in five parts it times itself (cull.instances,
    //    gbuffer.early, hiz.build, cull.late, gbuffer.late); its late render pass's finalLayouts
    //    are: color buffers → SHADER_READ_ONLY_OPTIMAL, depth → DEPTH_STENCIL_READ_ONLY_OPTIMAL
    if (m_gbufferPass) {
        m_gbufferPass->setPassTimer(m_passTimer);
        m_renderGraph.addPass("gbuffer",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.declareColorWrite(m_graphNormalHandle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (m_graphAlbedoHandle.isValid())
                    builder.declareColorWrite(m_graphAlbedoHandle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (m_graphDepthHandle.isValid())
                    builder.declareDepthWrite(m_graphDepthHandle,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            },
            [&](VkCommandBuffer c) { m_gbufferPass->execute(c, frameIndex); }, /*selfTimed=*/true);
    }

    // 2.5. lights.cluster — every froxel's light list, for the camera lighting shades with (the
    //      jittered projection the G-buffer used, so a pixel's froxel holds its surface point).
    //      Without clusters (a test switch, or an orthographic projection: froxels are view
    //      frusta slices) lighting sums every light and nothing is built.
    const bool perspective = m_proj[2][3] != 0.0f;
    const bool clustered = m_clusteredLighting && perspective && m_lightClusters &&
                           m_lightClusters->clusterBuffer() != VK_NULL_HANDLE;
    if (m_lightingPass) {
        m_lightingPass->setClusters(clustered ? m_lightClusters->clusterBuffer() : VK_NULL_HANDLE,
                                    m_lightClusters ? m_lightClusters->sliceScale() : 0.0f,
                                    m_lightClusters ? m_lightClusters->sliceBias() : 0.0f);
    }
    if (clustered) {
        m_renderGraph.addComputePass("lights.cluster", [&](PassBuilder&) {},
            [&, jitteredProj](VkCommandBuffer c) {
                m_lightClusters->build(c, frameIndex, m_view, jitteredProj, m_width, m_height, m_clusterFar, m_frameNumber);
                if (m_lightingPass)
                    m_lightingPass->setClusters(m_lightClusters->clusterBuffer(), m_lightClusters->sliceScale(),
                                                m_lightClusters->sliceBias());
            });
    }

    // 3. SSAO — reads the G-buffer's position and normal (SHADER_READ_ONLY after the
    //    G-buffer pass, whose render pass makes its writes visible to later shaders) and
    //    manages its own images' layouts. Only when the effect is on, so a frame without it
    //    has no ssao pass (or timing); lighting then binds its fallback view.
    const bool ssaoOn = m_postProcessing && m_postProcessing->getSSAOEnabled();
    if (!ssaoOn && m_lightingPass) m_lightingPass->setSSAOTexture(VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (ssaoOn && m_gbufferPass) {
        m_renderGraph.addComputePass("ssao",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readComputeTexture(m_graphNormalHandle);
            },
            [&](VkCommandBuffer c) {
                m_postProcessing->executeSSAO(c, frameIndex);
                if (m_lightingPass) {
                    VkImageView ssaoView = m_postProcessing->getSSAOOutput();
                    VkSampler ssaoSampler = m_postProcessing->getSSAOSampler();
                    if (ssaoView != VK_NULL_HANDLE && ssaoSampler != VK_NULL_HANDLE)
                        m_lightingPass->setSSAOTexture(ssaoView, ssaoSampler);
                }
            });

    }

    // This frame's index since the last temporal reset: with the render seed, it seeds
    // every RT sample (and RT GI's first frame takes no history).
    const uint32_t rtFrame = m_framesSinceReset++;

    // 3.7. RT Shadows — trace the scene's shadow-casting lights (setShadowLights) through the
    // TLAS, then (realtime) filter them, one visibility channel a light. Lighting records
    // after these passes and writes its descriptors then.
    const auto shadowsToLighting = [&]() {
        if (!m_lightingPass || !m_rtShadow->rendered()) return;
        m_lightingPass->setRTShadowMask(m_rtShadow->getOutput().visibility);
    };
    if (rtShadows) {
        m_renderGraph.addComputePass("rt.shadows",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readComputeTexture(m_graphNormalHandle);
            },
            [&](VkCommandBuffer c) {
                ShadowInput si{};
                si.positionBuffer = m_gbufferPass->getPositionView();
                si.normalBuffer = m_gbufferPass->getNormalView();
                si.depthBuffer = m_gbufferPass->getDepthView();
                si.velocityBuffer = m_gbufferPass->getVelocityView();
                si.width = m_width;
                si.height = m_height;
                si.frameSlot = frameIndex;
                si.frameNumber = m_frameNumber;
                si.cameraPos = m_cameraPos;
                si.lights = m_shadowLights;
                si.renderSeed = m_renderSeed;
                si.frameIndex = rtFrame;
                si.quality = m_quality;
                si.maxLightsPerPixel = m_shadowLightsPerPixel;
                si.tlas = sceneTlas;
                m_rtShadow->render(c, si);
                if (!m_rtShadow->filters()) shadowsToLighting();   // reference: the trace is the output
            });
        if (m_quality == RenderQuality::Realtime) {
            m_renderGraph.addComputePass("rt.shadows.filter",
                [&](PassBuilder&) {},
                [&](VkCommandBuffer c) {
                    m_rtShadow->filter(c);
                    shadowsToLighting();
                });
        }
    }

    // 3.8. RT GI — one-bounce diffuse indirect light: the trace, then (realtime) the
    // denoiser. Lighting reads it at binding 11.
    const auto giToLighting = [&]() {
        if (m_lightingPass && m_rtGI->rendered())
            m_lightingPass->setSSGITexture(m_rtGI->getOutput().indirectLightView, VK_NULL_HANDLE);
    };
    if (m_useRTGI && m_rtgiWanted && m_rtGI && sceneTlas != VK_NULL_HANDLE && m_gbufferPass) {
        m_renderGraph.addComputePass("rt.gi",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readComputeTexture(m_graphNormalHandle);
                if (m_graphAlbedoHandle.isValid())
                    builder.readComputeTexture(m_graphAlbedoHandle);
            },
            [&](VkCommandBuffer c) {
                GIInput gi{};
                gi.positionBuffer = m_gbufferPass->getPositionView();
                gi.normalBuffer = m_gbufferPass->getNormalView();
                gi.albedoBuffer = m_gbufferPass->getAlbedoView();
                gi.velocityBuffer = m_gbufferPass->getVelocityView();
                gi.cameraPos = m_cameraPos;
                gi.width = m_width;
                gi.height = m_height;
                gi.frameIndex = rtFrame;
                gi.renderSeed = m_renderSeed;
                gi.frameSlot = frameIndex;
                gi.quality = m_quality;
                gi.tlas = sceneTlas;
                gi.instances = m_renderScene->instanceBuffer();
                gi.lights = m_renderScene->lights();
                gi.envMap = m_envView;
                gi.envIntensity = m_envIntensity;
                m_rtGI->render(c, gi);
                if (!m_rtGI->denoises()) giToLighting();   // reference: the trace is the output
            });
        if (m_quality == RenderQuality::Realtime) {
            m_renderGraph.addComputePass("rt.gi.denoise",
                [&](PassBuilder&) {},
                [&](VkCommandBuffer c) {
                    m_rtGI->denoise(c);
                    giToLighting();
                });
        }
    }

    // 4. Deferred lighting — reads GBuffer + shadow. Key barrier: shadow map
    //    DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL (was missing before).
    if (m_lightingPass && m_gbufferPass) {
        m_renderGraph.addPass("lighting",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readTexture(m_graphNormalHandle,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                if (m_graphAlbedoHandle.isValid())
                    builder.readTexture(m_graphAlbedoHandle,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                if (m_graphShadowHandle.isValid())
                    builder.readTexture(m_graphShadowHandle,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                if (m_graphLightingHandle.isValid())
                    builder.declareColorWrite(m_graphLightingHandle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            },
            [&](VkCommandBuffer c) { m_lightingPass->execute(c, frameIndex); });
    }

    // RT shadow mask and GI reach lighting only from a frame that traced them: the
    // RTShadow / RTGI passes above hand them over after a successful render(). Until
    // then lighting falls back to CSM shadows and no indirect light.
    if (m_lightingPass) {
        m_lightingPass->setRTShadowMask(VK_NULL_HANDLE);
        m_lightingPass->setSSGITexture(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }

    // The frame's culling state (the draw table bound, a stale history cleared), before the
    // graph: CSM culls its cascades and the G-buffer its two phases inside it.
    if (m_culling && m_renderScene) (void)m_culling->beginFrame(cmd, frameIndex, *m_renderScene);

    m_renderGraph.setPassTimer(m_passTimer);   // times each graph pass under its name
    m_renderGraph.compile();
    m_renderGraph.execute(cmd);

    // Direct passes: timed by registry name (render/frame/pass_registry.hpp).
    auto timerBegin = [&](const char* name) {
        if (m_passTimer) m_passTimer->begin(cmd, name);
    };
    auto timerEnd = [&]() {
        if (m_passTimer) m_passTimer->end(cmd);
    };

    // Sky and particles draw into the lighting output before SSS blurs it in place and SSR
    // and post read it.

    // 4.5. Sky pass — fills sky pixels with Preetham sky
    if (m_skyPass && m_skyEnabled) {
        timerBegin("sky");
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_skyPass->setCameraData(invViewProj, m_cameraPos);
        // Sun direction: opposite of the directional light direction (light points down → sun up)
        m_skyPass->setSunDirection(-m_lightDirection);
        m_skyPass->setTurbidity(m_skyTurbidity);
        m_skyPass->setSunIntensity(m_skyIntensity);
        m_skyPass->setGroundColor(m_skyGroundColor);

        // Night state already computed eagerly in setSunDirection()
        m_skyPass->setNightFactor(m_nightFactor);
        m_skyPass->setMoonDirection(m_moonDirection);
        m_skyPass->setStarSeed(m_totalTime);

        m_skyPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.6. Particle system (forward pass over HDR, before SSS and post-processing)
    // Only while a particle can be alive: since the last emit plus its longest lifetime.
    m_totalTime += m_deltaTime;
    const bool particlesLive = !m_pendingEmits.empty() || m_totalTime <= m_particlesLiveUntil;
    const bool particlesRun = m_particleSystem && m_lightingPass && particlesLive && m_particlesDrawn;
    if (particlesRun) {
        timerBegin("particles");

        // Process pending emits
        while (!m_pendingEmits.empty()) {
            m_particleSystem->emit(cmd, m_pendingEmits.front(), m_totalTime);
            m_particlesLiveUntil = std::max(m_particlesLiveUntil, m_totalTime + m_pendingEmits.front().maxLifetime);
            m_pendingEmits.pop();
        }

        // Update particles (compute shader)
        m_particleSystem->update(cmd, m_deltaTime);

        // Begin particle render pass (renders over HDR output with depth test)
        if (m_particleRenderPass != VK_NULL_HANDLE && m_particleFramebuffer != VK_NULL_HANDLE) {
            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass = m_particleRenderPass;
            rpInfo.framebuffer = m_particleFramebuffer;
            rpInfo.renderArea.extent = {m_width > 0 ? m_width : 1920u, m_height > 0 ? m_height : 1080u};

            vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

            // Set dynamic viewport and scissor
            VkViewport viewport{};
            viewport.width = static_cast<float>(rpInfo.renderArea.extent.width);
            viewport.height = static_cast<float>(rpInfo.renderArea.extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.extent = rpInfo.renderArea.extent;
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            // Render particles as billboards
            glm::mat4 viewProj = m_proj * m_view;
            glm::mat4 invView = glm::inverse(m_view);
            glm::vec3 cameraRight = glm::vec3(invView[0]);
            glm::vec3 cameraUp = glm::vec3(invView[1]);
            m_particleSystem->render(cmd, viewProj, cameraRight, cameraUp);

            vkCmdEndRenderPass(cmd);
        }
        timerEnd();  // particles
    }

    // Sky and particles drew into the lit image as a color attachment; SSS (compute), SSR
    // and post sample it. Neither render pass's dependencies reach a compute read.
    if (m_lightingPass && ((m_skyPass && m_skyEnabled) || particlesRun)) {
        VkMemoryBarrier toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &toRead,
                             0, nullptr, 0, nullptr);
    }

    // 4.7. SSS — separable subsurface scattering blur, in place on the lit image (with sky
    // and particles in it), over the tiles the G-buffer pass flagged for subsurface materials
    const VkImageView litOutput = m_lightingPass ? m_lightingPass->getOutputView() : VK_NULL_HANDLE;
    if (m_sssPass && m_sssWanted && litOutput) {
        timerBegin("sss");
        m_sssPass->setLitImage(m_lightingPass->getOutputImage(), litOutput);
        m_sssPass->setCameraPos(m_cameraPos);
        m_sssPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.8. SSR — screen-space reflections on glossy surfaces: traced at half resolution, then
    // resolved into the lit image in place, replacing the environment's reflection where the
    // trace hit (before bloom and TAA, which see the reflections like any lit pixel).
    if (m_ssrPass && m_ssrWanted && litOutput) {
        timerBegin("ssr");
        m_ssrPass->setCameraData(m_proj * m_view, m_cameraPos);
        m_ssrPass->setLitScene(m_lightingPass->getOutputImage(), litOutput, m_lightingPass->envSpecularView(),
                               m_lightingPass->specularWeightView());
        m_ssrPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 5. Post-processing (bloom, TAA, tonemapping): it times its own sub-passes.
    if (m_postProcessing && m_lightingPass) {
        // The lit image, SSS applied in place
        m_postProcessing->setHDRInputWithImage(m_lightingPass->getOutputView(), m_lightingPass->getOutputImage());
        m_postProcessing->setDeltaTime(m_deltaTime);
        m_postProcessing->setPassTimer(m_passTimer);
        m_postProcessing->execute(cmd, frameIndex);
    }

    // 6. Gizmo pass (transform handles for selected objects)
    if (m_gizmoPass && m_gizmoEnabled) {
        timerBegin("gizmo");
        // Determine the final output image — must be from a pass that actually ran
        VkImage finalImage = VK_NULL_HANDLE;
        VkImageView finalView = VK_NULL_HANDLE;

        if (m_postProcessing && m_postProcessing->didExecute()) {
            finalImage = m_postProcessing->getOutputImage();
            finalView = m_postProcessing->getOutputView();
        } else if (m_lightingPass) {
            finalImage = m_lightingPass->getOutputImage();
            finalView = m_lightingPass->getOutputView();
        }

        if (finalImage != VK_NULL_HANDLE && finalView != VK_NULL_HANDLE) {
            m_gizmoPass->setTargetImage(finalImage, finalView);
            m_gizmoPass->setViewProjection(m_proj * m_view);
            m_gizmoPass->execute(cmd, frameIndex);
        }
        timerEnd();  // gizmo
    }

    // 7. The HUD, over the final RGBA8 image (a frame without a list records nothing).
    if (m_hudPass && m_hudPass->hasItems() && m_postProcessing && m_postProcessing->didExecute()) {
        timerBegin("hud");
        m_hudPass->setTarget(m_postProcessing->getOutputImage(), m_postProcessing->getOutputView(), m_width, m_height);
        m_hudPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // Store current view-proj for next frame's motion vectors
    m_prevViewProj = m_proj * m_view;   // unjittered: next frame's velocity carries its own jitter
}

void DeferredRenderer::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    if (m_gbufferPass) m_gbufferPass->onResize(width, height);
    // The Hi-Z pyramid follows the new depth buffer (the G-buffer resize idled the device).
    if (m_culling && m_gbufferPass) m_culling->resize(width, height, m_gbufferPass->getDepthView());
    // The G-buffer's views were just destroyed and recreated. Hand the new ones to
    // post-processing NOW, before its own onResize rewrites descriptor sets: any
    // rewrite that still holds an old view writes a destroyed handle.
    if (m_postProcessing && m_gbufferPass) {
        m_postProcessing->setGBufferInputs(m_gbufferPass->getDepthView(),
                                           m_gbufferPass->getNormalView(),
                                           m_gbufferPass->getVelocityView());
    }
    // Every screen-sized target follows the extent. A pass left at its creation size
    // is invisible to validation: the RT shadow raygen stores at gl_LaunchID into the
    // wrong texels (or past the image when the window grows) and lighting samples by UV.
    if (m_rtShadow) m_rtShadow->resize(width, height);
    if (m_rtGI) m_rtGI->resize(width, height);
    if (m_sssPass) m_sssPass->onResize(width, height);
    if (m_ssrPass) m_ssrPass->onResize(width, height);
    if (m_lightingPass) m_lightingPass->onResize(width, height);
    if (m_postProcessing) m_postProcessing->onResize(width, height);
    if (m_gizmoPass) m_gizmoPass->onResize(width, height);
    if (m_hudPass) m_hudPass->onResize(width, height);
    if (m_skyPass) {
        m_skyPass->onResize(width, height);
        // Reconnect to reallocated lighting output image/view
        if (m_lightingPass) {
            m_skyPass->setHDROutput(m_lightingPass->getOutputView(),
                                    m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_skyPass->setDepthBuffer(m_gbufferPass->getDepthView());
        }
    }

    // Recreate particle framebuffer (it references lighting output which was resized)
    if (m_particleSystem && m_particleRenderPass != VK_NULL_HANDLE) {
        if (m_particleFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_particleFramebuffer, nullptr);
            m_particleFramebuffer = VK_NULL_HANDLE;
        }
        createParticleFramebuffer();
    }

    // Reconnect after resize
    if (m_lightingPass && m_gbufferPass) {
        // SSAO, RT GI and the RT shadow mask may all have been recreated above;
        // lighting still holds their old views. Drop them before the rewrite --
        // render() re-supplies them before the lighting pass executes.
        m_lightingPass->clearPerFrameInputs();
        m_lightingPass->setGBufferPass(m_gbufferPass.get());
    }

    // Re-import all textures with new dimensions (passes reallocated their images on resize)
    importGraphTextures();
}

void DeferredRenderer::resetTemporalState() {
    m_particlesLiveUntil -= m_totalTime;   // the same remaining lifetime on the restarted clock
    m_totalTime = 0.0f;
    m_framesSinceReset = 0;
    m_prevViewProjValid = false;   // the next frame uses its own view-projection: no motion into it
    if (m_rtShadow) m_rtShadow->resetHistory();
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

void DeferredRenderer::setEnvMap(VkImageView source, VkImageView prefiltered, VkImageView irradiance,
                                 VkSampler iblSampler, float intensity) {
    m_envView = source;   // RT GI's misses read the environment itself
    m_envIntensity = intensity;
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

void DeferredRenderer::setDirectionalLight(const glm::vec3& direction,
                                           const glm::vec3& /*color*/, float /*intensity*/) {
    m_lightDirection = glm::normalize(direction);
    if (m_csmPass) {
        m_csmPass->setLightDirection(m_lightDirection);
    }
}

void DeferredRenderer::setLights(std::span<const LightData> lights, uint64_t version, float ambient) {
    m_lights = lights;
    m_lightsVersion = version;
    m_ambient = ambient;
}

VkImageView DeferredRenderer::getFinalOutput() const {
    if (m_postProcessing && m_postProcessing->didExecute()) {
        return m_postProcessing->getOutputView();
    }
    if (m_lightingPass) {
        return m_lightingPass->getOutputView();
    }
    return VK_NULL_HANDLE;
}

bool DeferredRenderer::finalOutputIsRGBA8() const {
    return m_postProcessing && m_postProcessing->didExecute();
}

VkImage DeferredRenderer::getFinalOutputImage() const {
    if (m_postProcessing && m_postProcessing->didExecute()) {
        return m_postProcessing->getOutputImage();
    }
    if (m_lightingPass) {
        return m_lightingPass->getOutputImage();
    }
    return VK_NULL_HANDLE;
}

VkImageView DeferredRenderer::getSSAOOutput() const {
    if (m_postProcessing) {
        return m_postProcessing->getSSAOOutput();
    }
    return VK_NULL_HANDLE;
}

glm::vec2 DeferredRenderer::getJitterOffset(uint32_t frameIndex) const {
    if (m_postProcessing) {
        return m_postProcessing->getJitterOffset(frameIndex);
    }
    return glm::vec2(0.0f);
}

void DeferredRenderer::setSkyEnabled(bool enabled) {
    m_skyEnabled = enabled;
    if (m_skyPass) m_skyPass->setEnabled(enabled);
}

void DeferredRenderer::setSunDirection(const glm::vec3& dir) {
    m_skySunDirection = glm::normalize(dir);
    // Also update light direction (sun direction = -light direction for CSM)
    m_lightDirection = -m_skySunDirection;

    // Eagerly compute night state so it's available to the light buffer
    // update (which runs BEFORE render()). Avoids one-frame lag between
    // sky appearance and object lighting.
    float sunHeight = m_skySunDirection.y;
    m_nightFactor = 1.0f - glm::smoothstep(-0.3f, 0.1f, sunHeight);
    glm::vec3 rawMoon = -m_skySunDirection + glm::vec3(0.0f, 0.15f, 0.2f);
    float len = glm::length(rawMoon);
    m_moonDirection = (len > 0.001f) ? rawMoon / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

void DeferredRenderer::setSkyTurbidity(float t) {
    m_skyTurbidity = t;
}

void DeferredRenderer::setSkyIntensity(float i) {
    m_skyIntensity = i;
}

void DeferredRenderer::setSkyGroundColor(const glm::vec3& c) {
    m_skyGroundColor = c;
}

void DeferredRenderer::setWireframeEnabled(bool enabled) {
    m_wireframeEnabled = enabled;
    if (m_gbufferPass) {
        m_gbufferPass->setWireframeEnabled(enabled);
    }
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

void DeferredRenderer::spawnParticles(const glm::vec3& position, ParticleType type,
                                       const glm::vec3& direction) {
    if (!m_particleSystem) return;

    ParticleEmitterConfig config;
    switch (type) {
        case ParticleType::MUZZLE_FLASH:
            config = ParticleSystem::presetMuzzleFlash(position, direction);
            break;
        case ParticleType::IMPACT_SPARK:
            config = ParticleSystem::presetImpactSpark(position, direction);
            break;
        case ParticleType::EXPLOSION:
            config = ParticleSystem::presetExplosion(position);
            break;
        case ParticleType::SMOKE:
            config = ParticleSystem::presetSmoke(position);
            break;
        case ParticleType::WATER_SPLASH:
            config = ParticleSystem::presetWaterSplash(position, direction);
            break;
        default:
            config = ParticleSystem::presetImpactSpark(position, direction);
            break;
    }

    m_pendingEmits.push(config);
}

void DeferredRenderer::emitParticles(const ParticleEmitterConfig& config) {
    if (!m_particleSystem || !m_particlesDrawn) return;
    m_pendingEmits.push(config);
}

void DeferredRenderer::setParticlesDrawn(bool drawn) {
    m_particlesDrawn = drawn;
    if (!drawn) m_pendingEmits = {};
}

void DeferredRenderer::setRTGIRealtime(bool fullResolution, uint32_t rays) {
    if (m_rtGI) m_rtGI->setRealtime(fullResolution, rays);
    m_giFull = fullResolution;
    m_giRays = std::clamp(rays, 1u, 32u);
}

void DeferredRenderer::configureCSM(uint32_t cascades, uint32_t size) {
    if (!m_csmPass || !m_csmPass->configure(cascades, size)) return;
    // A new shadow map: lighting samples it and the graph tracks its layout.
    if (m_lightingPass) m_lightingPass->setShadowMap(m_csmPass->getShadowMapArrayView(), m_csmPass->getShadowSampler());
    importGraphTextures();
}

void DeferredRenderer::importGraphTextures() {
    // Reinitialize the graph to clear previous imported handles.
    m_renderGraph.shutdown();
    m_renderGraph.initialize(m_device, m_physicalDevice);

    // Reset handles
    m_graphDepthHandle   = TextureHandle::invalid();
    m_graphNormalHandle  = TextureHandle::invalid();
    m_graphAlbedoHandle  = TextureHandle::invalid();
    m_graphShadowHandle  = TextureHandle::invalid();
    m_graphLightingHandle = TextureHandle::invalid();

    uint32_t w = m_width  > 0 ? m_width  : 1920u;
    uint32_t h = m_height > 0 ? m_height : 1080u;

    if (m_gbufferPass) {
        if (m_gbufferPass->getDepthImage() != VK_NULL_HANDLE) {
            m_graphDepthHandle = m_renderGraph.importTexture(
                "gbuffer_depth",
                m_gbufferPass->getDepthImage(), m_gbufferPass->getDepthView(),
                m_gbufferPass->getDepthFormat(), w, h,
                VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (m_gbufferPass->getNormalImage() != VK_NULL_HANDLE) {
            m_graphNormalHandle = m_renderGraph.importTexture(
                "gbuffer_normal",
                m_gbufferPass->getNormalImage(), m_gbufferPass->getNormalView(),
                m_gbufferPass->getNormalFormat(), w, h,
                VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (m_gbufferPass->getAlbedoImage() != VK_NULL_HANDLE) {
            m_graphAlbedoHandle = m_renderGraph.importTexture(
                "gbuffer_albedo",
                m_gbufferPass->getAlbedoImage(), m_gbufferPass->getAlbedoView(),
                m_gbufferPass->getAlbedoFormat(), w, h,
                VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }

    if (m_csmPass && m_csmPass->getShadowMapImage() != VK_NULL_HANDLE) {
        // CSM shadow map is an array image; we import the full-array view.
        // Width/height reflect the per-layer size (SHADOW_MAP_SIZE x SHADOW_MAP_SIZE).
        m_graphShadowHandle = m_renderGraph.importTexture(
            "csm_shadow",
            m_csmPass->getShadowMapImage(), m_csmPass->getShadowMapArrayView(),
            VK_FORMAT_D32_SFLOAT,
            m_csmPass->size(), m_csmPass->size(),
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    if (m_lightingPass && m_lightingPass->getOutputImage() != VK_NULL_HANDLE) {
        m_graphLightingHandle = m_renderGraph.importTexture(
            "lighting_output",
            m_lightingPass->getOutputImage(), m_lightingPass->getOutputView(),
            VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    OHAO_LOG(Debug, render, "DeferredRenderer: RenderGraph re-imported textures ({}x{})", w, h);
}

bool DeferredRenderer::createParticleRenderPass() {
    if (!m_lightingPass || !m_gbufferPass) return false;

    // Color attachment: HDR output from lighting (load existing content)
    // Lighting render pass finalLayout = SHADER_READ_ONLY_OPTIMAL, so we must match that
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR format
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth attachment: the G-buffer's, tested but never written (particles have
    // depthWriteEnable off). It stays DEPTH_STENCIL_READ_ONLY, the layout every
    // post-G-buffer reader (SSAO, sky, SSR, TAA) samples it in.
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_particleRenderPass) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "DeferredRenderer: Failed to create particle render pass");
        return false;
    }

    return true;
}

bool DeferredRenderer::createParticleFramebuffer() {
    if (m_particleRenderPass == VK_NULL_HANDLE || !m_lightingPass || !m_gbufferPass) return false;

    VkImageView hdrView = m_lightingPass->getOutputView();
    VkImageView depthView = m_gbufferPass->getDepthView();

    if (hdrView == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE) return false;

    std::array<VkImageView, 2> attachments = {hdrView, depthView};

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_particleRenderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbInfo.pAttachments = attachments.data();
    fbInfo.width = m_width > 0 ? m_width : 1920;
    fbInfo.height = m_height > 0 ? m_height : 1080;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_particleFramebuffer) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "DeferredRenderer: Failed to create particle framebuffer");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Hot-reload — runtime shader swap for MCP AI agents
// ---------------------------------------------------------------------------

bool DeferredRenderer::reloadShaderForPass(std::string_view passName, std::string_view spvPath) {
    // Map pass names to pass pointers
    struct PassEntry {
        const char* name;
        RenderPassBase* pass;
    };

    PassEntry entries[] = {
        {"GBuffer",          m_gbufferPass.get()},
        {"CSM",              m_csmPass.get()},
        {"DeferredLighting", m_lightingPass.get()},
        {"SkyPass",          m_skyPass.get()},
    };

    for (const auto& e : entries) {
        if (passName == e.name) {
            if (!e.pass) {
                OHAO_LOG(Error, render, "DeferredRenderer::reloadShader: pass '{}' not initialized", passName);
                return false;
            }
            bool ok = e.pass->reloadShader(spvPath);
            if (ok) {
                OHAO_LOG(Info, render, "DeferredRenderer: hot-reloaded shader for {}", passName);
            } else {
                OHAO_LOG(Error, render, "DeferredRenderer: hot-reload FAILED for {} (pass doesn't support hot-reload or SPV invalid)",
                         passName);
            }
            return ok;
        }
    }

    OHAO_LOG(Error, render, "DeferredRenderer::reloadShader: unknown pass '{}'", passName);
    return false;
}

} // namespace ohao
