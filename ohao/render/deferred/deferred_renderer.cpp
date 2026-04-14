#include "deferred_renderer.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "scene/scene.hpp"
#include "scene/component/light_component.hpp"
#include "scene/actor/actor.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
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

    std::cout << "DeferredRenderer: Initializing..." << std::endl;

    // Initialize G-Buffer pass
    m_gbufferPass = std::make_unique<GBufferPass>();
    if (m_textureManager) {
        m_gbufferPass->setTextureManager(m_textureManager);
    }
    if (!m_gbufferPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: GBufferPass failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: GBufferPass OK" << std::endl;

    // Initialize CSM pass
    m_csmPass = std::make_unique<CSMPass>();
    if (!m_csmPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: CSMPass failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: CSMPass OK" << std::endl;

    // Initialize deferred lighting pass
    m_lightingPass = std::make_unique<DeferredLightingPass>();
    if (!m_lightingPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: LightingPass failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: LightingPass OK" << std::endl;

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
        std::cout << "DeferredRenderer: SSSPass OK" << std::endl;
    } else {
        std::cerr << "DeferredRenderer: SSSPass failed (non-fatal)" << std::endl;
        m_sssPass.reset();
    }

    // Initialize SSR pass
    m_ssrPass = std::make_unique<SSRPass>();
    if (m_ssrPass->initialize(device, physicalDevice)) {
        m_ssrPass->setGBufferPass(m_gbufferPass.get());
        std::cout << "DeferredRenderer: SSRPass OK" << std::endl;
    } else {
        std::cerr << "DeferredRenderer: SSRPass failed (non-fatal)" << std::endl;
        m_ssrPass.reset();
    }

    // Initialize post-processing pipeline
    m_postProcessing = std::make_unique<PostProcessingPipeline>();
    if (!m_postProcessing->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: PostProcessing failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: PostProcessing OK" << std::endl;

    // Initialize gizmo pass (transform handles)
    m_gizmoPass = std::make_unique<GizmoPass>();
    if (!m_gizmoPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: GizmoPass failed (non-fatal)" << std::endl;
        m_gizmoPass.reset();
    } else {
        std::cout << "DeferredRenderer: GizmoPass OK" << std::endl;
    }

    // Connect depth/normal/velocity/albedo/position for post-processing
    m_postProcessing->setDepthBuffer(m_gbufferPass->getDepthView());
    m_postProcessing->setNormalBuffer(m_gbufferPass->getNormalView());
    m_postProcessing->setVelocityBuffer(m_gbufferPass->getVelocityView());

    // Initialize particle system
    m_particleSystem = std::make_unique<ParticleSystem>();
    if (!m_particleSystem->initialize(device, physicalDevice, 65536)) {
        std::cerr << "DeferredRenderer: ParticleSystem failed (non-fatal)" << std::endl;
        m_particleSystem.reset();
    } else {
        // Create render pass, framebuffer, and render pipeline for particle rendering
        if (createParticleRenderPass() && createParticleFramebuffer()) {
            if (m_particleSystem->initRenderPipeline(m_particleRenderPass)) {
                std::cout << "DeferredRenderer: ParticleSystem OK" << std::endl;
            } else {
                std::cerr << "DeferredRenderer: Particle render pipeline failed (non-fatal)" << std::endl;
            }
        } else {
            std::cerr << "DeferredRenderer: Particle render pass/framebuffer failed (non-fatal)" << std::endl;
        }
    }

    // Initialize sky pass (Preetham analytical sky)
    m_skyPass = std::make_unique<SkyPass>();
    if (!m_skyPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: SkyPass failed (non-fatal)" << std::endl;
        m_skyPass.reset();
    } else {
        m_skyPass->setHDROutput(m_lightingPass->getOutputView(),
                                m_lightingPass->getOutputImage());
        m_skyPass->setDepthBuffer(m_gbufferPass->getDepthView());
        std::cout << "DeferredRenderer: SkyPass OK" << std::endl;
    }

    // Initialize render graph and import all pass outputs
    importGraphTextures();

    // GPU timing for per-pass profiling
    initGpuTiming();

    // Initialize RT shadow technique
    m_rtShadow = std::make_unique<RTShadowTechnique>();
    if (m_rtShadow->init(device, physicalDevice, m_width, m_height)) {
        m_useRTShadows = true;
        std::cout << "DeferredRenderer: RTShadow OK" << std::endl;
    } else {
        std::cout << "DeferredRenderer: RTShadow not available (using CSM fallback)" << std::endl;
        m_rtShadow.reset();
    }

    // Initialize RT GI technique
    m_rtGI = std::make_unique<RTGITechnique>();
    if (m_rtGI->init(device, physicalDevice, m_width, m_height)) {
        m_useRTGI = true;
        std::cout << "DeferredRenderer: RTGI OK" << std::endl;
    } else {
        std::cout << "DeferredRenderer: RTGI not available" << std::endl;
        m_rtGI.reset();
    }

    std::cout << "DeferredRenderer: Fully initialized" << std::endl;
    return true;
}

void DeferredRenderer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    cleanupGpuTiming();
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
    if (m_gbufferPass) {
        m_gbufferPass->cleanup();
        m_gbufferPass.reset();
    }
}

// ---------------------------------------------------------------------------
// GPU Timing — VkQueryPool timestamp queries for per-pass profiling
// ---------------------------------------------------------------------------

bool DeferredRenderer::initGpuTiming() {
    // Query timestamp period from physical device
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    m_timestampPeriod = props.limits.timestampPeriod;  // nanoseconds per tick

    if (m_timestampPeriod == 0.0f) {
        std::cerr << "DeferredRenderer: GPU does not support timestamps" << std::endl;
        m_gpuTimingEnabled = false;
        return false;
    }

    // Create query pool: 2 queries per pass (begin + end)
    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = GPU_TIMER_COUNT * 2;

    if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_timestampPool) != VK_SUCCESS) {
        std::cerr << "DeferredRenderer: Failed to create timestamp query pool" << std::endl;
        m_gpuTimingEnabled = false;
        return false;
    }

    m_gpuTimingEnabled = true;
    m_passTimingsMs.fill(0.0f);
    m_passTimingNames.fill(nullptr);
    m_passTimingCount = 0;

    std::cout << "DeferredRenderer: GPU timing enabled (period=" << m_timestampPeriod << "ns)" << std::endl;
    return true;
}

void DeferredRenderer::cleanupGpuTiming() {
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
        m_timestampPool = VK_NULL_HANDLE;
    }
    m_gpuTimingEnabled = false;
}

void DeferredRenderer::readbackGpuTimings() {
    if (!m_gpuTimingEnabled || m_passTimingCount == 0) return;

    // Read all timestamp results (2 per pass: begin + end)
    std::array<uint64_t, GPU_TIMER_COUNT * 2> timestamps{};
    uint32_t queryCount = static_cast<uint32_t>(m_passTimingCount * 2);

    VkResult result = vkGetQueryPoolResults(
        m_device, m_timestampPool,
        0, queryCount,
        queryCount * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);

    if (result == VK_SUCCESS) {
        // Convert tick deltas to milliseconds
        float nsToMs = m_timestampPeriod / 1'000'000.0f;
        for (int i = 0; i < m_passTimingCount; i++) {
            uint64_t begin = timestamps[i * 2];
            uint64_t end   = timestamps[i * 2 + 1];
            m_passTimingsMs[i] = static_cast<float>(end - begin) * nsToMs;
        }
    }
    // VK_NOT_READY is fine — first frame has no results yet
}

void DeferredRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex) {
    // Read back GPU timings from PREVIOUS frame (results are 1 frame behind)
    if (m_gpuTimingEnabled) {
        readbackGpuTimings();
        vkCmdResetQueryPool(cmd, m_timestampPool, 0, GPU_TIMER_COUNT * 2);
        m_passTimingCount = 0;
    }

    // Update camera matrices for TAA jitter
    glm::vec2 jitter = m_postProcessing ? m_postProcessing->getJitterOffset(frameIndex) : glm::vec2(0.0f);
    glm::mat4 jitteredProj = m_proj;
    jitteredProj[2][0] += jitter.x;
    jitteredProj[2][1] += jitter.y;

    // Update G-Buffer pass
    if (m_gbufferPass) {
        m_gbufferPass->setScene(m_scene);
        m_gbufferPass->setViewProjection(m_view, jitteredProj, m_prevViewProj);
    }

    // Update CSM pass
    if (m_csmPass) {
        m_csmPass->setScene(m_scene);
        m_csmPass->setLightDirection(m_lightDirection);
        m_csmPass->setCameraData(m_view, m_proj, m_nearPlane, m_farPlane);

        // Share bone resources from GBuffer for skinned shadow rendering
        if (m_gbufferPass) {
            m_csmPass->setBoneDescriptor(
                m_gbufferPass->getBoneDescriptorSet(),
                m_gbufferPass->getBoneDescriptorLayout());
        }
    }

    // Update lighting pass
    if (m_lightingPass) {
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_lightingPass->setCameraData(m_cameraPos, m_view, invViewProj);
        m_lightingPass->setLightBuffer(m_lightBuffer);
        m_lightingPass->setLightCount(m_lightCount);

        // SSAO texture is set after executeSSAO() call below
    }

    // Update post-processing
    if (m_postProcessing) {
        glm::mat4 invProj = glm::inverse(m_proj);
        glm::mat4 invView = glm::inverse(m_view);
        m_postProcessing->setProjectionMatrix(m_proj, invProj);
    }

    // --- Render Graph: barrier-tracked passes (CSM → GBuffer → SSAO → Lighting) ---
    // The graph computes inter-pass image layout barriers centrally.
    // Each pass still manages its own VkRenderPass/VkFramebuffer internally.

    m_renderGraph.reset();

    // 1. Shadow pass (CSM) — writes shadow map, leaves at DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    if (m_csmPass) {
        m_renderGraph.addPass("CSM",
            [&](PassBuilder& builder) {
                if (m_graphShadowHandle.isValid())
                    builder.declareDepthWrite(m_graphShadowHandle,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            },
            [&](VkCommandBuffer c) { m_csmPass->execute(c, frameIndex); });
    }

    // 2. G-Buffer pass — writes MRT outputs; render pass finalLayouts are:
    //    color buffers → SHADER_READ_ONLY_OPTIMAL, depth → DEPTH_STENCIL_READ_ONLY_OPTIMAL
    if (m_gbufferPass) {
        m_renderGraph.addPass("GBuffer",
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
            [&](VkCommandBuffer c) { m_gbufferPass->execute(c, frameIndex); });
    }

    // 3. SSAO — reads normal buffer (already at SHADER_READ_ONLY after GBuffer).
    //    SSAO self-manages its output transitions internally (UNDEFINED→GENERAL→SHADER_READ_ONLY).
    if (m_postProcessing && m_gbufferPass) {
        m_renderGraph.addComputePass("SSAO",
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

    // 3.7. RT Shadows — traces shadow rays using TLAS, outputs shadow mask
    if (m_useRTShadows && m_rtShadow && m_rtAccel && m_rtAccel->isSupported() &&
        m_rtAccel->getInstanceCount() > 0 && m_gbufferPass) {
        m_renderGraph.addComputePass("RTShadow",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readComputeTexture(m_graphNormalHandle);
            },
            [&](VkCommandBuffer c) {
                ShadowInput si{};
                si.positionBuffer = m_gbufferPass->getPositionView();
                si.normalBuffer = m_gbufferPass->getNormalView();
                si.depthBuffer = m_gbufferPass->getDepthView();
                si.view = m_view;
                si.proj = m_proj;
                si.cameraPos = m_cameraPos;
                si.width = m_width;
                si.height = m_height;
                si.lightDirection = m_lightDirection;
                si.lightType = 1;  // point light
                // Use first scene light position if available, else default
                si.lightPosition = glm::vec3(0, 4.5f, 0);
                si.lightRange = 15.0f;
                si.accel = m_rtAccel;
                m_rtShadow->setLightRadius(0.5f);   // soft shadow — 0.5 unit light radius
                m_rtShadow->setSampleCount(8);       // 8 rays per pixel for penumbra
                m_rtShadow->render(c, si);

                // Note: descriptor update happens before render graph execute (see below)
            });
    }

    // 3.8. RT GI — traces indirect rays, outputs color bleeding
    if (m_useRTGI && m_rtGI && m_rtAccel && m_rtAccel->isSupported() &&
        m_rtAccel->getInstanceCount() > 0 && m_gbufferPass) {
        m_renderGraph.addComputePass("RTGI",
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
                gi.depthBuffer = m_gbufferPass->getDepthView();
                gi.view = m_view;
                gi.proj = m_proj;
                gi.cameraPos = m_cameraPos;
                gi.width = m_width;
                gi.height = m_height;
                gi.frameIndex = static_cast<uint32_t>(m_totalTime * 60.0f);
                gi.accel = m_rtAccel;

                // Pass per-instance albedos to RTGI material buffer
                if (m_scene) {
                    std::vector<glm::vec3> albedos;
                    for (const auto& [id, actor] : m_scene->getAllActors()) {
                        auto mc = actor->getComponent<MeshComponent>();
                        if (!mc || !mc->isVisible()) continue;
                        auto matComp = actor->getComponent<MaterialComponent>();
                        glm::vec3 color = matComp ? matComp->getMaterial().baseColor : glm::vec3(0.8f);
                        albedos.push_back(color);
                    }
                    m_rtGI->setMaterialAlbedos(albedos);
                }

                // Find brightest scene light for GI bounce
                if (m_scene) {
                    float bestIntensity = 0.0f;
                    for (const auto& [id, actor] : m_scene->getAllActors()) {
                        auto lc = actor->getComponent<LightComponent>();
                        if (!lc) continue;
                        if (lc->getIntensity() > bestIntensity) {
                            gi.lightPos = actor->getTransform()->getPosition();
                            gi.lightIntensity = lc->getIntensity();
                            bestIntensity = lc->getIntensity();
                        }
                    }
                }
                m_rtGI->setSampleCount(32); // quality over speed
                m_rtGI->render(c, gi);

                // Feed GI output to lighting pass via SSGI binding (11)
                // Note: descriptor update happens before render graph execute
            });
    }

    // 4. Deferred lighting — reads GBuffer + shadow. Key barrier: shadow map
    //    DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL (was missing before).
    if (m_lightingPass && m_gbufferPass) {
        m_renderGraph.addPass("DeferredLighting",
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

    // Time the render graph (CSM+GBuffer+SSAO+Lighting)
    if (m_gpuTimingEnabled && m_passTimingCount < GPU_TIMER_COUNT) {
        m_passTimingNames[m_passTimingCount] = "RenderGraph";
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, m_passTimingCount * 2);
    }
    // Pre-bind RT GI output to lighting pass (SSGI binding 11)
    if (m_useRTGI && m_rtGI && m_lightingPass) {
        auto giOut = m_rtGI->getOutput();
        if (giOut.indirectLightView != VK_NULL_HANDLE) {
            m_lightingPass->setSSGITexture(giOut.indirectLightView, VK_NULL_HANDLE);
            m_lightingPass->updateDescriptorSets();
        }
    }

    // Pre-bind RT shadow mask to lighting pass descriptors (must happen before execute)
    if (m_useRTShadows && m_rtShadow && m_lightingPass) {
        auto output = m_rtShadow->getOutput();
        if (output.shadowMaskView != VK_NULL_HANDLE) {
            m_lightingPass->setRTShadowMask(output.shadowMaskView, VK_NULL_HANDLE);
            m_lightingPass->updateDescriptorSets();
        }
    }

    m_renderGraph.compile();
    m_renderGraph.execute(cmd);
    if (m_gpuTimingEnabled && m_passTimingCount < GPU_TIMER_COUNT) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, m_passTimingCount * 2 + 1);
        m_passTimingCount++;
    }

    // GPU timing helper for direct passes
    auto timerBegin = [&](const char* name) {
        if (m_gpuTimingEnabled && m_passTimingCount < GPU_TIMER_COUNT) {
            m_passTimingNames[m_passTimingCount] = name;
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_timestampPool, m_passTimingCount * 2);
        }
    };
    auto timerEnd = [&]() {
        if (m_gpuTimingEnabled && m_passTimingCount < GPU_TIMER_COUNT) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timestampPool, m_passTimingCount * 2 + 1);
            m_passTimingCount++;
        }
    };

    // 4.5. SSS — separable subsurface scattering blur on skin
    VkImageView litOutput = m_lightingPass ? m_lightingPass->getOutputView() : VK_NULL_HANDLE;
    if (m_sssPass && litOutput) {
        timerBegin("SSS");
        m_sssPass->setLitSceneView(litOutput);
        m_sssPass->execute(cmd, frameIndex);
        litOutput = m_sssPass->getOutputView();  // downstream uses blurred output
        timerEnd();
    }

    // 4.6. SSR — screen-space reflections on glossy surfaces
    if (m_ssrPass && litOutput) {
        timerBegin("SSR");
        m_ssrPass->setCameraData(m_proj * m_view, m_cameraPos);
        m_ssrPass->setLitSceneView(litOutput);
        m_ssrPass->execute(cmd, frameIndex);
        if (m_postProcessing) {
            m_postProcessing->setSSRView(m_ssrPass->getOutputView());
        }
        timerEnd();
    }

    // 4.6. Sky pass — fills sky pixels with Preetham sky
    if (m_skyPass && m_skyEnabled) {
        timerBegin("SkyPass");
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

    // 4.7. Particle system (forward pass over HDR, before post-processing)
    timerBegin("Particles");
    if (m_particleSystem && m_lightingPass) {
        m_totalTime += m_deltaTime;

        // Process pending emits
        while (!m_pendingEmits.empty()) {
            m_particleSystem->emit(cmd, m_pendingEmits.front(), m_totalTime);
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
    }
    timerEnd();  // Particles

    // 5. Post-processing (bloom, TAA, SSAO, tonemapping)
    timerBegin("PostProcessing");
    if (m_postProcessing && m_lightingPass) {
        // Use SSS output if available (blurred skin), otherwise raw lighting
        VkImageView hdrView = (m_sssPass && m_sssPass->getOutputView()) ?
            m_sssPass->getOutputView() : m_lightingPass->getOutputView();
        VkImage hdrImage = (m_sssPass && m_sssPass->getOutputImage()) ?
            m_sssPass->getOutputImage() : m_lightingPass->getOutputImage();
        m_postProcessing->setHDRInputWithImage(hdrView, hdrImage);
        m_postProcessing->setDeltaTime(m_deltaTime);
        m_postProcessing->execute(cmd, frameIndex);
    }
    timerEnd();  // PostProcessing

    // 6. Gizmo pass (transform handles for selected objects)
    timerBegin("Gizmo");
    if (m_gizmoPass && m_gizmoEnabled) {
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
    }
    timerEnd();  // Gizmo

    // Store current view-proj for next frame's motion vectors
    m_prevViewProj = m_proj * m_view;
}

void DeferredRenderer::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    if (m_gbufferPass) m_gbufferPass->onResize(width, height);
    if (m_lightingPass) m_lightingPass->onResize(width, height);
    if (m_postProcessing) m_postProcessing->onResize(width, height);
    if (m_gizmoPass) m_gizmoPass->onResize(width, height);
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
        m_lightingPass->setGBufferPass(m_gbufferPass.get());
    }
    if (m_postProcessing && m_gbufferPass) {
        m_postProcessing->setDepthBuffer(m_gbufferPass->getDepthView());
        m_postProcessing->setNormalBuffer(m_gbufferPass->getNormalView());
        m_postProcessing->setVelocityBuffer(m_gbufferPass->getVelocityView());
    }

    // Re-import all textures with new dimensions (passes reallocated their images on resize)
    importGraphTextures();
}

void DeferredRenderer::setTextureManager(BindlessTextureManager* texManager) {
    m_textureManager = texManager;
    if (m_gbufferPass) {
        m_gbufferPass->setTextureManager(texManager);
    }
}

void DeferredRenderer::setEnvMap(VkImageView view, VkSampler sampler) {
    if (m_lightingPass) {
        m_lightingPass->setEnvMap(view, sampler);
        m_lightingPass->updateDescriptorSets();
    }
}

void DeferredRenderer::setScene(Scene* scene) {
    m_scene = scene;
}

void DeferredRenderer::setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer,
                                          const std::unordered_map<uint64_t, MeshBufferInfo>* bufferMap) {
    // Pass geometry buffers to all rendering passes that need them
    if (m_gbufferPass) {
        m_gbufferPass->setGeometryBuffers(vertexBuffer, indexBuffer);
        m_gbufferPass->setMeshBufferMap(bufferMap);
    }
    if (m_csmPass) {
        m_csmPass->setGeometryBuffers(vertexBuffer, indexBuffer);
        m_csmPass->setMeshBufferMap(bufferMap);
    }
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

void DeferredRenderer::setLightBuffer(VkBuffer lightBuffer, uint32_t lightCount) {
    m_lightBuffer = lightBuffer;
    m_lightCount = lightCount;
}

void DeferredRenderer::setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                                       VkImageView brdfLUT, VkSampler iblSampler) {
    if (m_lightingPass) {
        m_lightingPass->setIBLTextures(irradiance, prefiltered, brdfLUT, iblSampler);
    }
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

void DeferredRenderer::importGraphTextures() {
    // Reinitialize the graph to clear previous imported handles.
    m_renderGraph.shutdown();
    m_renderGraph.initialize(m_device, m_physicalDevice);

    // Reset handles
    m_graphDepthHandle   = TextureHandle::invalid();
    m_graphNormalHandle  = TextureHandle::invalid();
    m_graphAlbedoHandle  = TextureHandle::invalid();
    m_graphShadowHandle  = TextureHandle::invalid();
    m_graphSSAOHandle    = TextureHandle::invalid();
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
            CSMPass::SHADOW_MAP_SIZE, CSMPass::SHADOW_MAP_SIZE,
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    if (m_postProcessing) {
        VkImage ssaoImg = m_postProcessing->getSSAOImage();
        if (ssaoImg != VK_NULL_HANDLE) {
            m_graphSSAOHandle = m_renderGraph.importTexture(
                "ssao_output", ssaoImg, m_postProcessing->getSSAOOutput(),
                VK_FORMAT_R8_UNORM, w, h, VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }

    if (m_lightingPass && m_lightingPass->getOutputImage() != VK_NULL_HANDLE) {
        m_graphLightingHandle = m_renderGraph.importTexture(
            "lighting_output",
            m_lightingPass->getOutputImage(), m_lightingPass->getOutputView(),
            VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    std::cout << "DeferredRenderer: RenderGraph re-imported textures ("
              << w << "x" << h << ")" << std::endl;
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

    // Depth attachment: from GBuffer (read-only for depth testing)
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

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
        std::cerr << "DeferredRenderer: Failed to create particle render pass" << std::endl;
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
        std::cerr << "DeferredRenderer: Failed to create particle framebuffer" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Introspection — pipeline info for MCP AI agents
// ---------------------------------------------------------------------------

nlohmann::json DeferredRenderer::getPipelineInfo() const {
    using json = nlohmann::json;

    auto passEntry = [](const char* name, const char* type, int order,
                        bool initialized, bool enabled) -> json {
        return {
            {"name", name}, {"type", type}, {"order", order},
            {"initialized", initialized}, {"enabled", enabled}
        };
    };

    json passes = json::array();
    // Render graph passes
    passes.push_back(passEntry("CSM",              "graphics", 1,  m_csmPass != nullptr,        true));
    passes.push_back(passEntry("GBuffer",           "graphics", 2,  m_gbufferPass != nullptr,    true));
    passes.push_back(passEntry("SSAO",              "compute",  3,  true,                        m_postProcessing && m_postProcessing->getSSAOEnabled()));
    passes.push_back(passEntry("DeferredLighting",  "graphics", 4,  m_lightingPass != nullptr,   true));
    passes.push_back(passEntry("SkyPass",           "graphics", 5,  m_skyPass != nullptr,        m_skyEnabled));
    passes.push_back(passEntry("ParticleSystem",    "graphics", 6,  m_particleSystem != nullptr, m_particleSystem != nullptr));
    passes.push_back(passEntry("PostProcessing",    "graphics", 7,  m_postProcessing != nullptr, true));
    passes.push_back(passEntry("GizmoPass",         "graphics", 8,  m_gizmoPass != nullptr,      m_gizmoEnabled));

    return {
        {"pass_count",  passes.size()},
        {"passes",      passes},
        {"resolution",  {m_width, m_height}},
        {"delta_time",  m_deltaTime},
        {"total_time",  m_totalTime},
    };
}

nlohmann::json DeferredRenderer::getPerfStats() const {
    using json = nlohmann::json;

    // Count active passes
    int activePasses = 0;
    auto check = [&](bool initialized, bool enabled) {
        if (initialized && enabled) ++activePasses;
    };
    check(m_gbufferPass != nullptr,    true);
    check(m_csmPass != nullptr,        true);
    check(m_lightingPass != nullptr,   true);
    check(m_postProcessing != nullptr, true);
    check(m_skyPass != nullptr,        m_skyEnabled);

    return {
        {"resolution",    {m_width, m_height}},
        {"delta_time",    m_deltaTime},
        {"total_time",    m_totalTime},
        {"fps_estimate",  m_deltaTime > 0.0f ? 1.0f / m_deltaTime : 0.0f},
        {"active_passes", activePasses},
        {"ssao_enabled",  m_postProcessing && m_postProcessing->getSSAOEnabled()},
        {"effects", {
            {"sky",        m_skyEnabled},
        }},
        {"gpu_timing_enabled", m_gpuTimingEnabled},
        {"gpu_timings_ms",     [&]() {
            json timings = json::object();
            float totalMs = 0.0f;
            for (int i = 0; i < m_passTimingCount; i++) {
                if (m_passTimingNames[i]) {
                    timings[m_passTimingNames[i]] = m_passTimingsMs[i];
                    totalMs += m_passTimingsMs[i];
                }
            }
            timings["_total"] = totalMs;
            return timings;
        }()},
    };
}

// ---------------------------------------------------------------------------
// Hot-reload — runtime shader swap for MCP AI agents
// ---------------------------------------------------------------------------

bool DeferredRenderer::reloadShaderForPass(const std::string& passName, const std::string& spvPath) {
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
                std::cerr << "DeferredRenderer::reloadShader: pass '" << passName << "' not initialized" << std::endl;
                return false;
            }
            bool ok = e.pass->reloadShader(spvPath);
            if (ok) {
                std::cout << "DeferredRenderer: hot-reloaded shader for " << passName << std::endl;
            } else {
                std::cerr << "DeferredRenderer: hot-reload FAILED for " << passName
                          << " (pass doesn't support hot-reload or SPV invalid)" << std::endl;
            }
            return ok;
        }
    }

    std::cerr << "DeferredRenderer::reloadShader: unknown pass '" << passName << "'" << std::endl;
    return false;
}

} // namespace ohao
