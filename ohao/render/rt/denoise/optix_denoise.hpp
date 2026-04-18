#pragma once

// OptiX denoiser — NVIDIA RTX-only denoiser backend.
//
// API mirrors oidn_denoise.hpp exactly for drop-in dispatch from
// VulkanRenderer::getPixels(). Implementation in optix_denoise.cpp is
// conditionally compiled:
//
//   OHAO_HAS_OPTIX defined  → real OptiX + CUDA impl (see .cpp)
//   OHAO_HAS_OPTIX undef    → no-op stub returns false; caller falls back to OIDN

#include <cstdint>

namespace ohao {

// Denoise a path-traced image using NVIDIA OptiX.
// Inputs: HDR float3 interleaved buffers (beauty, albedo AOV, normal AOV).
// Output: denoised HDR float3 buffer (written in-place to beauty).
// Returns true on success; false on failure (beauty left unchanged).
bool optixDenoise(float* beauty, const float* albedo, const float* normal,
                  uint32_t width, uint32_t height, bool hdr = true);

} // namespace ohao
