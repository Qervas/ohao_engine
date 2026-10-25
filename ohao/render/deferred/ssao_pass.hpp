#pragma once

// SSAO (sub-project 2b spec, "SSAO"): horizon-based ambient occlusion at half resolution,
// then a 4x4 depth-aware blur (compute/ssao.slang, timed together as `ssao`). The lighting
// pass upsamples the blurred image with depth awareness (binding 10, flag bit 1) and
// multiplies only the ambient and environment terms by it.

#include "render_pass_base.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/frame/frame_resources.hpp"
#include <array>

namespace ohao {

class SSAOPass : public RenderPassBase {
public:
    SSAOPass() = default;
    ~SSAOPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    // Both dispatches; the outputs end in SHADER_READ_ONLY_OPTIMAL. Does nothing until
    // setInputs() has the G-buffer.
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "SSAOPass"; }
    // Two entry points in one module: not hot-reloadable.
    [[nodiscard]] bool reloadShader(std::string_view spvPath) override;

    // The G-buffer's world position and normal (SHADER_READ_ONLY_OPTIMAL when executed).
    void setInputs(VkImageView position, VkImageView normal) noexcept {
        m_positionView = position;
        m_normalView = normal;
    }
    // The unjittered view-projection and the camera position of the frame.
    void setCamera(const glm::mat4& viewProj, const glm::vec3& cameraPos) noexcept {
        m_viewProj = viewProj;
        m_cameraPos = cameraPos;
    }

    void setRadius(float radius) { m_radius = radius; }
    void setBias(float bias) { m_bias = bias; }
    void setIntensity(float intensity) { m_intensity = intensity; }
    void setSampleCount(uint32_t directions) { m_directions = directions; }
    // The 4x4 interleaved rotations are a function of this seed alone (default 0, the
    // renderer's default seed), so SSAO is the same in every process. They travel in the
    // push constants: reseeding takes effect on the next frame and uploads nothing.
    void setNoiseSeed(uint32_t seed) noexcept { m_noiseSeed = seed; }
    [[nodiscard]] uint32_t noiseSeed() const noexcept { return m_noiseSeed; }
    // The 16 rotations for `seed`, texel x + 4y: a seeded permutation of (k + 0.5) / 16,
    // each a fraction of the angle between two sampling directions, so a 4x4 window holds
    // 16 evenly spaced rotations.
    [[nodiscard]] static std::array<float, 16> noise(uint32_t seed);

    // The blurred output lighting reads: (ao, distance from the camera; 0 for sky), half
    // resolution rounded up.
    static constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16_SFLOAT;
    [[nodiscard]] VkImageView getOutputView() const { return m_output.view; }
    [[nodiscard]] VkImage getOutputImage() const { return m_output.image; }
    // The occlusion before the blur, same format and size (for tests and debugging).
    [[nodiscard]] VkImage occlusionImage() const { return m_occlusion.image; }
    [[nodiscard]] VkExtent2D outputExtent() const { return {m_output.width, m_output.height}; }
    [[nodiscard]] VkSampler getSampler() const { return m_sampler; }

private:
    [[nodiscard]] bool createImages();
    void destroyImages();

    VkImageView m_positionView{VK_NULL_HANDLE};
    VkImageView m_normalView{VK_NULL_HANDLE};
    glm::mat4 m_viewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};

    rtc::Image m_occlusion{}, m_output{};

    VkPipeline m_occlusionPipeline{VK_NULL_HANDLE}, m_blurPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_occlusionLayout{VK_NULL_HANDLE}, m_blurLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_occlusionSetLayout{VK_NULL_HANDLE}, m_blurSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_occlusionSets{}, m_blurSets{};
    VkSampler m_sampler{VK_NULL_HANDLE};

    uint32_t m_width{0};
    uint32_t m_height{0};

    float m_radius{0.5f};
    float m_bias{0.025f};
    float m_intensity{1.0f};
    uint32_t m_directions{8};
    uint32_t m_steps{4};
    float m_falloffStart{50.0f};
    float m_falloffEnd{300.0f};
    uint32_t m_noiseSeed{0};
};

} // namespace ohao
