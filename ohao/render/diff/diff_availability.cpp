#include "render/diff/diff_availability.hpp"

#include "diff/device_caps.hpp"

namespace ohao {

DiffAvailability queryDiffAvailability(VkPhysicalDevice physicalDevice) {
    DiffAvailability out;
    if (physicalDevice == VK_NULL_HANDLE) {
        out.reason = "no physical device has been selected yet";
        return out;
    }

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(physicalDevice);
    out.rayQuery = caps.rayQuery;
    out.bufferFloat32AtomicAdd = caps.bufferFloat32AtomicAdd;
    out.available = caps.sufficient();
    if (out.available) return out;

    // BOTH NAMED WHEN BOTH ARE MISSING, rather than stopping at the first.
    // A user who upgrades a driver for the ray query and then finds the
    // atomics still missing has been told half the truth twice.
    if (!out.rayQuery && !out.bufferFloat32AtomicAdd) {
        out.reason =
            "this device reports neither VK_KHR_ray_query nor shaderBufferFloat32AtomicAdd";
    } else if (!out.rayQuery) {
        out.reason = "this device does not report VK_KHR_ray_query";
    } else {
        out.reason = "this device does not report shaderBufferFloat32AtomicAdd";
    }
    return out;
}

}  // namespace ohao
