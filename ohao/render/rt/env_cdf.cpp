// Env CDF — construction of marginal + conditional CDFs from an HDR env map.
// See env_cdf.hpp for usage and mathematical notes.

#include "render/rt/env_cdf.hpp"
#include <cmath>

namespace ohao {

static float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

void EnvCDF::build(std::span<const float> pixels, int width, int height) {
    m_width = width;
    m_height = height;
    m_conditionalCDF.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);
    m_marginalCDF.assign(static_cast<size_t>(height), 0.0f);

    // Per row: luminance weighted by sin(theta), build per-row conditional CDF
    std::vector<float> rowSum(static_cast<size_t>(height), 0.0f);
    constexpr float kPi = 3.14159265358979323846f;

    for (int y = 0; y < height; y++) {
        float theta = kPi * (float(y) + 0.5f) / float(height);
        float sinTheta = std::sin(theta);

        float rowAccum = 0.0f;
        for (int x = 0; x < width; x++) {
            const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            float w = luminance(pixels[base], pixels[base + 1], pixels[base + 2]) * sinTheta;
            rowAccum += w;
            m_conditionalCDF[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = rowAccum;
        }
        // Normalize row CDF
        if (rowAccum > 0.0f) {
            for (int x = 0; x < width; x++) {
                m_conditionalCDF[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] /= rowAccum;
            }
        } else {
            // Degenerate black row — fall back to uniform
            for (int x = 0; x < width; x++) {
                m_conditionalCDF[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                    float(x + 1) / float(width);
            }
        }
        rowSum[static_cast<size_t>(y)] = rowAccum;
    }

    // Marginal CDF over rows
    float total = 0.0f;
    for (int y = 0; y < height; y++) {
        total += rowSum[static_cast<size_t>(y)];
        m_marginalCDF[static_cast<size_t>(y)] = total;
    }
    m_integral = total;
    if (total > 0.0f) {
        for (int y = 0; y < height; y++) m_marginalCDF[static_cast<size_t>(y)] /= total;
    } else {
        for (int y = 0; y < height; y++) m_marginalCDF[static_cast<size_t>(y)] = float(y + 1) / float(height);
    }
}

} // namespace ohao
