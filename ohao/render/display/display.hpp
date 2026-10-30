#pragma once

// The display transform on the CPU: shaders/display/display.slang, operation for operation, for
// the frames the CPU finishes (OIDN's). display_test holds the two to the same bits through the
// module's slangc C++ build (tools/display). Change one, change both.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ohao::render {

// What the display curve is: render.settings / render.capture `tonemap`.
enum class DisplayCurve : uint32_t { AgX = 0, Neutral = 1 };

// EV100 (ISO 100) to the linear exposure the display multiplies by: 1 / (1.2 x 2^EV100)
// (Lagarde and de Rousiers 2014).
[[nodiscard]] inline float displayExposure(float ev100) noexcept { return 1.0f / (1.2f * std::exp2(ev100)); }

// The default EV100 of every renderer and capture (frontier F2): about today's deferred 0.6.
inline constexpr float kDefaultEv100 = 0.5f;

namespace display_detail {
struct V3 { float x, y, z; };
inline V3 columns(V3 c0, V3 c1, V3 c2, V3 v) noexcept {
    return {c0.x * v.x + c1.x * v.y + c2.x * v.z, c0.y * v.x + c1.y * v.y + c2.y * v.z,
            c0.z * v.x + c1.z * v.y + c2.z * v.z};
}
inline float saturate(float v) noexcept { return std::min(std::max(v, 0.0f), 1.0f); }
inline float agxContrast(float x) noexcept {
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}
inline constexpr float kAgxMinEv = -12.47393f, kAgxMaxEv = 4.026069f;
inline V3 agx(V3 c) noexcept {
    V3 v = columns({0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f},
                   {0.0784335999999992f, 0.878468636469772f, 0.0784336f},
                   {0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f},
                   {std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f)});
    const auto encode = [](float a) {
        a = std::min(std::max(std::log2(std::max(a, 1e-10f)), kAgxMinEv), kAgxMaxEv);
        return agxContrast((a - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv));
    };
    v = {encode(v.x), encode(v.y), encode(v.z)};
    v = columns({1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f},
                {-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f},
                {-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f}, v);
    return {std::pow(saturate(v.x), 2.2f), std::pow(saturate(v.y), 2.2f), std::pow(saturate(v.z), 2.2f)};
}
inline V3 pbrNeutral(V3 c) noexcept {
    constexpr float startCompression = 0.8f - 0.04f, desaturation = 0.15f;
    c = {std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f)};
    const float x = std::min(c.x, std::min(c.y, c.z));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c = {c.x - offset, c.y - offset, c.z - offset};
    const float peak = std::max(c.x, std::max(c.y, c.z));
    if (peak < startCompression) return c;
    constexpr float d = 1.0f - startCompression;
    const float newPeak = 1.0f - d * d / (peak + d - startCompression);
    const float k = newPeak / peak;
    c = {c.x * k, c.y * k, c.z * k};
    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return {c.x + (newPeak - c.x) * g, c.y + (newPeak - c.y) * g, c.z + (newPeak - c.z) * g};
}
inline float srgbEncode(float c) noexcept {
    c = saturate(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
}  // namespace display_detail

// Scene-linear radiance to sRGB-encoded [0, 1] (the module's displayPixel).
inline void displayPixel(const float in[3], float exposure, DisplayCurve curve, float out[3]) noexcept {
    using namespace display_detail;
    const V3 c{in[0] * exposure, in[1] * exposure, in[2] * exposure};
    const V3 m = curve == DisplayCurve::Neutral ? pbrNeutral(c) : agx(c);
    out[0] = srgbEncode(m.x);
    out[1] = srgbEncode(m.y);
    out[2] = srgbEncode(m.z);
}

// An encoded [0, 1] channel to its 8-bit code, rounded to nearest as a UNORM store rounds.
[[nodiscard]] inline uint8_t displayByte(float v) noexcept {
    return static_cast<uint8_t>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f));
}

}  // namespace ohao::render
