#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// PIMPL wrapper around NVIDIA RayTracingDenoiser (NRD).
///
/// 4.A scope: lifecycle only (initialize / shutdown).
/// Denoise dispatch added in 4.B+.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, this header
/// compiles but instantiating NrdDenoiser will produce a link error —
/// callers must guard with `#ifdef OHAO_NRD_ENABLED`.
class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&)            = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    /// Create an NRD instance sized for w x h against the given Vulkan device.
    /// Returns false on NRD-side failure (logs error).
    bool initialize(VkDevice         device,
                    VkPhysicalDevice physicalDevice,
                    uint32_t         width,
                    uint32_t         height);

    /// Destroy the NRD instance. Safe to call multiple times; safe to call
    /// without a prior successful initialize.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
