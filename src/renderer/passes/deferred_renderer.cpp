#include "deferred_renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <array>
#include <cmath>

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

    // Initialize post-processing pipeline
    m_postProcessing = std::make_unique<PostProcessingPipeline>();
    if (!m_postProcessing->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: PostProcessing failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: PostProcessing OK" << std::endl;

    // Initialize overlay pass (grid, debug visualizations)
    m_overlayPass = std::make_unique<OverlayPass>();
    if (!m_overlayPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: OverlayPass failed (non-fatal)" << std::endl;
        m_overlayPass.reset();
    } else {
        std::cout << "DeferredRenderer: OverlayPass OK" << std::endl;
    }

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
    m_postProcessing->setAlbedoBuffer(m_gbufferPass->getAlbedoView());
    m_postProcessing->setPositionBuffer(m_gbufferPass->getPositionView());

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

    // Initialize cloud pass (volumetric ray-march, runs before sky pass)
    m_cloudPass = std::make_unique<CloudPass>();
    if (!m_cloudPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: CloudPass failed (non-fatal)" << std::endl;
        m_cloudPass.reset();
    } else {
        m_cloudPass->setDepthBuffer(m_gbufferPass->getDepthView());
        std::cout << "DeferredRenderer: CloudPass OK" << std::endl;
    }

    // Initialize rain pass (procedural rain streaks, runs after sky pass)
    m_rainPass = std::make_unique<RainPass>();
    if (!m_rainPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: RainPass failed (non-fatal)" << std::endl;
        m_rainPass.reset();
    } else {
        if (m_lightingPass) {
            m_rainPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
        m_rainPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: RainPass OK" << std::endl;
    }

    // Initialize snow pass (procedural snowflakes + blizzard streaks, runs after rain)
    m_snowPass = std::make_unique<SnowPass>();
    if (!m_snowPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: SnowPass failed (non-fatal)" << std::endl;
        m_snowPass.reset();
    } else {
        if (m_lightingPass) {
            m_snowPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
        m_snowPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: SnowPass OK" << std::endl;
    }

    // Initialize sky pass (Preetham analytical sky, runs after cloud pass)
    m_skyPass = std::make_unique<SkyPass>();
    if (!m_skyPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: SkyPass failed (non-fatal)" << std::endl;
        m_skyPass.reset();
    } else {
        m_skyPass->setHDROutput(m_lightingPass->getOutputView(),
                                m_lightingPass->getOutputImage());
        m_skyPass->setDepthBuffer(m_gbufferPass->getDepthView());
        if (m_cloudPass) {
            m_skyPass->setCloudBuffer(m_cloudPass->getOutputView());
        }
        std::cout << "DeferredRenderer: SkyPass OK" << std::endl;
    }

    // Initialize render graph and import all pass outputs
    importGraphTextures();

    std::cout << "DeferredRenderer: Fully initialized" << std::endl;
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

    if (m_snowPass) {
        m_snowPass->cleanup();
        m_snowPass.reset();
    }
    if (m_rainPass) {
        m_rainPass->cleanup();
        m_rainPass.reset();
    }
    if (m_cloudPass) {
        m_cloudPass->cleanup();
        m_cloudPass.reset();
    }
    if (m_skyPass) {
        m_skyPass->cleanup();
        m_skyPass.reset();
    }
    if (m_gizmoPass) {
        m_gizmoPass->cleanup();
        m_gizmoPass.reset();
    }
    if (m_overlayPass) {
        m_overlayPass->cleanup();
        m_overlayPass.reset();
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

void DeferredRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex) {
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
    }

    // Update lighting pass
    if (m_lightingPass) {
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_lightingPass->setCameraData(m_cameraPos, m_view, invViewProj);
        m_lightingPass->setLightBuffer(m_lightBuffer);
        m_lightingPass->setLightCount(m_lightCount);

        // Temporal wetness integration: ramp toward rain intensity, dry slowly when off
        float wetnessTarget = (m_rainEnabled && m_rainIntensity > 0.001f)
                              ? m_rainIntensity : 0.0f;
        if (m_wetness < wetnessTarget)
            m_wetness = std::min(m_wetness + m_wetRate  * m_deltaTime, wetnessTarget);
        else
            m_wetness = std::max(m_wetness - m_dryRate  * m_deltaTime, wetnessTarget);
        m_lightingPass->setWetness(m_wetness);

        // Snow accumulation integration (slower than wetness — snow builds up and melts very slowly)
        float snowTarget = (m_snowEnabled && m_snowIntensity > 0.001f)
                           ? m_snowIntensity : 0.0f;
        if (m_snowAccumulation < snowTarget)
            m_snowAccumulation = std::min(m_snowAccumulation + m_snowAccumRate * m_deltaTime, snowTarget);
        else
            m_snowAccumulation = std::max(m_snowAccumulation - m_snowMeltRate * m_deltaTime, snowTarget);
        m_lightingPass->setSnowCover(m_snowAccumulation);

        // SSAO texture is set after executeSSAO() call below
    }

    // Update post-processing
    if (m_postProcessing) {
        glm::mat4 invProj = glm::inverse(m_proj);
        glm::mat4 invView = glm::inverse(m_view);
        m_postProcessing->setProjectionMatrix(m_proj, invProj);

        // SSR needs view/proj matrices for screen-space ray marching
        m_postProcessing->setSSRMatrices(m_view, m_proj, invView, invProj);

        // SSGI needs view/proj/invProj for screen-space ray marching
        m_postProcessing->setSSGIMatrices(m_view, m_proj, invProj);

        // Volumetric fog needs view/proj matrices + light/shadow data
        m_postProcessing->setVolumetricMatrices(m_view, m_proj, invView, invProj);
        m_postProcessing->setLightBuffer(m_lightBuffer);
        if (m_csmPass) {
            m_postProcessing->setShadowMap(m_csmPass->getShadowMapArrayView(),
                                            m_csmPass->getShadowSampler());
        }

        // Lightning flash — auto-enable when heavy rain active; also accepts manual trigger
        bool stormActive = m_rainEnabled && m_rainIntensity >= 0.7f;
        bool shouldStrike = m_lightningEnabled || stormActive;
        if (shouldStrike) {
            // Count down timer; forced trigger bypasses it
            bool fireNow = m_lightningTimerForce;
            m_lightningTimerForce = false;
            if (!fireNow) {
                m_lightningTimer -= m_deltaTime;
                if (m_lightningTimer <= 0.0f) fireNow = true;
            }
            if (fireNow) {
                // Randomize: use totalTime as cheap seed
                float seed = std::fmod(m_totalTime * 127.3f, 1.0f);
                float seed2 = std::fmod(m_totalTime * 311.7f, 1.0f);
                m_flashIntensity  = m_lightningBrightness * (0.75f + 0.5f * seed);
                m_flickerTimer    = 0.05f + 0.08f * seed2;  // secondary flicker delay
                m_flickerFired    = false;
                m_lightningPending = true;
                // Shorter intervals during heavy storm
                float intensityFactor = stormActive ? (0.4f + 0.6f * (1.0f - m_rainIntensity)) : 1.0f;
                m_lightningTimer = m_lightningInterval * intensityFactor
                                   * (0.5f + seed);  // ±50% jitter
            }
            // Secondary flicker
            if (!m_flickerFired && m_flickerTimer > 0.0f) {
                m_flickerTimer -= m_deltaTime;
                if (m_flickerTimer <= 0.0f) {
                    m_flashIntensity = m_flashIntensity * 0.4f + m_lightningBrightness * 0.6f;
                    m_flickerFired = true;
                }
            }
            // Exponential decay (~1.5s to reach near zero at default decay)
            float decayFactor = std::pow(0.04f, m_deltaTime);
            m_flashIntensity *= decayFactor;
        } else {
            m_flashIntensity = 0.0f;
        }
        m_postProcessing->setFlashIntensity(m_flashIntensity);
    }

    // --- Render Graph: barrier-tracked passes (CSM → GBuffer → SSAO → SSGI → Lighting) ---
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

        // 3.5. SSGI — reads normal + albedo (both at SHADER_READ_ONLY after GBuffer).
        m_renderGraph.addComputePass("SSGI",
            [&](PassBuilder& builder) {
                if (m_graphNormalHandle.isValid())
                    builder.readComputeTexture(m_graphNormalHandle);
                if (m_graphAlbedoHandle.isValid())
                    builder.readComputeTexture(m_graphAlbedoHandle);
            },
            [&](VkCommandBuffer c) {
                m_postProcessing->executeSSGI(c, frameIndex);
                if (m_lightingPass) {
                    VkImageView ssgiView = m_postProcessing->getSSGIOutput();
                    VkSampler ssgiSampler = m_postProcessing->getSSGISampler();
                    if (ssgiView != VK_NULL_HANDLE && ssgiSampler != VK_NULL_HANDLE)
                        m_lightingPass->setSSGITexture(ssgiView, ssgiSampler);
                }
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

    m_renderGraph.compile();
    m_renderGraph.execute(cmd);

    // 4.5. Cloud pass — ray-march volumetric clouds into half-res buffer
    if (m_cloudPass) {
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_cloudPass->setEnabled(m_cloudEnabled);
        m_cloudPass->setCameraData(invViewProj, m_cameraPos);
        m_cloudPass->setSunData(-m_lightDirection,
                                glm::vec3(1.0f, 0.95f, 0.85f),
                                m_skyIntensity);
        m_cloudPass->setTime(m_totalTime);
        m_cloudPass->execute(cmd, frameIndex);
    }

    // 4.6. Sky pass — fills sky pixels with Preetham sky + cloud composite
    if (m_skyPass && m_skyEnabled) {
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_skyPass->setCameraData(invViewProj, m_cameraPos);
        // Sun direction: opposite of the directional light direction (light points down → sun up)
        m_skyPass->setSunDirection(-m_lightDirection);
        m_skyPass->setTurbidity(m_skyTurbidity);
        m_skyPass->setSunIntensity(m_skyIntensity);
        m_skyPass->setGroundColor(m_skyGroundColor);
        m_skyPass->execute(cmd, frameIndex);
    }

    // 4.65. Rain pass — procedural rain streaks composited into HDR
    if (m_rainPass) {
        m_rainPass->setEnabled(m_rainEnabled);
        m_rainPass->setIntensity(m_rainIntensity);
        m_rainPass->setWindX(m_rainWindX);
        m_rainPass->setTime(m_totalTime);
        m_rainPass->execute(cmd, frameIndex);
    }

    // 4.66. Snow pass — procedural snowflakes composited into HDR
    if (m_snowPass) {
        m_snowPass->setEnabled(m_snowEnabled);
        m_snowPass->setIntensity(m_snowIntensity);
        m_snowPass->setWindX(m_snowWindX);
        m_snowPass->setTime(m_totalTime);
        m_snowPass->execute(cmd, frameIndex);
    }

    // 4.7. Particle system (forward pass over HDR, before post-processing)
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

    // 5. Post-processing (SSR, volumetric, bloom, motion blur, TAA, DoF, tonemapping)
    if (m_postProcessing && m_lightingPass) {
        m_postProcessing->setColorBuffer(m_lightingPass->getOutputView());
        m_postProcessing->setHDRInput(m_lightingPass->getOutputView());
        m_postProcessing->execute(cmd, frameIndex);
    }

    // 6. Overlay pass (grid, debug visualizations)
    if (m_overlayPass && m_gridEnabled) {
        // Use post-processing output if it actually ran, otherwise fall back to lighting output
        VkImageView inputView = VK_NULL_HANDLE;
        if (m_postProcessing && m_postProcessing->didExecute()) {
            inputView = m_postProcessing->getOutputView();
        }
        if (inputView == VK_NULL_HANDLE && m_lightingPass) {
            inputView = m_lightingPass->getOutputView();
        }
        VkImageView depthView = m_gbufferPass ? m_gbufferPass->getDepthView() : VK_NULL_HANDLE;

        if (inputView != VK_NULL_HANDLE && depthView != VK_NULL_HANDLE) {
            glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
            m_overlayPass->setInputImage(inputView);
            m_overlayPass->setDepthBuffer(depthView);
            m_overlayPass->setCameraData(m_view, m_proj, invViewProj, m_cameraPos);
            m_overlayPass->setGridEnabled(m_gridEnabled);
            m_overlayPass->execute(cmd, frameIndex);
        }
    }

    // 7. Gizmo pass (transform handles for selected objects)
    if (m_gizmoPass && m_gizmoEnabled) {
        // Determine the final output image — must be from a pass that actually ran
        VkImage finalImage = VK_NULL_HANDLE;
        VkImageView finalView = VK_NULL_HANDLE;

        if (m_overlayPass && m_gridEnabled && m_overlayPass->getOutputView() != VK_NULL_HANDLE) {
            finalImage = m_overlayPass->getOutputImage();
            finalView = m_overlayPass->getOutputView();
        } else if (m_postProcessing && m_postProcessing->didExecute()) {
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
    if (m_overlayPass) m_overlayPass->onResize(width, height);
    if (m_gizmoPass) m_gizmoPass->onResize(width, height);
    if (m_cloudPass) {
        m_cloudPass->onResize(width, height);
        if (m_gbufferPass) {
            m_cloudPass->setDepthBuffer(m_gbufferPass->getDepthView());
        }
    }
    if (m_rainPass) {
        m_rainPass->onResize(width, height);
        if (m_lightingPass) {
            m_rainPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
    }
    if (m_snowPass) {
        m_snowPass->onResize(width, height);
        if (m_lightingPass) {
            m_snowPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
    }
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
        if (m_cloudPass) {
            m_skyPass->setCloudBuffer(m_cloudPass->getOutputView());
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
        m_postProcessing->setAlbedoBuffer(m_gbufferPass->getAlbedoView());
        m_postProcessing->setPositionBuffer(m_gbufferPass->getPositionView());
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
    if (m_overlayPass && m_gridEnabled && m_overlayPass->getOutputView() != VK_NULL_HANDLE) {
        return m_overlayPass->getOutputView();
    }
    if (m_postProcessing && m_postProcessing->didExecute()) {
        return m_postProcessing->getOutputView();
    }
    if (m_lightingPass) {
        return m_lightingPass->getOutputView();
    }
    return VK_NULL_HANDLE;
}

VkImage DeferredRenderer::getFinalOutputImage() const {
    if (m_overlayPass && m_gridEnabled && m_overlayPass->getOutputImage() != VK_NULL_HANDLE) {
        return m_overlayPass->getOutputImage();
    }
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

// Cloud API
void DeferredRenderer::setCloudEnabled(bool e) {
    m_cloudEnabled = e;
    if (m_cloudPass) m_cloudPass->setEnabled(e);
}

void DeferredRenderer::setCloudCoverage(float v) {
    m_cloudCoverage = v;
    if (m_cloudPass) m_cloudPass->setCoverage(v);
}

void DeferredRenderer::setCloudDensity(float v) {
    m_cloudDensity = v;
    if (m_cloudPass) m_cloudPass->setDensity(v);
}

void DeferredRenderer::setCloudAltMin(float v) {
    m_cloudAltMin = v;
    if (m_cloudPass) m_cloudPass->setAltMin(v);
}

void DeferredRenderer::setCloudAltMax(float v) {
    m_cloudAltMax = v;
    if (m_cloudPass) m_cloudPass->setAltMax(v);
}

void DeferredRenderer::setCloudSpeed(float v) {
    m_cloudSpeed = v;
    if (m_cloudPass) m_cloudPass->setSpeed(v);
}

// Rain API
void DeferredRenderer::setRainEnabled(bool e) {
    m_rainEnabled = e;
    if (m_rainPass) m_rainPass->setEnabled(e);
}

void DeferredRenderer::setRainIntensity(float v) {
    m_rainIntensity = glm::clamp(v, 0.0f, 1.0f);
    if (m_rainPass) m_rainPass->setIntensity(m_rainIntensity);
}

void DeferredRenderer::setRainWindX(float v) {
    m_rainWindX = glm::clamp(v, -1.0f, 1.0f);
    if (m_rainPass) m_rainPass->setWindX(m_rainWindX);
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
    m_graphSSGIHandle    = TextureHandle::invalid();
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
        VkImage ssgiImg = m_postProcessing->getSSGIImage();
        if (ssgiImg != VK_NULL_HANDLE) {
            m_graphSSGIHandle = m_renderGraph.importTexture(
                "ssgi_output", ssgiImg, m_postProcessing->getSSGIOutput(),
                VK_FORMAT_R16G16B16A16_SFLOAT, w / 2, h / 2,
                VK_IMAGE_LAYOUT_UNDEFINED);
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

} // namespace ohao
