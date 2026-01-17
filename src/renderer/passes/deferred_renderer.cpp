#include "deferred_renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

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

    // Connect shadow map to lighting
    m_lightingPass->setShadowMap(m_csmPass->getShadowMapArrayView(),
                                  m_csmPass->getShadowSampler());

    // Initialize post-processing pipeline
    m_postProcessing = std::make_unique<PostProcessingPipeline>();
    if (!m_postProcessing->initialize(device, physicalDevice)) {
        std::cerr << "DeferredRenderer: PostProcessing failed" << std::endl;
        return false;
    }
    std::cout << "DeferredRenderer: PostProcessing OK" << std::endl;

    // Connect depth/normal/velocity for post-processing
    m_postProcessing->setDepthBuffer(m_gbufferPass->getDepthView());
    m_postProcessing->setNormalBuffer(m_gbufferPass->getNormalView());
    m_postProcessing->setVelocityBuffer(m_gbufferPass->getVelocityView());

    std::cout << "DeferredRenderer: Fully initialized" << std::endl;
    return true;
}

void DeferredRenderer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

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
        m_lightingPass->setCameraData(m_cameraPos, invViewProj);
        m_lightingPass->setLightBuffer(m_lightBuffer);
        m_lightingPass->setLightCount(m_lightCount);

        // Set SSAO if available
        if (m_postProcessing) {
            m_lightingPass->setSSAOTexture(m_postProcessing->getSSAOOutput(),
                                           nullptr); // Sampler from SSAO pass
        }
    }

    // Update post-processing
    if (m_postProcessing) {
        glm::mat4 invProj = glm::inverse(m_proj);
        m_postProcessing->setProjectionMatrix(m_proj, invProj);
    }

    // Execute render passes in order

    // 1. Shadow pass (CSM)
    if (m_csmPass) {
        m_csmPass->execute(cmd, frameIndex);
    }

    // 2. G-Buffer pass
    if (m_gbufferPass) {
        m_gbufferPass->execute(cmd, frameIndex);
    }

    // 3. SSAO pass (before lighting, after G-Buffer)
    // This is handled internally by PostProcessingPipeline

    // 4. Deferred lighting pass
    if (m_lightingPass) {
        m_lightingPass->execute(cmd, frameIndex);
    }

    // 5. Post-processing (bloom, TAA, tonemapping)
    if (m_postProcessing && m_lightingPass) {
        m_postProcessing->setHDRInput(m_lightingPass->getOutputView());
        m_postProcessing->execute(cmd, frameIndex);
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

    // Reconnect after resize
    if (m_lightingPass && m_gbufferPass) {
        m_lightingPass->setGBufferPass(m_gbufferPass.get());
    }
    if (m_postProcessing && m_gbufferPass) {
        m_postProcessing->setDepthBuffer(m_gbufferPass->getDepthView());
        m_postProcessing->setNormalBuffer(m_gbufferPass->getNormalView());
        m_postProcessing->setVelocityBuffer(m_gbufferPass->getVelocityView());
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
    if (m_postProcessing) {
        return m_postProcessing->getOutputView();
    }
    if (m_lightingPass) {
        return m_lightingPass->getOutputView();
    }
    return VK_NULL_HANDLE;
}

VkImage DeferredRenderer::getFinalOutputImage() const {
    if (m_postProcessing) {
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

} // namespace ohao
