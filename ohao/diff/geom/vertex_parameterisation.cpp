#include "diff/geom/vertex_parameterisation.hpp"

#include <cmath>
#include <cstddef>

namespace ohao::diff {

bool AffineVertexParameterisation::setBase(const std::vector<float>& base) {
    if (base.empty() || base.size() % 2u != 0u) {
        m_base.clear();
        return false;
    }
    m_base = base;
    return true;
}

std::vector<float> AffineVertexParameterisation::apply(const std::vector<float>& theta) const {
    if (m_base.empty() || theta.size() != kParamCount) return {};
    const double scale = std::exp(static_cast<double>(theta[2]));
    std::vector<float> out(m_base.size(), 0.0f);
    for (std::size_t v = 0; v < m_base.size() / 2u; ++v) {
        out[v * 2u + 0u] = static_cast<float>(scale * static_cast<double>(m_base[v * 2u + 0u]) +
                                              static_cast<double>(theta[0]));
        out[v * 2u + 1u] = static_cast<float>(scale * static_cast<double>(m_base[v * 2u + 1u]) +
                                              static_cast<double>(theta[1]));
    }
    return out;
}

std::vector<float> AffineVertexParameterisation::pullback(
    const std::vector<float>& theta, const std::vector<float>& positionGradients) const {
    if (m_base.empty() || theta.size() != kParamCount ||
        positionGradients.size() != m_base.size()) {
        return {};
    }
    const double scale = std::exp(static_cast<double>(theta[2]));
    // Accumulated in DOUBLE. The three sums run over every vertex, and a
    // translation's gradient is the sum of many similar terms -- exactly the
    // shape where float32 accumulation loses the low bits that distinguish
    // one step from the next.
    double dtx = 0.0;
    double dty = 0.0;
    double ds = 0.0;
    for (std::size_t v = 0; v < m_base.size() / 2u; ++v) {
        const double gx = static_cast<double>(positionGradients[v * 2u + 0u]);
        const double gy = static_cast<double>(positionGradients[v * 2u + 1u]);
        // d(position_v)/d(tx) is (1, 0) for EVERY vertex, so the translation
        // gradient is the plain sum. That is the part of the Jacobian that is
        // constant; the scale's is not.
        dtx += gx;
        dty += gy;
        // d(position_v)/d(s) = exp(s) * base_v, by the derivative of the
        // exponential -- which is why this term carries `scale` and the two
        // above do not.
        ds += scale * (gx * static_cast<double>(m_base[v * 2u + 0u]) +
                       gy * static_cast<double>(m_base[v * 2u + 1u]));
    }
    return {static_cast<float>(dtx), static_cast<float>(dty), static_cast<float>(ds)};
}

}  // namespace ohao::diff
