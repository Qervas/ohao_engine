#include "render/rt/env_sun.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ohao::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float luminance(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

}  // namespace

EnvSun separateEnvSun(std::span<float> rgba, int width, int height) {
    EnvSun sun;
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4u) return sun;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);

    std::vector<float> lum(n);
    size_t peakIdx = 0;
    for (size_t i = 0; i < n; ++i) {
        lum[i] = luminance(&rgba[i * 4]);
        if (lum[i] > lum[peakIdx]) peakIdx = i;
    }
    std::vector<float> sorted(lum);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
    sun.median = sorted[n / 2];
    sun.peak = lum[peakIdx];
    if (!(sun.median > 0.0f) || sun.peak < kSunPeakOverMedian * sun.median) return sun;

    // Solid angle of a texel in row y, and its direction (row 0 is the zenith: elevation +pi/2).
    const auto rowElevation = [&](int y) {
        return (0.5f - (static_cast<float>(y) + 0.5f) / static_cast<float>(height)) * kPi;
    };
    const auto rowSolidAngle = [&](int y) {
        return (2.0f * kPi / static_cast<float>(width)) * (kPi / static_cast<float>(height)) * std::cos(rowElevation(y));
    };
    const auto texelDir = [&](int x, int y) {
        const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 0.5f) * 2.0f * kPi;
        const float elevation = rowElevation(y);
        return glm::vec3(std::cos(elevation) * std::cos(phi), std::sin(elevation), std::cos(elevation) * std::sin(phi));
    };

    // The sun: the peak's 8-connected region over the threshold (x wraps; y does not).
    const float threshold = kSunTexelOverMedian * sun.median;
    std::vector<uint8_t> inSun(n, 0);
    std::vector<size_t> sunTexels{peakIdx};
    inSun[peakIdx] = 1;
    for (size_t k = 0; k < sunTexels.size(); ++k) {
        const int x = static_cast<int>(sunTexels[k] % width), y = static_cast<int>(sunTexels[k] / width);
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= height) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const size_t j = static_cast<size_t>(ny) * width + static_cast<size_t>((x + dx + width) % width);
                if (!inSun[j] && lum[j] > threshold) {
                    inSun[j] = 1;
                    sunTexels.push_back(j);
                }
            }
        }
    }
    double solidAngle = 0.0;
    for (const size_t i : sunTexels) solidAngle += rowSolidAngle(static_cast<int>(i / width));
    if (solidAngle > kSunMaxSolidAngle) return sun;

    // The sky around it: the mean of the texels bordering the region.
    glm::dvec3 ringSum(0.0);
    size_t ringCount = 0;
    std::vector<uint8_t> inRing(n, 0);
    for (const size_t i : sunTexels) {
        const int x = static_cast<int>(i % width), y = static_cast<int>(i / width);
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= height) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const size_t j = static_cast<size_t>(ny) * width + static_cast<size_t>((x + dx + width) % width);
                if (inSun[j] || inRing[j]) continue;
                inRing[j] = 1;
                ringSum += glm::dvec3(rgba[j * 4], rgba[j * 4 + 1], rgba[j * 4 + 2]);
                ++ringCount;
            }
        }
    }
    if (ringCount == 0) return sun;
    const glm::vec3 fill(ringSum / static_cast<double>(ringCount));
    const float fillLum = 0.2126f * fill.r + 0.7152f * fill.g + 0.0722f * fill.b;

    double totalPower = 0.0;
    for (int y = 0; y < height; ++y) {
        const double dOmega = rowSolidAngle(y);
        for (int x = 0; x < width; ++x) totalPower += lum[static_cast<size_t>(y) * width + x] * dOmega;
    }
    glm::dvec3 irradiance(0.0), weightedDir(0.0);
    double removedPower = 0.0;
    for (const size_t i : sunTexels) {
        const int x = static_cast<int>(i % width), y = static_cast<int>(i / width);
        const double dOmega = rowSolidAngle(y);
        float* p = &rgba[i * 4];
        irradiance += glm::dvec3(p[0] - fill.r, p[1] - fill.g, p[2] - fill.b) * dOmega;
        const double w = (lum[i] - fillLum) * dOmega;
        removedPower += w;
        weightedDir += glm::dvec3(texelDir(x, y)) * w;
        p[0] = fill.r;
        p[1] = fill.g;
        p[2] = fill.b;
    }

    sun.found = true;
    sun.texels = static_cast<uint32_t>(sunTexels.size());
    sun.solidAngle = static_cast<float>(solidAngle);
    sun.irradiance = glm::vec3(irradiance);
    sun.direction = glm::length(weightedDir) > 0.0 ? glm::vec3(glm::normalize(weightedDir)) : texelDir(
        static_cast<int>(peakIdx % width), static_cast<int>(peakIdx / width));
    sun.powerShare = totalPower > 0.0 ? static_cast<float>(removedPower / totalPower) : 0.0f;
    return sun;
}

}  // namespace ohao::render
