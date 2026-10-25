#pragma once
// The environment as the deferred frame's image-based lighting reads it, built once per
// environment map (shaders/ibl/env_prefilter.slang):
//
//   prefiltered  equirectangular, the environment's size, kEnvPrefilterLevels mip levels:
//                level i is the GGX lobe at roughness i / (kEnvPrefilterLevels - 1) integrated
//                around each direction (level 0 the environment itself). Lighting reads it at
//                lod = roughness * (kEnvPrefilterLevels - 1): the split-sum's first factor.
//   irradiance   equirectangular, kEnvIrradianceWidth x half that: the cosine-weighted mean of
//                the incoming radiance (diffuse irradiance over pi).
//
// Both come from a mip chain of the environment (scratch, kept with the object) sampled at
// the mip each sample's solid angle covers. The path tracer and RT GI keep reading the
// source environment: nothing here changes a path-traced image.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace ohao::render {

class EnvPrefilter {
public:
    EnvPrefilter() = default;
    ~EnvPrefilter();
    EnvPrefilter(const EnvPrefilter&) = delete;
    EnvPrefilter& operator=(const EnvPrefilter&) = delete;

    // Pipelines, the descriptor set layout and the sampler. False (logged) without the SPIR-V.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // Records the build into `cmd` from `source` (RGBA32F, w x h, SHADER_READ_ONLY_OPTIMAL,
    // already written by commands before these). Replaces what an earlier build made: the
    // caller has idled the queue. The images end in SHADER_READ_ONLY_OPTIMAL for fragment
    // shaders once `cmd` has run.
    bool build(VkCommandBuffer cmd, VkImageView source, uint32_t width, uint32_t height);

    [[nodiscard]] bool ready() const noexcept { return m_prefiltered.full != VK_NULL_HANDLE; }
    [[nodiscard]] VkImageView prefilteredView() const noexcept { return m_prefiltered.full; }
    [[nodiscard]] VkImageView irradianceView() const noexcept { return m_irradiance.full; }
    // Linear, mips linear, u repeats, v clamps: for both views.
    [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }

private:
    struct MipImage {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView full{VK_NULL_HANDLE};     // every level, for sampling
        std::vector<VkImageView> levels;      // one a level, for writing
        uint32_t width{0}, height{0}, mips{0};
    };
    bool createMipImage(MipImage& img, uint32_t width, uint32_t height, uint32_t mips);
    void destroyMipImage(MipImage& img);
    void destroyImages();

    VkDescriptorSet allocateSet(VkImageView in, VkImageLayout inLayout, VkImageView chain, VkImageView out);
    void dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, uint32_t w, uint32_t h,
                  uint32_t samples, float roughness);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_layouts[4]{};
    VkPipeline m_copy{VK_NULL_HANDLE}, m_downsample{VK_NULL_HANDLE}, m_prefilter{VK_NULL_HANDLE},
        m_irradianceSum{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};

    MipImage m_chain;         // scratch: the environment and its 2x2 means
    MipImage m_prefiltered;
    MipImage m_irradiance;
    uint32_t m_sourceWidth{0}, m_sourceHeight{0};
};

} // namespace ohao::render
