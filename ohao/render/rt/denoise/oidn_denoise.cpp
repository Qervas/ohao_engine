#include "render/rt/denoise/oidn_denoise.hpp"
#include <OpenImageDenoise/oidn.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>

namespace ohao {

bool oidnDenoise(float* beauty, const float* albedo, const float* normal,
                 uint32_t width, uint32_t height, bool hdr) {
    oidn::DeviceRef device = oidn::newDevice(oidn::DeviceType::Default);
    device.commit();

    oidn::FilterRef filter = device.newFilter("RT");
    filter.setImage("color",  beauty, oidn::Format::Float3, width, height);
    filter.setImage("output", beauty, oidn::Format::Float3, width, height);  // in-place
    if (albedo) filter.setImage("albedo", (void*)albedo, oidn::Format::Float3, width, height);
    if (normal) filter.setImage("normal", (void*)normal, oidn::Format::Float3, width, height);
    filter.set("hdr", hdr);
    filter.commit();
    filter.execute();

    const char* errorMessage;
    if (device.getError(errorMessage) != oidn::Error::None) {
        std::cerr << "[OIDN] Error: " << errorMessage << std::endl;
        return false;
    }

    std::cout << "[OIDN] Denoised " << width << "x" << height << std::endl;
    return true;
}

std::vector<float> rgba32fToFloat3(const float* rgba, uint32_t width, uint32_t height) {
    std::vector<float> rgb(width * height * 3);
    for (uint32_t i = 0; i < width * height; i++) {
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

std::vector<uint8_t> float3ToRGBA8(const float* rgb, uint32_t width, uint32_t height, float exposure) {
    std::vector<uint8_t> rgba8(width * height * 4);
    for (uint32_t i = 0; i < width * height; i++) {
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
