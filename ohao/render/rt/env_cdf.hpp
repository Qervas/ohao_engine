#pragma once

// Env CDF — CPU-side 2D importance-sampling CDF builder for HDR environment maps.

#include "core/concepts.hpp"

#include <span>
#include <vector>

namespace ohao {

class EnvCDF {
public:
    /// pixels: RGBA float, row-major. Expects size >= width * height * 4.
    void build(std::span<const float> pixels, int width, int height);

    void build(const float* pixels, int width, int height) {
        build(std::span<const float>(pixels,
                                     static_cast<size_t>(width) * static_cast<size_t>(height) * 4u),
              width, height);
    }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] float integral() const noexcept { return m_integral; }
    [[nodiscard]] bool valid() const noexcept {
        return m_width > 0 && m_height > 0 && !m_marginalCDF.empty();
    }
    [[nodiscard]] bool empty() const noexcept { return !valid(); }

    [[nodiscard]] const std::vector<float>& marginalCDF() const noexcept { return m_marginalCDF; }
    [[nodiscard]] const std::vector<float>& conditionalCDF() const noexcept { return m_conditionalCDF; }

    [[nodiscard]] std::span<const float> marginalSpan() const noexcept { return m_marginalCDF; }
    [[nodiscard]] std::span<const float> conditionalSpan() const noexcept { return m_conditionalCDF; }

private:
    int m_width = 0;
    int m_height = 0;
    float m_integral = 0.0f;
    std::vector<float> m_marginalCDF;
    std::vector<float> m_conditionalCDF;
};

} // namespace ohao
