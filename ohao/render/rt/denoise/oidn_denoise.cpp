#include "render/rt/denoise/oidn_denoise.hpp"
#include <OpenImageDenoise/oidn.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace ohao {

bool oidnDenoise(std::span<float> beauty, std::span<const float> albedo,
                 std::span<const float> normal, uint32_t width, uint32_t height, bool hdr) {
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (beauty.size() < pixelCount * 3u) {
        std::cerr << "[OIDN] beauty buffer too small\n";
        return false;
    }

    oidn::DeviceRef device = oidn::newDevice(oidn::DeviceType::Default);
    device.commit();

    oidn::FilterRef filter = device.newFilter("RT");
    filter.setImage("color",  beauty.data(), oidn::Format::Float3, width, height);
    filter.setImage("output", beauty.data(), oidn::Format::Float3, width, height);  // in-place
    if (!albedo.empty()) {
        filter.setImage("albedo", const_cast<float*>(albedo.data()), oidn::Format::Float3, width, height);
    }
    if (!normal.empty()) {
        filter.setImage("normal", const_cast<float*>(normal.data()), oidn::Format::Float3, width, height);
    }
    filter.set("hdr", hdr);
    filter.commit();
    filter.execute();

    const char* errorMessage = nullptr;
    if (device.getError(errorMessage) != oidn::Error::None) {
        std::cerr << "[OIDN] Error: " << errorMessage << std::endl;
        return false;
    }

    std::cout << "[OIDN] Denoised " << width << "x" << height << std::endl;
    return true;
}

std::vector<float> rgba32fToFloat3(std::span<const float> rgba, uint32_t width, uint32_t height) {
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> rgb(pixelCount * 3u);
    for (size_t i = 0; i < pixelCount; i++) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return rgb;
}

// ACES tonemapping
static float aces(float x) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

std::vector<uint8_t> float3ToRGBA8(std::span<const float> rgb, uint32_t width, uint32_t height,
                                   float exposure) {
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba8(pixelCount * 4u);
    for (size_t i = 0; i < pixelCount; i++) {
        float r = aces(rgb[i * 3 + 0] * exposure);
        float g = aces(rgb[i * 3 + 1] * exposure);
        float b = aces(rgb[i * 3 + 2] * exposure);
        // Gamma
        r = std::pow(r, 1.0f / 2.2f);
        g = std::pow(g, 1.0f / 2.2f);
        b = std::pow(b, 1.0f / 2.2f);
        rgba8[i * 4 + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
        rgba8[i * 4 + 1] = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
        rgba8[i * 4 + 2] = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
        rgba8[i * 4 + 3] = 255;
    }
    return rgba8;
}

} // namespace ohao
