#pragma once

// Diff-IR forward: ground-plane UV sample + simple lighting → linear RGB image.

#include "render/diff/diff_camera.hpp"
#include "render/diff/diff_map.hpp"
#include "render/diff/diff_types.hpp"

#include "inverse/image_loss.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ohao::diff {

struct DiffLight {
    glm::vec3 dir{0.4f, 0.85f, 0.25f}; // toward scene
    float key{1.15f};
    float ambient{0.22f};
};

/// Linear RGB beauty (w*h*3). Backdrop is constant cool gray when ray misses ground.
[[nodiscard]] inline std::vector<float> forwardLinear(const DiffAlbedoMap& map,
                                                      const DiffCamera& cam,
                                                      const DiffLight& light,
                                                      std::uint32_t w, std::uint32_t h) {
    std::vector<float> img(static_cast<size_t>(w) * h * 3u, 0.f);
    const glm::vec3 L = glm::normalize(light.dir);
    const float nDotL = std::max(0.f, glm::dot(glm::vec3(0.f, 1.f, 0.f), L));
    const float shade = light.ambient + light.key * nDotL;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            float u, v, wx, wz;
            float r, g, b;
            if (cam.groundHit(static_cast<float>(x), static_cast<float>(y), w, h, u, v, wx, wz)) {
                map.sample(u, v, r, g, b);
                r *= shade;
                g *= shade;
                b *= shade;
            } else {
                // Backdrop-ish
                r = 0.82f * 0.35f;
                g = 0.84f * 0.35f;
                b = 0.88f * 0.35f;
            }
            const size_t o = (static_cast<size_t>(y) * w + x) * 3u;
            img[o + 0] = r;
            img[o + 1] = g;
            img[o + 2] = b;
        }
    }
    return img;
}

[[nodiscard]] inline ohao::inverse::ImageRGBA8 linearToRGBA8(const std::vector<float>& lin,
                                                            std::uint32_t w, std::uint32_t h) {
    ohao::inverse::ImageRGBA8 out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4u);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        auto enc = [](float c) {
            c = std::clamp(c, 0.f, 1.f);
            // mild gamma for display / LDR loss domain
            c = std::pow(c, 1.f / 2.2f);
            return static_cast<std::uint8_t>(c * 255.f + 0.5f);
        };
        out.rgba[i * 4 + 0] = enc(lin[i * 3 + 0]);
        out.rgba[i * 4 + 1] = enc(lin[i * 3 + 1]);
        out.rgba[i * 4 + 2] = enc(lin[i * 3 + 2]);
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

/// MSE loss + analytic ∂L/∂map (nearest scatter) in linear domain vs target LDR.
[[nodiscard]] inline double lossAndGrad(const DiffAlbedoMap& map, const DiffCamera& cam,
                                        const DiffLight& light, std::uint32_t w, std::uint32_t h,
                                        const ohao::inverse::ImageRGBA8& target,
                                        std::vector<float>& gradOut) {
    gradOut.assign(map.rgb.size(), 0.f);
    if (target.width != w || target.height != h || target.rgba.empty()) return 1e6;
    const glm::vec3 L = glm::normalize(light.dir);
    const float nDotL = std::max(0.f, glm::dot(glm::vec3(0.f, 1.f, 0.f), L));
    const float shade = light.ambient + light.key * nDotL;
    double sum = 0.0;
    size_t count = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            float u, v, wx, wz;
            if (!cam.groundHit(static_cast<float>(x), static_cast<float>(y), w, h, u, v, wx, wz))
                continue;
            float ar, ag, ab;
            map.sample(u, v, ar, ag, ab);
            const float pr = ar * shade;
            const float pg = ag * shade;
            const float pb = ab * shade;
            // target as linear-ish (undo gamma)
            const size_t po = (static_cast<size_t>(y) * w + x) * 4u;
            auto toLin = [](std::uint8_t c) {
                return std::pow(static_cast<float>(c) / 255.f, 2.2f);
            };
            const float tr = toLin(target.rgba[po + 0]);
            const float tg = toLin(target.rgba[po + 1]);
            const float tb = toLin(target.rgba[po + 2]);
            const float er = pr - tr;
            const float eg = pg - tg;
            const float eb = pb - tb;
            sum += static_cast<double>(er * er + eg * eg + eb * eb);
            ++count;
            // d(pred)/d(albedo) = shade; dMSE/d(pred) = 2*err → scatter 2*err*shade
            map.scatterGrad(u, v, 2.f * er * shade, 2.f * eg * shade, 2.f * eb * shade, gradOut);
        }
    }
    if (count == 0) return 1e6;
    const double inv = 1.0 / static_cast<double>(count * 3);
    // scale grads to mean
    for (float& g : gradOut) g = static_cast<float>(static_cast<double>(g) * inv);
    return sum * inv;
}

} // namespace ohao::diff
