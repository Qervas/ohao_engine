#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace ohao {

// Denoise a path-traced image using Intel OpenImageDenoise
// Inputs: HDR float3 buffers (beauty, albedo AOV, normal AOV)
// Output: denoised HDR float3 buffer (written in-place to beauty)
bool oidnDenoise(float* beauty, const float* albedo, const float* normal,
                 uint32_t width, uint32_t height, bool hdr = true);

// Convert RGBA32F buffer to RGB float3 (strip alpha, interleaved)
std::vector<float> rgba32fToFloat3(const float* rgba, uint32_t width, uint32_t height);

// Convert RGB float3 to RGBA8 with ACES tonemapping + gamma
std::vector<uint8_t> float3ToRGBA8(const float* rgb, uint32_t width, uint32_t height, float exposure = 0.5f);

} // namespace ohao
