#pragma once

#include "path_tracer.hpp"

namespace ohao {

class IRTRendererProfile {
public:
    virtual ~IRTRendererProfile() = default;

    virtual const char* getName() const = 0;
    virtual RTRenderProfile getProfile() const = 0;
    virtual RTRenderSettings getDefaultSettings() const = 0;

    virtual bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                      uint32_t width, uint32_t height) = 0;
    virtual void destroy() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;

    virtual void render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                        const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& lightPos, float lightIntensity,
                        const glm::vec3& lightColor, float lightRadius) = 0;

    virtual VkImage getOutputImage() const = 0;
    virtual VkImage getAccumImage() const = 0;
    virtual VkImage getAlbedoAOV() const = 0;
    virtual VkImage getNormalAOV() const = 0;
    virtual VkImageView getMotionVectorAOV() const = 0;

    virtual void setMaterialData(const std::vector<glm::vec4>& materials) = 0;
    virtual void setNormalBuffer(VkBuffer normalBuf, VkBuffer indexBuf, uint32_t vertexCount) = 0;
    virtual void setUVBuffer(VkBuffer uvBuf) = 0;
    virtual void setMaterialBuffers(VkBuffer matIDBuf, VkBuffer matColorBuf) = 0;
    virtual void setLightBuffer(VkBuffer lightBuf, uint32_t lightCount) = 0;
    virtual void setTextureArray(VkImageView view, VkSampler sampler, uint32_t count) = 0;
    virtual void setBindlessTextures(const std::vector<VkImageView>& views,
                                     const std::vector<VkSampler>& samplers) = 0;
    virtual std::vector<VkImageView> getBindlessImageViews() const = 0;
    virtual std::vector<VkSampler> getBindlessSamplers() const = 0;

    virtual void setEnvCDFBuffers(VkBuffer marginal, VkBuffer conditional,
                                   uint32_t envWidth, uint32_t envHeight, float integral) = 0;

    virtual void setRenderSettings(const RTRenderSettings& settings) = 0;
    virtual void notifyViewChanged() = 0;
    virtual void resetAccumulation() = 0;
    virtual uint32_t getFrameIndex() const = 0;
    virtual bool resetsAccumulationOnViewChange() const = 0;
};

class RTProfileRendererBase : public IRTRendererProfile {
public:
    RTProfileRendererBase(RTRenderSettings settings, PathTracerShaderSet shaderSet)
        : m_settings(settings), m_shaderSet(shaderSet) {}

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height) override {
        m_pathTracer.setShaderSet(m_shaderSet);
        if (!m_pathTracer.init(device, physicalDevice, width, height)) {
            return false;
        }
        m_pathTracer.setMaxBounces(m_settings.maxBounces);
        m_pathTracer.setRenderSettings(m_settings);
        return true;
    }

    void destroy() override { m_pathTracer.destroy(); }
    void resize(uint32_t width, uint32_t height) override { m_pathTracer.resize(width, height); }

    void render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& lightPos, float lightIntensity,
                const glm::vec3& lightColor, float lightRadius) override {
        m_pathTracer.render(cmd, accel, view, proj, lightPos, lightIntensity, lightColor, lightRadius);
    }

    VkImage getOutputImage() const override { return m_pathTracer.getOutputImage(); }
    VkImage getAccumImage() const override { return m_pathTracer.getAccumImage(); }
    VkImage getAlbedoAOV() const override { return m_pathTracer.getAlbedoAOV(); }
    VkImage getNormalAOV() const override { return m_pathTracer.getNormalAOV(); }
    VkImageView getMotionVectorAOV() const override { return m_pathTracer.getMotionVectorAOV(); }

    void setMaterialData(const std::vector<glm::vec4>& materials) override { m_pathTracer.setMaterialData(materials); }
    void setNormalBuffer(VkBuffer normalBuf, VkBuffer indexBuf, uint32_t vertexCount) override {
        m_pathTracer.setNormalBuffer(normalBuf, indexBuf, vertexCount);
    }
    void setUVBuffer(VkBuffer uvBuf) override { m_pathTracer.setUVBuffer(uvBuf); }
    void setMaterialBuffers(VkBuffer matIDBuf, VkBuffer matColorBuf) override {
        m_pathTracer.setMaterialBuffers(matIDBuf, matColorBuf);
    }
    void setLightBuffer(VkBuffer lightBuf, uint32_t lightCount) override { m_pathTracer.setLightBuffer(lightBuf, lightCount); }
    void setTextureArray(VkImageView view, VkSampler sampler, uint32_t count) override {
        m_pathTracer.setTextureArray(view, sampler, count);
    }
    void setBindlessTextures(const std::vector<VkImageView>& views,
                             const std::vector<VkSampler>& samplers) override {
        m_pathTracer.setBindlessTextures(views, samplers);
    }
    std::vector<VkImageView> getBindlessImageViews() const override { return m_pathTracer.getBindlessImageViews(); }
    std::vector<VkSampler> getBindlessSamplers() const override { return m_pathTracer.getBindlessSamplers(); }

    void setEnvCDFBuffers(VkBuffer marginal, VkBuffer conditional,
                          uint32_t envWidth, uint32_t envHeight, float integral) override {
        m_pathTracer.setEnvCDFBuffers(marginal, conditional, envWidth, envHeight, integral);
    }

    void setRenderSettings(const RTRenderSettings& settings) override {
        m_settings = settings;
        m_pathTracer.setMaxBounces(settings.maxBounces);
        m_pathTracer.setRenderSettings(settings);
    }
    void notifyViewChanged() override {
        if (resetsAccumulationOnViewChange()) {
            m_pathTracer.resetAccumulation();
            return;
        }
        m_pathTracer.notifyViewChanged();
    }
    void resetAccumulation() override { m_pathTracer.resetAccumulation(); }
    uint32_t getFrameIndex() const override { return m_pathTracer.getFrameIndex(); }

protected:
    PathTracer m_pathTracer;
    RTRenderSettings m_settings;
    PathTracerShaderSet m_shaderSet;
};

class RTRealtimeRenderer final : public RTProfileRendererBase {
public:
    RTRealtimeRenderer()
        : RTProfileRendererBase(kRealtimeRTSettings,
            PathTracerShaderSet{
                "bin/shaders/rt_pt_raygen_realtime.rgen.spv",
                "bin/shaders/rt_pt_miss.rmiss.spv",
                "bin/shaders/rt_pt_closesthit.rchit.spv",
                "bin/shaders/rt_pt_anyhit.rahit.spv"}) {}
    const char* getName() const override { return "RTRealtimeRenderer"; }
    RTRenderProfile getProfile() const override { return RTRenderProfile::Realtime; }
    RTRenderSettings getDefaultSettings() const override { return kRealtimeRTSettings; }
    bool resetsAccumulationOnViewChange() const override { return false; }
};

class RTOfflineRenderer final : public RTProfileRendererBase {
public:
    RTOfflineRenderer()
        : RTProfileRendererBase(kOfflineRTSettings,
            PathTracerShaderSet{
                "bin/shaders/rt_pt_raygen_offline.rgen.spv",
                "bin/shaders/rt_pt_miss.rmiss.spv",
                "bin/shaders/rt_pt_closesthit.rchit.spv",
                "bin/shaders/rt_pt_anyhit.rahit.spv"}) {}
    const char* getName() const override { return "RTOfflineRenderer"; }
    RTRenderProfile getProfile() const override { return RTRenderProfile::Offline; }
    RTRenderSettings getDefaultSettings() const override { return kOfflineRTSettings; }
    bool resetsAccumulationOnViewChange() const override { return true; }
};

} // namespace ohao
