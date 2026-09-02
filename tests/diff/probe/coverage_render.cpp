#include "probe/coverage_render.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool coverageInsideTriangle(const std::vector<float>& tri, double px, double py) {
    if (tri.size() < 6u) return false;
    auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    const double s0 = cross(tri[2] - tri[0], tri[3] - tri[1], px - tri[0], py - tri[1]);
    const double s1 = cross(tri[4] - tri[2], tri[5] - tri[3], px - tri[2], py - tri[3]);
    const double s2 = cross(tri[0] - tri[4], tri[1] - tri[5], px - tri[4], py - tri[5]);
    // Either winding. The parameterisation of check 62 cannot flip the
    // winding (exp is positive), but nothing here needs to assume that.
    return (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
}

std::vector<float> renderTriangleCoverage(const std::vector<float>& tri, std::uint32_t image,
                                          std::uint32_t sub, double lIn, double lOut) {
    std::vector<float> out(static_cast<std::size_t>(image) * image, 0.0f);
    if (tri.size() < 6u || image == 0u || sub == 0u) return out;
    const double invSub = 1.0 / static_cast<double>(sub);
    const double invSamples = 1.0 / (static_cast<double>(sub) * static_cast<double>(sub));
    for (std::uint32_t py = 0; py < image; ++py) {
        for (std::uint32_t px = 0; px < image; ++px) {
            std::uint32_t in = 0;
            for (std::uint32_t sy = 0; sy < sub; ++sy) {
                for (std::uint32_t sx = 0; sx < sub; ++sx) {
                    if (coverageInsideTriangle(tri, px + (sx + 0.5) * invSub,
                                               py + (sy + 0.5) * invSub)) {
                        ++in;
                    }
                }
            }
            const double cov = static_cast<double>(in) * invSamples;
            out[static_cast<std::size_t>(py) * image + px] =
                static_cast<float>(cov * lIn + (1.0 - cov) * lOut);
        }
    }
    return out;
}

}  // namespace ohao::diff::probe
