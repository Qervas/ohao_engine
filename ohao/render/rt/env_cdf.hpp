#pragma once

// The environment's importance sampling: a 2D CDF over the equirectangular map's texels, each
// weighted by its luminance times sin(theta) (the texel's share of the sphere). A draw inverts
// the marginal CDF over rows, then the row's conditional CDF: a continuous warp, so the path
// tracer's low-discrepancy points stay stratified on the sphere (an alias table, O(1), measured
// 14% more error at 16 samples for 1.1 ms less a 1080p frame: frontier F1 T6). The path tracer
// (rt/path_tracer.slang, sampleEnv/envPdf) reads both from one device-local buffer (binding 17).

#include <span>
#include <vector>

namespace ohao {

class EnvCdf {
public:
    /// pixels: RGBA float, row-major (row 0 the zenith). Expects size >= width * height * 4.
    /// A black row samples its texels alike (and is never drawn); a black map samples every texel alike.
    void build(std::span<const float> pixels, int width, int height);

    void build(const float* pixels, int width, int height) {
        build(std::span<const float>(pixels,
                                     static_cast<size_t>(width) * static_cast<size_t>(height) * 4u),
              width, height);
    }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] bool valid() const noexcept { return m_width > 0 && m_height > 0 && !m_data.empty(); }

    /// The buffer the shader reads: the marginal CDF over rows (height floats), then each row's
    /// conditional CDF (width floats a row). Every CDF ends at exactly 1.
    [[nodiscard]] const std::vector<float>& data() const noexcept { return m_data; }
    [[nodiscard]] std::span<const float> marginal() const noexcept {
        return std::span<const float>(m_data).first(static_cast<size_t>(m_height));
    }
    [[nodiscard]] std::span<const float> conditional(int row) const noexcept {
        return std::span<const float>(m_data).subspan(
            static_cast<size_t>(m_height) + static_cast<size_t>(row) * static_cast<size_t>(m_width),
            static_cast<size_t>(m_width));
    }
    /// A texel's probability (the product of its row's and its own CDF steps), as the shader computes it.
    [[nodiscard]] float probability(int x, int y) const noexcept;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<float> m_data;
};

} // namespace ohao
