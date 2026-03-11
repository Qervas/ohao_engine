#include "deferred_renderer.hpp"
#include "renderer/material/bindless_texture_manager.hpp"
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

    // Initialize cloud shadow pass (top-down march, reuses cloud noise/weather textures)
    if (m_cloudPass) {
        m_cloudShadowPass = std::make_unique<CloudShadowPass>();
        if (!m_cloudShadowPass->initialize(device, physicalDevice)) {
            std::cerr << "DeferredRenderer: CloudShadowPass failed (non-fatal)" << std::endl;
            m_cloudShadowPass.reset();
        } else {
            m_cloudShadowPass->setNoiseTexture(m_cloudPass->getNoiseView(),
                                                m_cloudPass->getNoiseSampler());
            m_cloudShadowPass->setWeatherTexture(m_cloudPass->getWeatherView(),
                                                  m_cloudPass->getWeatherSampler());
            std::cout << "DeferredRenderer: CloudShadowPass OK" << std::endl;
        }
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

    // Initialize sand pass (ochre streaks, runs after snow)
    m_sandPass = std::make_unique<SandPass>();
    if (!m_sandPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: SandPass failed (non-fatal)" << std::endl;
        m_sandPass.reset();
    } else {
        if (m_lightingPass) {
            m_sandPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
        m_sandPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: SandPass OK" << std::endl;
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

    // Initialize God Rays pass (radial light shafts, step 4.61)
    m_godRaysPass = std::make_unique<GodRaysPass>();
    if (!m_godRaysPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: GodRaysPass failed (non-fatal)" << std::endl;
        m_godRaysPass.reset();
    } else {
        if (m_lightingPass) {
            m_godRaysPass->setHDROutput(m_lightingPass->getOutputView(),
                                        m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            // Create a depth-compatible sampler for the pass
            m_godRaysPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
        m_godRaysPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: GodRaysPass OK" << std::endl;
    }

    // Initialize Aurora pass (sky ribbon effect, step 4.62)
    m_auroraPass = std::make_unique<AuroraPass>();
    if (!m_auroraPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: AuroraPass failed (non-fatal)" << std::endl;
        m_auroraPass.reset();
    } else {
        if (m_lightingPass) {
            m_auroraPass->setHDROutput(m_lightingPass->getOutputView(),
                                       m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_auroraPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
        m_auroraPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: AuroraPass OK" << std::endl;
    }

    // Initialize Rainbow pass (prismatic arc, step 4.63)
    m_rainbowPass = std::make_unique<RainbowPass>();
    if (!m_rainbowPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: RainbowPass failed (non-fatal)" << std::endl;
        m_rainbowPass.reset();
    } else {
        if (m_lightingPass) {
            m_rainbowPass->setHDROutput(m_lightingPass->getOutputView(),
                                        m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_rainbowPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
        m_rainbowPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: RainbowPass OK" << std::endl;
    }

    // Initialize terrain pass (GPU-tessellated heightmap, writes into GBuffer, step 2.5)
    m_terrainPass = std::make_unique<TerrainPass>();
    if (!m_terrainPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: TerrainPass failed (non-fatal)" << std::endl;
        m_terrainPass.reset();
    } else {
        if (m_gbufferPass) {
            m_terrainPass->setGBufferAttachments(
                m_gbufferPass->getPositionView(),
                m_gbufferPass->getNormalView(),
                m_gbufferPass->getAlbedoView(),
                m_gbufferPass->getVelocityView(),
                m_gbufferPass->getDepthView(),
                m_gbufferPass->getPositionFormat(),
                m_gbufferPass->getDepthFormat());
        }
        std::cout << "DeferredRenderer: TerrainPass OK" << std::endl;
    }

    // Initialize foliage pass (GPU-instanced billboards, step 2.6)
    m_foliagePass = std::make_unique<FoliagePass>();
    if (!m_foliagePass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: FoliagePass failed (non-fatal)" << std::endl;
        m_foliagePass.reset();
    } else {
        if (m_gbufferPass) {
            m_foliagePass->setGBufferAttachments(
                m_gbufferPass->getPositionView(),
                m_gbufferPass->getNormalView(),
                m_gbufferPass->getAlbedoView(),
                m_gbufferPass->getVelocityView(),
                m_gbufferPass->getDepthView(),
                m_gbufferPass->getPositionFormat(),
                m_gbufferPass->getDepthFormat());
        }
        std::cout << "DeferredRenderer: FoliagePass OK" << std::endl;
    }

    // Initialize decal pass (deferred OBB decals, step 2.7)
    m_decalPass = std::make_unique<DecalPass>();
    if (!m_decalPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: DecalPass failed (non-fatal)" << std::endl;
        m_decalPass.reset();
    } else {
        if (m_gbufferPass) {
            m_decalPass->setGBufferAlbedo(m_gbufferPass->getAlbedoView(),
                                          m_gbufferPass->getAlbedoFormat());
            m_decalPass->setDepthBuffer(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
        if (m_textureManager) {
            m_decalPass->setBindlessManager(m_textureManager);
        }
        std::cout << "DeferredRenderer: DecalPass OK" << std::endl;
    }

    // Initialize Water pass (Gerstner wave forward render, after sky/weather)
    m_waterPass = std::make_unique<WaterPass>();
    if (!m_waterPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: WaterPass failed (non-fatal)" << std::endl;
        m_waterPass.reset();
    } else {
        if (m_lightingPass) {
            m_waterPass->setHDROutput(m_lightingPass->getOutputView(),
                                      m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_waterPass->setDepthBuffer(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
        m_waterPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: WaterPass OK" << std::endl;
    }

    // Initialize FFT ocean simulation (compute-only, optional — failure falls back to Gerstner)
    m_fftOceanSim = std::make_unique<FFTOceanSim>();
    if (!m_fftOceanSim->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: FFTOceanSim failed (non-fatal — FFT mode unavailable)" << std::endl;
        m_fftOceanSim.reset();
    } else {
        std::cout << "DeferredRenderer: FFTOceanSim OK" << std::endl;
    }

    // Initialize Caustics pass (pre-lighting caustic projection, step 2.8)
    m_causticsPass = std::make_unique<CausticsPass>();
    if (!m_causticsPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: CausticsPass failed (non-fatal)" << std::endl;
        m_causticsPass.reset();
    } else {
        if (m_gbufferPass) {
            m_causticsPass->setGBufferImages(
                m_gbufferPass->getDepthView(),
                m_gbufferPass->getAlbedoImage(),
                m_gbufferPass->getAlbedoView());
        }
        m_causticsPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: CausticsPass OK" << std::endl;
    }

    // Initialize Ripple simulation pass (GPU wave equation, step 4.63)
    m_ripplePass = std::make_unique<RipplePass>();
    if (!m_ripplePass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: RipplePass failed (non-fatal)" << std::endl;
        m_ripplePass.reset();
    } else {
        m_ripplePass->setTerrainSize(m_terrainSize);
        std::cout << "DeferredRenderer: RipplePass OK" << std::endl;
    }

    // Initialize Underwater pass (post-effect when camera submerged, step 4.65)
    m_underwaterPass = std::make_unique<UnderwaterPass>();
    if (!m_underwaterPass->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: UnderwaterPass failed (non-fatal)" << std::endl;
        m_underwaterPass.reset();
    } else {
        if (m_lightingPass) {
            // Read and write from the same HDR image.
            // The lighting pass output supports both SAMPLED and STORAGE usages.
            m_underwaterPass->setHDRTarget(m_lightingPass->getOutputView(),
                                           m_lightingPass->getOutputImage(),
                                           m_lightingPass->getOutputView());
        }
        m_underwaterPass->onResize(m_width, m_height);
        std::cout << "DeferredRenderer: UnderwaterPass OK" << std::endl;
    }

    // Initialize render graph and import all pass outputs
    importGraphTextures();

    // GPU timing for per-pass profiling
    initGpuTiming();

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

    if (m_foliagePass) {
        m_foliagePass->cleanup();
        m_foliagePass.reset();
    }
    if (m_decalPass) {
        m_decalPass->cleanup();
        m_decalPass.reset();
    }
    if (m_terrainPass) {
        m_terrainPass->cleanup();
        m_terrainPass.reset();
    }
    if (m_underwaterPass) {
        m_underwaterPass->cleanup();
        m_underwaterPass.reset();
    }
    if (m_ripplePass) {
        m_ripplePass->cleanup();
        m_ripplePass.reset();
    }
    if (m_causticsPass) {
        m_causticsPass->cleanup();
        m_causticsPass.reset();
    }
    if (m_fftOceanSim) {
        m_fftOceanSim->cleanup();
        m_fftOceanSim.reset();
    }
    if (m_waterPass) {
        m_waterPass->cleanup();
        m_waterPass.reset();
    }
    if (m_terrainLayerSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_terrainLayerSampler, nullptr);
        m_terrainLayerSampler = VK_NULL_HANDLE;
    }
    if (m_rainbowPass) {
        m_rainbowPass->cleanup();
        m_rainbowPass.reset();
    }
    if (m_auroraPass) {
        m_auroraPass->cleanup();
        m_auroraPass.reset();
    }
    if (m_godRaysPass) {
        m_godRaysPass->cleanup();
        m_godRaysPass.reset();
    }
    if (m_sandPass) {
        m_sandPass->cleanup();
        m_sandPass.reset();
    }
    if (m_snowPass) {
        m_snowPass->cleanup();
        m_snowPass.reset();
    }
    if (m_rainPass) {
        m_rainPass->cleanup();
        m_rainPass.reset();
    }
    if (m_cloudShadowPass) {
        m_cloudShadowPass->cleanup();
        m_cloudShadowPass.reset();
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
    }

    // Update wind direction from dominant weather state (used by foliage/terrain)
    {
        float wx = m_sandEnabled ? m_sandWindX :
                   m_rainEnabled ? m_rainWindX : m_snowWindX;
        float strength = m_sandEnabled ? m_sandIntensity :
                         m_rainEnabled ? m_rainIntensity : m_snowIntensity;
        if (std::abs(wx) > 0.001f) {
            m_windDirection = glm::normalize(glm::vec3(wx, 0.0f, 0.0f));
            m_windStrength  = glm::clamp(strength * 2.0f, 0.05f, 1.0f);
        } else {
            m_windStrength = 0.05f; // light breeze even with no weather
        }
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

        // Frost temporal integration: accumulates when snow > 0.6, melts when snow clears
        float frostTarget = (m_snowAccumulation > 0.6f) ? 1.0f : 0.0f;
        if (m_frostCover < frostTarget)
            m_frostCover = std::min(m_frostCover + m_frostAccumRate * m_deltaTime, 1.0f);
        else
            m_frostCover = std::max(m_frostCover - m_frostMeltRate * m_deltaTime, 0.0f);
        m_lightingPass->setFrostCover(m_frostCover);

        // Cloud shadow (1-frame latency — shadow map computed after lighting)
        if (m_cloudShadowPass && m_cloudEnabled) {
            m_lightingPass->setCloudShadow(
                m_cloudShadowPass->getOutputView(),
                m_cloudShadowPass->getOutputSampler(),
                m_cloudShadowPass->getMapCenter(),
                m_cloudShadowPass->getMapExtent());
        }

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

    // 2.5. Terrain pass — LOAD_OP_LOAD into GBuffer, writes displaced heightmap terrain
    if (m_terrainPass && m_terrainEnabled) {
        m_renderGraph.addPass("Terrain",
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
            [&](VkCommandBuffer c) {
                m_terrainPass->setViewProjection(m_proj * m_view, m_cameraPos);
                m_terrainPass->setSnowCover(m_snowAccumulation);
                m_terrainPass->setWetness(m_wetness);
                m_terrainPass->setFrostCover(m_frostCover);
                m_terrainPass->setWaterLevel(m_waterLevel);
                m_terrainPass->setTime(m_totalTime);

                if (m_terrainTiles.empty()) {
                    // Single-tile mode (default)
                    m_terrainPass->setTileOffset(0.0f, 0.0f);
                    m_terrainPass->execute(c, frameIndex);
                } else {
                    // Multi-tile mode: render each active tile within cull radius
                    for (auto& tile : m_terrainTiles) {
                        if (!tile.active) continue;
                        // Distance cull: skip tiles far from camera
                        float dx = tile.offsetX - m_cameraPos.x;
                        float dz = tile.offsetZ - m_cameraPos.z;
                        if (dx*dx + dz*dz > m_terrainTileCullRadius * m_terrainTileCullRadius) continue;
                        m_terrainPass->setTileOffset(tile.offsetX, tile.offsetZ);
                        m_terrainPass->execute(c, frameIndex);
                    }
                }
            });
    }

    // 2.6. Foliage pass — GPU-instanced billboards + compute cull, writes into GBuffer
    if (m_foliagePass && m_foliageEnabled) {
        m_renderGraph.addPass("Foliage",
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
            [&](VkCommandBuffer c) {
                m_foliagePass->setMatrices(m_proj * m_view, m_cameraPos);
                m_foliagePass->setCullDistance(m_foliageCullDistance);
                m_foliagePass->setTerrainSize(m_terrainSize);
                // Feed terrain splatmap to foliage culling for density-aware placement
                if (m_terrainPass && m_terrainPass->getSplatmapView() != VK_NULL_HANDLE) {
                    m_foliagePass->setSplatmap(m_terrainPass->getSplatmapView(),
                                               m_terrainPass->getHeightmapSampler());
                }
                // Compute frustum planes from view-projection matrix
                glm::mat4 vp = m_proj * m_view;
                std::array<glm::vec4, 6> planes;
                // Gribb/Hartmann frustum extraction
                for (int i = 0; i < 4; i++) planes[0][i] = vp[i][3] + vp[i][0]; // left
                for (int i = 0; i < 4; i++) planes[1][i] = vp[i][3] - vp[i][0]; // right
                for (int i = 0; i < 4; i++) planes[2][i] = vp[i][3] + vp[i][1]; // bottom
                for (int i = 0; i < 4; i++) planes[3][i] = vp[i][3] - vp[i][1]; // top
                for (int i = 0; i < 4; i++) planes[4][i] = vp[i][3] + vp[i][2]; // near
                for (int i = 0; i < 4; i++) planes[5][i] = vp[i][3] - vp[i][2]; // far
                m_foliagePass->setFrustumPlanes(planes);
                m_foliagePass->setWind(m_windDirection, m_windStrength, m_totalTime);
                m_foliagePass->execute(c, frameIndex);
            });
    }

    // 2.7. Decal pass — OBB-projected decals blend into GBuffer albedo
    if (m_decalPass && m_decalsEnabled) {
        m_renderGraph.addPass("Decals",
            [&](PassBuilder& builder) {
                if (m_graphAlbedoHandle.isValid())
                    builder.declareColorWrite(m_graphAlbedoHandle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (m_graphDepthHandle.isValid())
                    builder.readTexture(m_graphDepthHandle,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            },
            [&](VkCommandBuffer c) {
                glm::mat4 viewProj    = m_proj * m_view;
                glm::mat4 invViewProj = glm::inverse(viewProj);
                m_decalPass->setMatrices(viewProj, invViewProj,
                                         glm::vec2(static_cast<float>(m_width),
                                                   static_cast<float>(m_height)));
                m_decalPass->execute(c, frameIndex);
            });
    }

    // 2.8. Caustics pass — pre-lighting, additive caustic light on submerged geometry
    if (m_causticsPass && m_causticsEnabled && m_waterEnabled) {
        glm::mat4 viewProj    = m_proj * m_view;
        glm::mat4 invViewProj = glm::inverse(viewProj);
        m_causticsPass->setEnabled(m_causticsEnabled);
        m_causticsPass->setInvViewProj(invViewProj);
        m_causticsPass->setScreenSize(m_width, m_height);
        m_causticsPass->setWaterLevel(m_waterLevel);
        m_causticsPass->setTime(m_totalTime);
        m_causticsPass->setCausticsIntensity(m_causticsIntensity);
        m_causticsPass->execute(cmd, frameIndex);
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

    // Time the render graph (CSM+GBuffer+Terrain+Foliage+Decals+SSAO+SSGI+Lighting)
    if (m_gpuTimingEnabled && m_passTimingCount < GPU_TIMER_COUNT) {
        m_passTimingNames[m_passTimingCount] = "RenderGraph";
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, m_passTimingCount * 2);
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

    // 4.5. Cloud pass — ray-march volumetric clouds into half-res buffer
    if (m_cloudPass) {
        timerBegin("CloudPass");
        glm::mat4 invViewProj = glm::inverse(m_proj * m_view);
        m_cloudPass->setEnabled(m_cloudEnabled);
        m_cloudPass->setCameraData(invViewProj, m_cameraPos);
        m_cloudPass->setPrevViewProj(m_prevViewProj);
        m_cloudPass->setFrameIndex(frameIndex);
        m_cloudPass->setLightningData(m_flashIntensity, m_cameraPos.x, m_cameraPos.z);
        m_cloudPass->setSunData(-m_lightDirection,
                                glm::vec3(1.0f, 0.95f, 0.85f),
                                m_skyIntensity);
        m_cloudPass->setTime(m_totalTime);
        m_cloudPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.55. Cloud shadow pass — top-down march for ground shadow
    if (m_cloudShadowPass && m_cloudEnabled) {
        timerBegin("CloudShadow");
        m_cloudShadowPass->setSunDirection(-m_lightDirection);
        m_cloudShadowPass->setCameraPos(m_cameraPos);
        m_cloudShadowPass->setTime(m_totalTime);
        m_cloudShadowPass->setCloudParams(
            m_cloudAltMin, m_cloudAltMax, m_cloudCoverage,
            m_cloudDensity, 0.15f, m_cloudSpeed);
        m_cloudShadowPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.6. Sky pass — fills sky pixels with Preetham sky + cloud composite
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

    // 4.65. Rain pass — procedural rain streaks composited into HDR
    timerBegin("Weather");
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

    // 4.67. Sand pass — ochre streaks for sandstorm
    if (m_sandPass) {
        m_sandPass->setEnabled(m_sandEnabled);
        m_sandPass->setIntensity(m_sandIntensity);
        m_sandPass->setWindX(m_sandWindX);
        m_sandPass->setTime(m_totalTime);
        m_sandPass->execute(cmd, frameIndex);
    }

    // 4.61. God Rays — radial light shafts from sun position
    if (m_godRaysPass) {
        // Project sun direction to screen space
        glm::vec4 sunClip = m_proj * m_view * glm::vec4(-m_lightDirection * 1000.0f, 1.0f);
        glm::vec2 sunNDC  = (sunClip.w > 0.001f)
            ? glm::vec2(sunClip.x / sunClip.w, sunClip.y / sunClip.w)
            : glm::vec2(0.5f, 0.25f);
        // NDC [-1,1] → UV [0,1]  (note Vulkan Y-down, sunNDC.y already flipped by proj)
        glm::vec2 sunUV = glm::vec2(sunNDC.x * 0.5f + 0.5f, sunNDC.y * 0.5f + 0.5f);
        m_godRaysPass->setSunScreenPos(sunUV);
        m_godRaysPass->setIntensity(m_godRaysIntensity);
        m_godRaysPass->setTime(m_totalTime);
        // Only render when sun is above horizon (dot(sun, up) > 0)
        bool sunVisible = -m_lightDirection.y > 0.05f;
        m_godRaysPass->setEnabled(m_godRaysEnabled && sunVisible);
        m_godRaysPass->execute(cmd, frameIndex);
    }
    timerEnd();  // Weather (Rain+Snow+Sand+GodRays)

    // 4.62. Aurora — dancing ribbons in sky
    if (m_auroraPass) {
        timerBegin("Aurora");
        m_auroraPass->setEnabled(m_auroraEnabled);
        m_auroraPass->setIntensity(m_auroraIntensity);
        m_auroraPass->setHue(m_auroraHue);
        m_auroraPass->setTime(m_totalTime);
        m_auroraPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.63. Rainbow — prismatic arc when raining with sun visible
    if (m_rainbowPass) {
        timerBegin("Rainbow");
        float rainbowIntensity = (m_rainEnabled && m_rainIntensity > 0.2f)
            ? m_rainIntensity * 0.8f : 0.0f;
        // Compute antisolar point: reflect sun through screen center
        glm::vec4 sunClip = m_proj * m_view * glm::vec4(-m_lightDirection * 1000.0f, 1.0f);
        glm::vec2 sunNDC  = (sunClip.w > 0.001f)
            ? glm::vec2(sunClip.x / sunClip.w, sunClip.y / sunClip.w)
            : glm::vec2(0.0f, -0.5f);
        glm::vec2 sunUV       = glm::vec2(sunNDC.x * 0.5f + 0.5f, sunNDC.y * 0.5f + 0.5f);
        glm::vec2 antiSolarUV = glm::vec2(1.0f) - sunUV;
        m_rainbowPass->setAntiSolarPos(antiSolarUV);
        m_rainbowPass->setIntensity(rainbowIntensity);
        m_rainbowPass->setEnabled(m_rainbowEnabled && rainbowIntensity > 0.001f);
        m_rainbowPass->execute(cmd, frameIndex);
        timerEnd();
    }

    // 4.625. FFT ocean simulation — compute-only, produces displacement + normal textures
    timerBegin("Water");
    if (m_waveMode == 1 && m_fftOceanSim && m_waterEnabled) {
        // Sync FFT params to sim
        m_fftOceanSim->setPatchSize(m_fftPatchSize);
        m_fftOceanSim->setChoppiness(m_fftChoppiness);
        m_fftOceanSim->setNormalStrength(m_fftNormalStrength);
        m_fftOceanSim->simulate(cmd, m_totalTime, m_deltaTime);
        // Wire FFT textures into WaterPass bindings 9+10
        if (m_waterPass) m_waterPass->setWaveSim(m_fftOceanSim.get());
    } else if (m_waveMode == 0 && m_waterPass) {
        // Clear FFT binding so WaterPass uses Gerstner pipeline
        m_waterPass->setWaveSim(nullptr);
    }

    // 4.63. Ripple simulation — GPU wave equation, output feeds WaterPass normals
    if (m_ripplePass && m_waterRipplesEnabled && m_waterEnabled) {
        m_ripplePass->setEnabled(m_waterRipplesEnabled);
        m_ripplePass->setTerrainSize(m_waterSize);
        m_ripplePass->setDeltaTime(m_deltaTime);
        m_ripplePass->execute(cmd, frameIndex);
        // Feed ripple map to water pass
        if (m_waterPass) {
            m_waterPass->setRippleMap(m_ripplePass->getRippleMapView());
        }
    }

    // 4.64. Water pass — Gerstner wave forward render, semi-transparent, depth-tested
    if (m_waterPass) {
        glm::mat4 viewProj    = m_proj * m_view;
        glm::mat4 invViewProj = glm::inverse(viewProj);
        m_waterPass->setEnabled(m_waterEnabled);
        m_waterPass->setWaterLevel(m_waterLevel);
        m_waterPass->setWaterSize(m_waterSize);
        m_waterPass->setFoamIntensity(m_waterFoamIntensity);
        m_waterPass->setWaveAmplitude(m_waterWaveAmplitude);
        m_waterPass->setTime(m_totalTime);
        m_waterPass->setMatrices(viewProj, invViewProj, m_cameraPos);
        m_waterPass->setWaterSSSStrength(m_waterSSSStrength);
        // Feed HDR scene color + SSR into water for refraction + reflections
        if (m_postProcessing) m_waterPass->setSSROutput(m_postProcessing->getSSROutput());
        // Scene color (HDR lighting output before water) for refraction
        if (m_lightingPass) m_waterPass->setSceneColor(m_lightingPass->getOutputView());
        // Sun direction from weather/sky system
        m_waterPass->setSunDirection(-m_lightDirection, m_skyIntensity > 0 ? m_skyIntensity : 8.0f);
        m_waterPass->execute(cmd, frameIndex);
    }

    // 4.65. Underwater pass — chromatic aberration + depth fog when camera submerged
    if (m_underwaterPass && m_underwaterEnabled && m_waterEnabled) {
        float waterDepth = m_waterLevel - m_cameraPos.y;  // positive when underwater
        m_underwaterPass->setEnabled(waterDepth > 0.0f);
        m_underwaterPass->setWaterDepth(glm::max(waterDepth, 0.0f));
        m_underwaterPass->setTime(m_totalTime);
        m_underwaterPass->setFogColor(m_underwaterFogColor);
        m_underwaterPass->setFogDensity(m_underwaterFogDensity);
        m_underwaterPass->setChromStrength(m_underwaterChromStrength);
        m_underwaterPass->execute(cmd, frameIndex);
    }
    timerEnd();  // Water (FFT+Ripple+Water+Underwater)

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

    // 5. Post-processing (SSR, volumetric, bloom, motion blur, TAA, DoF, heat haze, tonemapping)
    timerBegin("PostProcessing");
    if (m_postProcessing && m_lightingPass) {
        m_postProcessing->setColorBuffer(m_lightingPass->getOutputView());
        m_postProcessing->setHDRInputWithImage(m_lightingPass->getOutputView(),
                                               m_lightingPass->getOutputImage());
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
    if (m_sandPass) {
        m_sandPass->onResize(width, height);
        if (m_lightingPass) {
            m_sandPass->setHDROutput(m_lightingPass->getOutputView(),
                                     m_lightingPass->getOutputImage());
        }
    }
    if (m_godRaysPass) {
        m_godRaysPass->onResize(width, height);
        if (m_lightingPass) {
            m_godRaysPass->setHDROutput(m_lightingPass->getOutputView(),
                                        m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_godRaysPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
    }
    if (m_auroraPass) {
        m_auroraPass->onResize(width, height);
        if (m_lightingPass) {
            m_auroraPass->setHDROutput(m_lightingPass->getOutputView(),
                                       m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_auroraPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
    }
    if (m_rainbowPass) {
        m_rainbowPass->onResize(width, height);
        if (m_lightingPass) {
            m_rainbowPass->setHDROutput(m_lightingPass->getOutputView(),
                                        m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_rainbowPass->setDepthView(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
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
    if (m_waterPass) {
        m_waterPass->onResize(width, height);
        if (m_lightingPass) {
            m_waterPass->setHDROutput(m_lightingPass->getOutputView(),
                                      m_lightingPass->getOutputImage());
        }
        if (m_gbufferPass) {
            m_waterPass->setDepthBuffer(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
        }
    }
    if (m_causticsPass) {
        m_causticsPass->onResize(width, height);
        if (m_gbufferPass) {
            m_causticsPass->setGBufferImages(
                m_gbufferPass->getDepthView(),
                m_gbufferPass->getAlbedoImage(),
                m_gbufferPass->getAlbedoView());
        }
    }
    if (m_underwaterPass) {
        m_underwaterPass->onResize(width, height);
        if (m_lightingPass) {
            m_underwaterPass->setHDRTarget(m_lightingPass->getOutputView(),
                                           m_lightingPass->getOutputImage(),
                                           m_lightingPass->getOutputView());
        }
    }
    if (m_terrainPass && m_gbufferPass) {
        m_terrainPass->onResize(width, height);
        m_terrainPass->setGBufferAttachments(
            m_gbufferPass->getPositionView(),
            m_gbufferPass->getNormalView(),
            m_gbufferPass->getAlbedoView(),
            m_gbufferPass->getVelocityView(),
            m_gbufferPass->getDepthView(),
            m_gbufferPass->getPositionFormat(),
            m_gbufferPass->getDepthFormat());
    }
    if (m_foliagePass && m_gbufferPass) {
        m_foliagePass->onResize(width, height);
        m_foliagePass->setGBufferAttachments(
            m_gbufferPass->getPositionView(),
            m_gbufferPass->getNormalView(),
            m_gbufferPass->getAlbedoView(),
            m_gbufferPass->getVelocityView(),
            m_gbufferPass->getDepthView(),
            m_gbufferPass->getPositionFormat(),
            m_gbufferPass->getDepthFormat());
    }
    if (m_decalPass && m_gbufferPass) {
        m_decalPass->onResize(width, height);
        m_decalPass->setGBufferAlbedo(m_gbufferPass->getAlbedoView(),
                                      m_gbufferPass->getAlbedoFormat());
        m_decalPass->setDepthBuffer(m_gbufferPass->getDepthView(), VK_NULL_HANDLE);
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
    // Forward prefiltered env cube + BRDF LUT to water pass for IBL reflections.
    if (m_waterPass) {
        m_waterPass->setIBL(prefiltered, brdfLUT, iblSampler);
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

// ---------------------------------------------------------------------------
// Water normal map loading — load via BindlessTextureManager + wire to WaterPass.
// ---------------------------------------------------------------------------

// Helper: load a texture and return its VkImageView (or VK_NULL_HANDLE on failure).
static VkImageView loadTextureView(ohao::BindlessTextureManager* mgr,
                                    const std::string& path,
                                    ohao::BindlessTextureType type) {
    if (!mgr || path.empty()) return VK_NULL_HANDLE;
    auto handle = mgr->loadTexture(path, type);
    if (!handle.valid()) return VK_NULL_HANDLE;
    const auto* info = mgr->getTextureInfo(handle);
    return info ? info->view : VK_NULL_HANDLE;
}

void DeferredRenderer::setWaterNormalMap1(const std::string& path) {
    m_waterNormalMap1Path = path;
    if (!m_waterPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Normal);
    if (v != VK_NULL_HANDLE) {
        // If nm2 already set, keep it; otherwise re-use this map for both slots
        VkImageView nm2 = loadTextureView(m_textureManager, m_waterNormalMap2Path,
                                           BindlessTextureType::Normal);
        m_waterPass->setNormalMaps(v, nm2 != VK_NULL_HANDLE ? nm2 : v, VK_NULL_HANDLE);
    }
}

void DeferredRenderer::setWaterNormalMap2(const std::string& path) {
    m_waterNormalMap2Path = path;
    if (!m_waterPass || !m_textureManager) return;
    VkImageView nm1 = loadTextureView(m_textureManager, m_waterNormalMap1Path,
                                       BindlessTextureType::Normal);
    VkImageView nm2 = loadTextureView(m_textureManager, path, BindlessTextureType::Normal);
    if (nm1 != VK_NULL_HANDLE || nm2 != VK_NULL_HANDLE) {
        m_waterPass->setNormalMaps(
            nm1 != VK_NULL_HANDLE ? nm1 : nm2,
            nm2 != VK_NULL_HANDLE ? nm2 : nm1,
            VK_NULL_HANDLE);
    }
}

void DeferredRenderer::setWaterSceneColor(VkImageView view) {
    if (m_waterPass) m_waterPass->setSceneColor(view);
}
void DeferredRenderer::setWaterSSROutput(VkImageView view) {
    if (m_waterPass) m_waterPass->setSSROutput(view);
}
void DeferredRenderer::setWaterSunDirection(const glm::vec3& dir, float intensity) {
    if (m_waterPass) m_waterPass->setSunDirection(dir, intensity);
}
void DeferredRenderer::setWaterColors(const glm::vec3& shallow, const glm::vec3& deep) {
    if (m_waterPass) m_waterPass->setWaterColors(shallow, deep);
}

// ---------------------------------------------------------------------------
// Wave mode / FFT ocean API
// ---------------------------------------------------------------------------

void DeferredRenderer::setWaveMode(int mode) {
    m_waveMode = glm::clamp(mode, 0, 1);
    if (m_waveMode == 0 && m_waterPass) {
        // Immediately switch WaterPass back to Gerstner so descriptors update.
        m_waterPass->setWaveSim(nullptr);
    }
}

void DeferredRenderer::setFFTWindSpeed(float s) {
    m_fftWindSpeed = glm::max(s, 0.1f);
    if (m_fftOceanSim) m_fftOceanSim->setWindSpeed(m_fftWindSpeed);
}

void DeferredRenderer::setFFTWindDirection(float x, float z) {
    m_fftWindDirX = x; m_fftWindDirZ = z;
    if (m_fftOceanSim) m_fftOceanSim->setWindDirection(x, z);
}

void DeferredRenderer::setFFTPatchSize(float s) {
    m_fftPatchSize = glm::max(s, 10.0f);
    if (m_fftOceanSim) m_fftOceanSim->setPatchSize(s);
}

void DeferredRenderer::setFFTChoppiness(float c) {
    m_fftChoppiness = glm::clamp(c, 0.0f, 4.0f);
    if (m_fftOceanSim) m_fftOceanSim->setChoppiness(c);
}

void DeferredRenderer::setFFTNormalStrength(float v) {
    m_fftNormalStrength = glm::max(v, 0.1f);
    if (m_fftOceanSim) m_fftOceanSim->setNormalStrength(v);
}

// ---------------------------------------------------------------------------
// Caustics / Ripple / Underwater / Enhanced Water API
// ---------------------------------------------------------------------------

void DeferredRenderer::setCausticsIntensity(float v) {
    m_causticsIntensity = glm::clamp(v, 0.0f, 2.0f);
    if (m_causticsPass) m_causticsPass->setCausticsIntensity(m_causticsIntensity);
}

void DeferredRenderer::setCausticsTexturePath(const std::string& path) {
    m_causticsTexturePath = path;
    if (!m_textureManager || path.empty()) return;
    // Load texture via bindless manager, then forward view+sampler to caustics pass
    if (m_causticsPass) {
        auto texIdx = m_textureManager->loadTexture(path);
        VkImageView view     = m_textureManager->getImageView(texIdx);
        VkSampler   sampler  = m_textureManager->getSampler(texIdx);
        if (view != VK_NULL_HANDLE) {
            m_causticsPass->setCausticsTexture(view, sampler);
        }
    }
}

void DeferredRenderer::addWaterRipple(float worldX, float worldZ, float strength) {
    if (m_ripplePass) {
        m_ripplePass->addRipple(glm::vec2(worldX, worldZ), strength, 4.0f);
    }
}

void DeferredRenderer::clearWaterRipples() {
    if (m_ripplePass) m_ripplePass->clearRipples();
}

void DeferredRenderer::setWaterSSSStrength(float v) {
    m_waterSSSStrength = glm::clamp(v, 0.0f, 1.0f);
    if (m_waterPass) m_waterPass->setWaterSSSStrength(m_waterSSSStrength);
}

void DeferredRenderer::setWaterFoamTexturePath(const std::string& path) {
    m_waterFoamTexturePath = path;
    if (!m_textureManager || path.empty()) return;
    if (m_waterPass) {
        auto texIdx = m_textureManager->loadTexture(path);
        VkImageView view    = m_textureManager->getImageView(texIdx);
        VkSampler   sampler = m_textureManager->getSampler(texIdx);
        if (view != VK_NULL_HANDLE) {
            m_waterPass->setFoamTexture(view, sampler);
        }
    }
}

void DeferredRenderer::setUnderwaterFogColor(const glm::vec3& c) {
    m_underwaterFogColor = c;
    if (m_underwaterPass) m_underwaterPass->setFogColor(c);
}

void DeferredRenderer::setUnderwaterFogDensity(float v) {
    m_underwaterFogDensity = glm::clamp(v, 0.0f, 1.0f);
    if (m_underwaterPass) m_underwaterPass->setFogDensity(m_underwaterFogDensity);
}

void DeferredRenderer::setUnderwaterChromStrength(float v) {
    m_underwaterChromStrength = glm::clamp(v, 0.0f, 0.05f);
    if (m_underwaterPass) m_underwaterPass->setChromStrength(m_underwaterChromStrength);
}

void DeferredRenderer::setWaterRippleDamping(float v) {
    m_waterRippleDamping = glm::clamp(v, 0.0f, 0.1f);
    if (m_ripplePass) m_ripplePass->setDamping(m_waterRippleDamping);
}

void DeferredRenderer::setWaterRippleSpeed(float v) {
    m_waterRippleSpeed = glm::clamp(v, 0.5f, 20.0f);
    if (m_ripplePass) m_ripplePass->setWaveSpeed(m_waterRippleSpeed);
}

void DeferredRenderer::setCausticsScale(float v) {
    m_causticsScale = glm::clamp(v, 0.01f, 0.5f);
    if (m_causticsPass) m_causticsPass->setCausticsScale(m_causticsScale);
}

void DeferredRenderer::setUnderwaterDistortFrequency(float v) {
    m_underwaterDistortFreq = glm::clamp(v, 1.0f, 40.0f);
    if (m_underwaterPass) m_underwaterPass->setDistortFrequency(m_underwaterDistortFreq);
}

void DeferredRenderer::setUnderwaterDistortSpeed(float v) {
    m_underwaterDistortSpeed = glm::clamp(v, 0.1f, 10.0f);
    if (m_underwaterPass) m_underwaterPass->setDistortSpeed(m_underwaterDistortSpeed);
}

void DeferredRenderer::setWaterGridResolution(int n) {
    m_waterGridN = glm::clamp(n, 32, 256);
    if (m_waterPass) m_waterPass->setGridResolution(m_waterGridN);
}

// ---------------------------------------------------------------------------
// Terrain texture loading.
// ---------------------------------------------------------------------------

// Helper: get-or-create the shared linear-repeat sampler for terrain layers.
static VkSampler getOrCreateLinearSampler(VkDevice device, VkSampler& cache) {
    if (cache != VK_NULL_HANDLE) return cache;
    VkSamplerCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter        = VK_FILTER_LINEAR;
    info.minFilter        = VK_FILTER_LINEAR;
    info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxLod           = VK_LOD_CLAMP_NONE;
    info.anisotropyEnable = VK_FALSE;
    vkCreateSampler(device, &info, nullptr, &cache);
    return cache;
}

void DeferredRenderer::setTerrainHeightmapPath(const std::string& path) {
    m_terrainHeightmapPath = path;
    if (!m_terrainPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Height);
    if (v != VK_NULL_HANDLE) {
        m_terrainPass->setHeightmap(v, VK_NULL_HANDLE);
        m_terrainPass->setEnabled(m_terrainEnabled);
    }
}

void DeferredRenderer::setTerrainSplatMapPath(const std::string& path) {
    m_terrainSplatMapPath = path;
    if (!m_terrainPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Custom);
    if (v != VK_NULL_HANDLE) m_terrainPass->setSplatMap(v, VK_NULL_HANDLE);
}

void DeferredRenderer::setTerrainLayerAlbedo(uint32_t layer, const std::string& path) {
    if (layer >= 4) return;
    m_terrainLayerAlbedoPaths[layer] = path;
    if (!m_terrainPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Albedo);
    if (v != VK_NULL_HANDLE) {
        m_terrainPass->setLayerAlbedo(layer, v);
        VkSampler s = getOrCreateLinearSampler(m_device, m_terrainLayerSampler);
        if (s != VK_NULL_HANDLE) m_terrainPass->setLayerSampler(s);
    }
}

void DeferredRenderer::setTerrainLayerNormal(uint32_t layer, const std::string& path) {
    if (layer >= 4) return;
    m_terrainLayerNormalPaths[layer] = path;
    if (!m_terrainPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Normal);
    if (v != VK_NULL_HANDLE) m_terrainPass->setLayerNormal(layer, v);
}

void DeferredRenderer::setTerrainType(int type) {
    if (!m_terrainPass) return;
    m_terrainPass->setTerrainType(type);
}

void DeferredRenderer::setTerrainGenFrequency(float f) {
    if (!m_terrainPass) return;
    m_terrainPass->setGenFrequency(f);
}

void DeferredRenderer::setTerrainGenOctaves(int n) {
    if (!m_terrainPass) return;
    m_terrainPass->setGenOctaves(n);
}

void DeferredRenderer::setTerrainGenOffset(glm::vec2 off) {
    if (!m_terrainPass) return;
    m_terrainPass->setGenOffset(off);
}

void DeferredRenderer::setTerrainGenResolution(uint32_t r) {
    if (!m_terrainPass) return;
    m_terrainPass->setGenResolution(r);
    m_terrainPass->setHeightmapResolution(r);
}

void DeferredRenderer::setTerrainMacroVariationPath(const std::string& path) {
    if (!m_terrainPass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Custom);
    if (v != VK_NULL_HANDLE) m_terrainPass->setMacroVariation(v);
}

void DeferredRenderer::generateTerrain() {
    if (!m_terrainPass) return;
    // ensureGenHeightmap allocates the storage image if needed; the actual
    // dispatch happens inside TerrainPass::execute() on the next frame.
    m_terrainPass->ensureGenHeightmap();
}

// ---------------------------------------------------------------------------
// Decal management — delegates to DecalPass.
// ---------------------------------------------------------------------------

uint32_t DeferredRenderer::addDecal(const glm::vec3& pos, const glm::vec3& normal,
                                     const glm::vec3& size, const std::string& albedoPath,
                                     float opacity, const glm::vec4& tint) {
    if (!m_decalPass) return 0;

    // Build OBB matrices from pos + normal + size
    // normal defines the Z-axis of the OBB; construct orthonormal frame
    glm::vec3 n = glm::normalize(normal);
    glm::vec3 tangent = glm::abs(n.y) < 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    glm::vec3 t = glm::normalize(glm::cross(tangent, n));
    glm::vec3 b = glm::cross(n, t);

    // worldMatrix transforms [-1,1]³ unit cube to world space
    glm::mat4 world(1.0f);
    world[0] = glm::vec4(t * size.x * 0.5f, 0.0f);
    world[1] = glm::vec4(b * size.y * 0.5f, 0.0f);
    world[2] = glm::vec4(n * size.z * 0.5f, 0.0f);
    world[3] = glm::vec4(pos, 1.0f);
    glm::mat4 decalMat = glm::inverse(world);

    // Resolve albedo to bindless index
    uint32_t albedoIdx = 0xFFFFFFFFu;
    if (m_textureManager && !albedoPath.empty()) {
        auto handle = m_textureManager->loadTexture(albedoPath, BindlessTextureType::Albedo);
        if (handle.valid()) albedoIdx = handle.index;
    }

    DecalPass::DecalDesc desc{};
    desc.decalMatrix    = decalMat;
    desc.worldMatrix    = world;
    desc.colorTint      = tint;
    desc.albedoIdx      = albedoIdx;
    desc.normalIdx      = 0xFFFFFFFFu;
    desc.opacity        = opacity;
    desc.roughnessScale = 1.0f;

    return m_decalPass->addDecal(desc);
}

void DeferredRenderer::removeDecal(uint32_t handle) {
    if (m_decalPass) m_decalPass->removeDecal(handle);
}

void DeferredRenderer::clearDecals() {
    if (m_decalPass) m_decalPass->clearDecals();
}

// ---------------------------------------------------------------------------
// Foliage management — delegates to FoliagePass.
// ---------------------------------------------------------------------------

void DeferredRenderer::setGrassTexturePath(const std::string& path) {
    m_grassTexturePath = path;
    if (!m_foliagePass || !m_textureManager) return;
    VkImageView v = loadTextureView(m_textureManager, path, BindlessTextureType::Albedo);
    if (v != VK_NULL_HANDLE) {
        m_foliagePass->setGrassTexture(v, VK_NULL_HANDLE);
    }
}

void DeferredRenderer::addFoliageCluster(const glm::vec3& center, float radius, float density) {
    if (!m_foliagePass) return;

    // Generate a random scatter of instances within the cluster radius
    std::vector<FoliagePass::FoliageInstance> instances;
    int count = static_cast<int>(density * (radius * radius) / 10000.0f * radius);
    count = std::max(1, std::min(count, 2048));  // clamp per-cluster

    // Simple deterministic scatter using center as seed
    float seed = center.x * 31.7f + center.z * 17.3f;
    for (int i = 0; i < count; i++) {
        seed = std::fmod(seed * 127.3f + 13.7f, 1000.0f);
        float angle = seed / 1000.0f * 6.28318f;
        seed = std::fmod(seed * 127.3f + 13.7f, 1000.0f);
        float r = std::sqrt(seed / 1000.0f) * radius;
        seed = std::fmod(seed * 127.3f + 13.7f, 1000.0f);

        FoliagePass::FoliageInstance inst{};
        inst.position = glm::vec3(center.x + std::cos(angle) * r,
                                  center.y,
                                  center.z + std::sin(angle) * r);
        inst.scale    = 0.4f + (seed / 1000.0f) * 0.4f;
        inst.color    = glm::vec4(0.3f + seed/3000.0f, 0.5f + seed/2000.0f, 0.1f, 1.0f);
        instances.push_back(inst);
    }

    m_foliagePass->uploadInstances(instances);
}

void DeferredRenderer::clearFoliage() {
    if (m_foliagePass) m_foliagePass->clearInstances();
}

// ---------------------------------------------------------------------------
// Multi-tile terrain streaming
// ---------------------------------------------------------------------------

int DeferredRenderer::addTerrainTile(float offsetX, float offsetZ) {
    if (static_cast<int>(m_terrainTiles.size()) >= MAX_TERRAIN_TILES) return -1;
    TerrainTile t;
    t.offsetX = offsetX;
    t.offsetZ = offsetZ;
    t.active  = true;
    m_terrainTiles.push_back(t);
    return static_cast<int>(m_terrainTiles.size() - 1);
}

void DeferredRenderer::clearTerrainTiles() {
    m_terrainTiles.clear();
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
    passes.push_back(passEntry("Terrain",           "graphics", 3,  m_terrainPass != nullptr,    m_terrainPass != nullptr));
    passes.push_back(passEntry("Foliage",           "graphics", 4,  m_foliagePass != nullptr,    m_foliagePass != nullptr));
    passes.push_back(passEntry("Decals",            "graphics", 5,  m_decalPass != nullptr,      m_decalPass != nullptr));
    passes.push_back(passEntry("SSAO",              "compute",  6,  true,                        m_postProcessing && m_postProcessing->getSSAOEnabled()));
    passes.push_back(passEntry("SSGI",              "compute",  7,  true,                        m_postProcessing && m_postProcessing->getSSGIEnabled()));
    passes.push_back(passEntry("DeferredLighting",  "graphics", 8,  m_lightingPass != nullptr,   true));

    // Direct passes
    passes.push_back(passEntry("CloudPass",         "compute",  9,  m_cloudPass != nullptr,          m_cloudEnabled));
    passes.push_back(passEntry("CloudShadowPass",   "compute",  10, m_cloudShadowPass != nullptr,    m_cloudEnabled));
    passes.push_back(passEntry("SkyPass",           "graphics", 11, m_skyPass != nullptr,            m_skyEnabled));
    passes.push_back(passEntry("RainPass",          "graphics", 12, m_rainPass != nullptr,           m_rainEnabled));
    passes.push_back(passEntry("SnowPass",          "graphics", 13, m_snowPass != nullptr,           m_snowEnabled));
    passes.push_back(passEntry("SandPass",          "graphics", 14, m_sandPass != nullptr,           m_sandEnabled));
    passes.push_back(passEntry("GodRaysPass",       "compute",  15, m_godRaysPass != nullptr,        m_godRaysEnabled));
    passes.push_back(passEntry("AuroraPass",        "graphics", 16, m_auroraPass != nullptr,         m_auroraEnabled));
    passes.push_back(passEntry("RainbowPass",       "graphics", 17, m_rainbowPass != nullptr,        m_rainbowEnabled));
    passes.push_back(passEntry("FFTOceanSim",       "compute",  18, m_fftOceanSim != nullptr,        m_fftOceanSim != nullptr));
    passes.push_back(passEntry("CausticsPass",      "compute",  19, m_causticsPass != nullptr,       m_causticsPass != nullptr));
    passes.push_back(passEntry("RipplePass",        "compute",  20, m_ripplePass != nullptr,         m_ripplePass != nullptr));
    passes.push_back(passEntry("WaterPass",         "graphics", 21, m_waterPass != nullptr,          m_waterPass != nullptr));
    passes.push_back(passEntry("UnderwaterPass",    "graphics", 22, m_underwaterPass != nullptr,     m_underwaterPass != nullptr));
    passes.push_back(passEntry("ParticleSystem",    "graphics", 23, m_particleSystem != nullptr,     m_particleSystem != nullptr));
    passes.push_back(passEntry("PostProcessing",    "graphics", 24, m_postProcessing != nullptr,     true));
    passes.push_back(passEntry("GizmoPass",         "graphics", 25, m_gizmoPass != nullptr,          m_gizmoEnabled));

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
    check(m_cloudPass != nullptr,      m_cloudEnabled);
    check(m_rainPass != nullptr,       m_rainEnabled);
    check(m_snowPass != nullptr,       m_snowEnabled);
    check(m_sandPass != nullptr,       m_sandEnabled);
    check(m_godRaysPass != nullptr,    m_godRaysEnabled);
    check(m_auroraPass != nullptr,     m_auroraEnabled);
    check(m_rainbowPass != nullptr,    m_rainbowEnabled);
    check(m_terrainPass != nullptr,    m_terrainPass != nullptr);
    check(m_waterPass != nullptr,      m_waterPass != nullptr);
    check(m_foliagePass != nullptr,    m_foliagePass != nullptr);
    check(m_decalPass != nullptr,      m_decalPass != nullptr);

    return {
        {"resolution",    {m_width, m_height}},
        {"delta_time",    m_deltaTime},
        {"total_time",    m_totalTime},
        {"fps_estimate",  m_deltaTime > 0.0f ? 1.0f / m_deltaTime : 0.0f},
        {"active_passes", activePasses},
        {"ssao_enabled",  m_postProcessing && m_postProcessing->getSSAOEnabled()},
        {"ssgi_enabled",  m_postProcessing && m_postProcessing->getSSGIEnabled()},
        {"effects", {
            {"sky",        m_skyEnabled},
            {"cloud",      m_cloudEnabled},
            {"rain",       m_rainEnabled},
            {"snow",       m_snowEnabled},
            {"sand",       m_sandEnabled},
            {"god_rays",   m_godRaysEnabled},
            {"aurora",     m_auroraEnabled},
            {"rainbow",    m_rainbowEnabled},
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
        {"CloudPass",        m_cloudPass.get()},
        {"CloudShadowPass",  m_cloudShadowPass.get()},
        {"RainPass",         m_rainPass.get()},
        {"SnowPass",         m_snowPass.get()},
        {"SandPass",         m_sandPass.get()},
        {"GodRaysPass",      m_godRaysPass.get()},
        {"AuroraPass",       m_auroraPass.get()},
        {"RainbowPass",      m_rainbowPass.get()},
        {"TerrainPass",      m_terrainPass.get()},
        {"WaterPass",        m_waterPass.get()},
        {"CausticsPass",     m_causticsPass.get()},
        {"RipplePass",       m_ripplePass.get()},
        {"UnderwaterPass",   m_underwaterPass.get()},
        {"DecalPass",        m_decalPass.get()},
        {"FoliagePass",      m_foliagePass.get()},
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
