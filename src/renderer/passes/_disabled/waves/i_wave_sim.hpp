#pragma once

#include <vulkan/vulkan.h>

namespace ohao {

// Abstract interface for wave simulation backends.
// WaterPass holds an IWaveSim* and calls simulate() once per frame before
// rendering.  If providesTextures() returns true, WaterPass binds
// getDisplacementView() and getNormalView() to descriptor bindings 9 / 10
// and uses the FFT vertex shader; otherwise it uses the inline Gerstner shader.
class IWaveSim {
public:
    virtual ~IWaveSim() = default;

    // Called once after the WaterPass is initialized.
    virtual bool initialize(VkDevice /*device*/, VkPhysicalDevice /*physicalDevice*/) { return true; }

    // Release all Vulkan resources.
    virtual void cleanup() {}

    // Record GPU compute work for the current frame.
    // time  — total elapsed time in seconds
    // dt    — frame delta time (currently unused but available for future use)
    virtual void simulate(VkCommandBuffer /*cmd*/, float /*time*/, float /*dt*/) {}

    // Returns true if this implementation produces displacement / normal textures
    // that WaterPass should sample via bindings 9 + 10.
    virtual bool providesTextures() const = 0;

    // Displacement map: post-IFFT result.
    //   .r = height Y,  .b = choppiness Dx
    // Valid only if providesTextures() == true.
    virtual VkImageView getDisplacementView() const { return VK_NULL_HANDLE; }

    // Normal map from fft_normal.comp:
    //   .rgb = packed world normal [0,1],  .a = foam factor
    // Valid only if providesTextures() == true.
    virtual VkImageView getNormalView() const { return VK_NULL_HANDLE; }
};

} // namespace ohao
