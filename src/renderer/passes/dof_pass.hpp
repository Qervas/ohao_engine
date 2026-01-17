#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Depth of Field using a physically-based bokeh approach
// Uses Circle of Confusion (CoC) and separable blur
class DoFPass : public RenderPassBase {
public:
    DoFPass() = default;
    ~DoFPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "DoFPass"; }

    // Input configuration
    void setColorBuffer(VkImageView color);
    void setDepthBuffer(VkImageView depth);

    void updateDescriptorSet();

    // Camera lens parameters
    void setFocalLength(float mm) { m_focalLength = mm; }
    void setAperture(float fStop) { m_aperture = fStop; }
    void setFocusDistance(float meters) { m_focusDistance = meters; }
    void setSensorSize(float mm) { m_sensorSize = mm; }

    // Effect parameters
    void setNearBlurStart(float dist) { m_nearStart = dist; }
    void setNearBlurEnd(float dist) { m_nearEnd = dist; }
    void setFarBlurStart(float dist) { m_farStart = dist; }
    void setFarBlurEnd(float dist) { m_farEnd = dist; }
    void setMaxBlurRadius(float pixels) { m_maxBlurRadius = pixels; }
    void setBokehShape(uint32_t blades) { m_bokehBlades = blades; }

    // Projection info (for depth linearization)
    void setNearPlane(float near) { m_nearPlane = near; }
    void setFarPlane(float far) { m_farPlane = far; }

    // Output
    VkImageView getOutputView() const { return m_outputView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createOutputImages();
    bool createDescriptors();
    bool createCoCPipeline();      // Circle of Confusion calculation
    bool createBlurPipeline();     // Separable bokeh blur
    bool createCompositePipeline(); // Combine near/far blur
    void destroyResources();

    // Input views
    VkImageView m_colorView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // Circle of Confusion buffer (single channel)
    VkImage m_cocImage{VK_NULL_HANDLE};
    VkDeviceMemory m_cocMemory{VK_NULL_HANDLE};
    VkImageView m_cocView{VK_NULL_HANDLE};

    // Near field blur (half resolution)
    VkImage m_nearImage{VK_NULL_HANDLE};
    VkDeviceMemory m_nearMemory{VK_NULL_HANDLE};
    VkImageView m_nearView{VK_NULL_HANDLE};

    // Far field blur (half resolution)
    VkImage m_farImage{VK_NULL_HANDLE};
    VkDeviceMemory m_farMemory{VK_NULL_HANDLE};
    VkImageView m_farView{VK_NULL_HANDLE};

    // Temp blur buffer for separable blur
    VkImage m_tempImage{VK_NULL_HANDLE};
    VkDeviceMemory m_tempMemory{VK_NULL_HANDLE};
    VkImageView m_tempView{VK_NULL_HANDLE};

    // Final output
    VkImage m_outputImage{VK_NULL_HANDLE};
    VkDeviceMemory m_outputMemory{VK_NULL_HANDLE};
    VkImageView m_outputView{VK_NULL_HANDLE};

    // Pipelines
    VkPipeline m_cocPipeline{VK_NULL_HANDLE};
    VkPipeline m_blurHPipeline{VK_NULL_HANDLE};  // Horizontal blur
    VkPipeline m_blurVPipeline{VK_NULL_HANDLE};  // Vertical blur
    VkPipeline m_compositePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Camera parameters
    float m_focalLength{50.0f};    // mm
    float m_aperture{2.8f};        // f-stop
    float m_focusDistance{5.0f};   // meters
    float m_sensorSize{36.0f};     // mm (full frame)

    // Near/far blur regions
    float m_nearStart{0.5f};       // Start of near blur
    float m_nearEnd{1.0f};         // End of near blur (fully in focus)
    float m_farStart{10.0f};       // Start of far blur
    float m_farEnd{50.0f};         // Full far blur

    // Effect parameters
    float m_maxBlurRadius{16.0f};  // Maximum blur in pixels
    uint32_t m_bokehBlades{6};     // Aperture blade count for bokeh shape

    // Projection
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Push constants
    struct DoFParams {
        glm::vec4 screenSize;     // xy = size, zw = 1/size
        glm::vec4 focusParams;    // x = focal length, y = aperture, z = focus dist, w = sensor size
        glm::vec4 blurRegions;    // x = near start, y = near end, z = far start, w = far end
        float maxBlurRadius;
        float nearPlane;
        float farPlane;
        uint32_t bokehBlades;
    };
};

} // namespace ohao
