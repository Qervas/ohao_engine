#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ohao {

// Denoise a path-traced image using Intel OpenImageDenoise.
// Inputs: HDR float3 buffers (beauty, albedo AOV, normal AOV).
// Empty albedo/normal spans skip those guide images.
// Output: denoised HDR float3 buffer (written in-place to beauty).
// beauty must contain at least width*height*3 floats.
[[nodiscard]] bool oidnDenoise(std::span<float> beauty,
                               std::span<const float> albedo,
                               std::span<const float> normal,
                               uint32_t width, uint32_t height, bool hdr = true);

// Convenience for C-style float3 buffers (nullptr albedo/normal skips guides).
[[nodiscard]] inline bool oidnDenoise(float* beauty, const float* albedo, const float* normal,
                                      uint32_t width, uint32_t height, bool hdr = true) {
    const size_t n3 = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    return oidnDenoise(std::span<float>(beauty, n3),
                       albedo ? std::span<const float>(albedo, n3) : std::span<const float>{},
                       normal ? std::span<const float>(normal, n3) : std::span<const float>{},
                       width, height, hdr);
}

// Convert RGBA32F buffer to RGB float3 (strip alpha, interleaved).
// rgba must contain at least width*height*4 floats.
[[nodiscard]] std::vector<float> rgba32fToFloat3(std::span<const float> rgba,
                                                 uint32_t width, uint32_t height);

[[nodiscard]] inline std::vector<float> rgba32fToFloat3(const float* rgba,
                                                        uint32_t width, uint32_t height) {
    return rgba32fToFloat3(
        std::span<const float>(rgba, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u),
        width, height);
}

// Convert RGB float3 to RGBA8 with ACES tonemapping + gamma.
// rgb must contain at least width*height*3 floats.
[[nodiscard]] std::vector<uint8_t> float3ToRGBA8(std::span<const float> rgb,
                                                 uint32_t width, uint32_t height,
                                                 float exposure = 0.5f);

[[nodiscard]] inline std::vector<uint8_t> float3ToRGBA8(const float* rgb,
                                                        uint32_t width, uint32_t height,
                                                        float exposure = 0.5f) {
    return float3ToRGBA8(
        std::span<const float>(rgb, static_cast<size_t>(width) * static_cast<size_t>(height) * 3u),
        width, height, exposure);
}

} // namespace ohao
