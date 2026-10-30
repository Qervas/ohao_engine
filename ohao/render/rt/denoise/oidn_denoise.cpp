#include "render/rt/denoise/oidn_denoise.hpp"
#if defined(OHAO_OIDN_ENABLED)
#include <OpenImageDenoise/oidn.hpp>
#endif
#include <algorithm>
#include <cmath>
#include "core/log.hpp"

namespace ohao {

bool oidnDenoise(std::span<float> beauty, std::span<const float> albedo,
                 std::span<const float> normal, uint32_t width, uint32_t height, bool hdr) {
#if !defined(OHAO_OIDN_ENABLED)
    (void)beauty;
    (void)albedo;
    (void)normal;
    (void)width;
    (void)height;
    (void)hdr;
    OHAO_LOG(Warn, render, "OIDN: built without OpenImageDenoise — skipping");
    return false;
#else
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (beauty.size() < pixelCount * 3u) {
        OHAO_LOG(Error, render, "OIDN: beauty buffer too small");
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
        OHAO_LOG(Error, render, "OIDN: error: {}", errorMessage ? errorMessage : "(no message)");
        return false;
    }

    OHAO_LOG(Debug, render, "OIDN: denoised {}x{}", width, height);
    return true;
#endif
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

std::vector<uint8_t> float3ToRGBA8(std::span<const float> rgb, uint32_t width, uint32_t height,
                                   float exposure, render::DisplayCurve curve) {
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba8(pixelCount * 4u);
    for (size_t i = 0; i < pixelCount; i++) {
        float out[3];
        render::displayPixel(&rgb[i * 3], exposure, curve, out);
        rgba8[i * 4 + 0] = render::displayByte(out[0]);
        rgba8[i * 4 + 1] = render::displayByte(out[1]);
        rgba8[i * 4 + 2] = render::displayByte(out[2]);
        rgba8[i * 4 + 3] = 255;
    }
    return rgba8;
}

} // namespace ohao
