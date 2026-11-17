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
#include "gpu/vulkan/gpu_allocator.hpp"
#include "gpu/vulkan/gpu_image.hpp"
#include "render/graph/pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace ohao::render {

class EnvPrefilter {
public:
    EnvPrefilter() = default;
    ~EnvPrefilter();
    EnvPrefilter(const EnvPrefilter&) = delete;
    EnvPrefilter& operator=(const EnvPrefilter&) = delete;

    // Pipelines, the descriptor set layout and the sampler. False (logged) without the SPIR-V. `allocator`
    // outlives cleanup().
    bool initialize(VkDevice device, GpuAllocator& allocator);
    void cleanup();

    // Records the build into `frame` from `source` (RGBA32F, equirectangular). Replaces what an earlier build
    // made: those images are retired, and the earlier build's commands have run (its sets are reset). A pass
    // that reads prefiltered() or irradiance() declares it sampled.
    bool build(const Frame& frame, gpu::Image& source);

    [[nodiscard]] bool ready() const noexcept { return static_cast<bool>(m_prefiltered); }
    [[nodiscard]] gpu::Image& prefiltered() noexcept { return m_prefiltered; }
    [[nodiscard]] gpu::Image& irradiance() noexcept { return m_irradiance; }
    // Linear, mips linear, u repeats, v clamps: for both images.
    [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }

private:
    void retireImages();

    VkDescriptorSet allocateSet(VkImageView in, VkImageLayout inLayout, VkImageView out);
    // One compute pass: `in` read as `source` says (the environment, sampled; else a level of the chain, in
    // GENERAL), every level of the chain sampled when `readsChain`, one level of `out` written.
    void dispatch(const Frame& frame, VkPipeline pipeline, gpu::Image& in, uint32_t inMip, bool readsChain,
                  gpu::Image& out, uint32_t outMip, uint32_t samples, float roughness);

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_layouts[4]{};
    VkPipeline m_copy{VK_NULL_HANDLE}, m_downsample{VK_NULL_HANDLE}, m_prefilter{VK_NULL_HANDLE},
        m_irradianceSum{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};

    gpu::Image m_chain;         // scratch: the environment and its 2x2 means
    gpu::Image m_prefiltered;
    gpu::Image m_irradiance;
};

} // namespace ohao::render
