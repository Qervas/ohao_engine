#pragma once

#include <vulkan/vulkan.h>

namespace ohao::diff {

/// What the differentiable renderer needs from a physical device.
///
/// Note that shaderImageFloat32AtomicAdd is deliberately absent: gradients are
/// buffer-backed (design doc S4.3), so image atomics are never used. That
/// matters because image float atomics are materially less available -- the
/// Intel iGPU on this machine reports buffer=true, image=false.
struct DeviceCaps {
    bool rayQuery{false};
    bool bufferFloat32AtomicAdd{false};

    [[nodiscard]] bool sufficient() const noexcept {
        return rayQuery && bufferFloat32AtomicAdd;
    }
};

[[nodiscard]] DeviceCaps queryDeviceCaps(VkPhysicalDevice physicalDevice);

}  // namespace ohao::diff
