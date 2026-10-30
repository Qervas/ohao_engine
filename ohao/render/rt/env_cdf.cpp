// The environment's 2D CDF (env_cdf.hpp), summed in double precision.

#include "render/rt/env_cdf.hpp"

#include <cmath>

namespace ohao {

void EnvCdf::build(std::span<const float> pixels, int width, int height) {
    m_width = width;
    m_height = height;
    const size_t W = static_cast<size_t>(width), H = static_cast<size_t>(height);
    m_data.assign(H + W * H, 0.0f);
    if (W == 0 || H == 0) return;

    constexpr double kPi = 3.14159265358979323846;
    std::vector<double> rowSum(H, 0.0);
    std::vector<double> run(W);
    for (size_t y = 0; y < H; ++y) {
        const double sinTheta = std::sin(kPi * (double(y) + 0.5) / double(H));
        double acc = 0.0;
        for (size_t x = 0; x < W; ++x) {
            const float* c = &pixels[(y * W + x) * 4u];
            const double lum = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
            acc += std::isfinite(lum) && lum > 0.0 ? lum * sinTheta : 0.0;
            run[x] = acc;
        }
        float* row = &m_data[H + y * W];
        for (size_t x = 0; x < W; ++x)
            row[x] = acc > 0.0 ? static_cast<float>(run[x] / acc) : static_cast<float>(double(x + 1) / double(W));
        row[W - 1] = 1.0f;
        rowSum[y] = acc;
    }
    double total = 0.0;
    for (double s : rowSum) total += s;
    double acc = 0.0;
    for (size_t y = 0; y < H; ++y) {
        acc += rowSum[y];
        m_data[y] = total > 0.0 ? static_cast<float>(acc / total) : static_cast<float>(double(y + 1) / double(H));
    }
    m_data[H - 1] = 1.0f;
}

float EnvCdf::probability(int x, int y) const noexcept {
    const std::span<const float> m = marginal(), c = conditional(y);
    const float marg = m[static_cast<size_t>(y)] - (y > 0 ? m[static_cast<size_t>(y) - 1] : 0.0f);
    const float cond = c[static_cast<size_t>(x)] - (x > 0 ? c[static_cast<size_t>(x) - 1] : 0.0f);
    return marg * cond;
}

} // namespace ohao
