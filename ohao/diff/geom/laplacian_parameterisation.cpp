#include "diff/geom/laplacian_parameterisation.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace ohao::diff {

bool LaplacianVertexParameterisation::build(std::uint32_t vertexCount,
                                            const std::vector<std::uint32_t>& edges,
                                            double lambda) {
    m_vertexCount = 0;
    m_m.clear();
    m_chol.clear();
    if (vertexCount == 0u) return false;
    if (edges.size() % 2u != 0u) return false;
    if (!(lambda >= 0.0) || !std::isfinite(lambda)) return false;

    const std::size_t n = vertexCount;
    std::vector<double> m(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) m[i * n + i] = 1.0;  // the I of I + lambda*L

    for (std::size_t e = 0; e < edges.size() / 2u; ++e) {
        const std::uint32_t a = edges[e * 2u + 0u];
        const std::uint32_t b = edges[e * 2u + 1u];
        if (a >= vertexCount || b >= vertexCount) return false;
        // A self-edge would add 2*lambda to a diagonal that L says should get
        // nothing, so it is a caller error rather than a no-op.
        if (a == b) return false;
        // Already connected: a duplicate would double this edge's weight
        // silently. Checked on the off-diagonal, which is zero until this
        // edge writes it.
        if (m[static_cast<std::size_t>(a) * n + b] != 0.0) return false;

        m[static_cast<std::size_t>(a) * n + a] += lambda;
        m[static_cast<std::size_t>(b) * n + b] += lambda;
        m[static_cast<std::size_t>(a) * n + b] -= lambda;
        m[static_cast<std::size_t>(b) * n + a] -= lambda;
    }

    // SYMMETRY, ASSERTED RATHER THAN ASSUMED. The factorisation below reads
    // only the LOWER triangle of m, while latentFor multiplies by the whole
    // matrix -- so an asymmetric M would leave the solve and the forward
    // multiply quietly disagreeing, and nothing above would notice. The loop
    // that builds M writes both halves together today; this is what stops a
    // later edit to one of those lines from being silent. Found by mutating
    // exactly that: halving one off-diagonal broke latentFor and left the
    // finite-difference oracle green, because the oracle differences a solve
    // that never reads the corrupted entry.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (m[i * n + j] != m[j * n + i]) return false;
        }
    }

    // Cholesky, M = L L^T. M is symmetric positive definite for lambda >= 0:
    // L is positive semi-definite (it is a graph Laplacian) and the I makes
    // it strictly positive, so the factorisation cannot hit a non-positive
    // pivot unless something above is wrong -- which is why the check below
    // returns false rather than clamping.
    std::vector<double> chol(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double sum = m[i * n + j];
            for (std::size_t k = 0; k < j; ++k) sum -= chol[i * n + k] * chol[j * n + k];
            if (i == j) {
                if (!(sum > 0.0) || !std::isfinite(sum)) return false;
                chol[i * n + j] = std::sqrt(sum);
            } else {
                chol[i * n + j] = sum / chol[j * n + j];
            }
        }
    }

    m_vertexCount = vertexCount;
    m_lambda = lambda;
    m_m = std::move(m);
    m_chol = std::move(chol);
    return true;
}

void LaplacianVertexParameterisation::solveInPlace(std::vector<double>& x) const {
    const std::size_t n = m_vertexCount;
    // Forward substitution, L y = b.
    for (std::size_t i = 0; i < n; ++i) {
        double sum = x[i];
        for (std::size_t k = 0; k < i; ++k) sum -= m_chol[i * n + k] * x[k];
        x[i] = sum / m_chol[i * n + i];
    }
    // Back substitution, L^T x = y.
    for (std::size_t ii = n; ii > 0; --ii) {
        const std::size_t i = ii - 1;
        double sum = x[i];
        for (std::size_t k = i + 1; k < n; ++k) sum -= m_chol[k * n + i] * x[k];
        x[i] = sum / m_chol[i * n + i];
    }
}

std::vector<float> LaplacianVertexParameterisation::apply(
    const std::vector<float>& latent) const {
    if (!valid() || latent.size() != paramCount()) return {};
    const std::size_t n = m_vertexCount;
    std::vector<float> out(latent.size(), 0.0f);
    // x and y are independent: M acts on the graph, not on the components.
    for (std::size_t c = 0; c < 2u; ++c) {
        std::vector<double> col(n, 0.0);
        for (std::size_t v = 0; v < n; ++v) col[v] = latent[v * 2u + c];
        solveInPlace(col);
        for (std::size_t v = 0; v < n; ++v) out[v * 2u + c] = static_cast<float>(col[v]);
    }
    return out;
}

std::vector<float> LaplacianVertexParameterisation::pullback(
    const std::vector<float>& latent, const std::vector<float>& positionGradients) const {
    // `latent` is unused on purpose: M = I + lambda*L depends on the mesh's
    // TOPOLOGY, not on where its vertices currently are, so the Jacobian is
    // constant and the pullback is the same solve everywhere. The parameter
    // stays for interface symmetry with AffineVertexParameterisation, whose
    // Jacobian does depend on its argument.
    (void)latent;
    if (!valid() || positionGradients.size() != paramCount()) return {};
    // dL/du = (dv/du)^T dL/dv = (M^-1)^T dL/dv = M^-1 dL/dv, by symmetry.
    return apply(positionGradients);
}

std::vector<float> LaplacianVertexParameterisation::latentFor(
    const std::vector<float>& positions) const {
    if (!valid() || positions.size() != paramCount()) return {};
    const std::size_t n = m_vertexCount;
    std::vector<float> out(positions.size(), 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        double sx = 0.0;
        double sy = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double mij = m_m[i * n + j];
            if (mij == 0.0) continue;
            sx += mij * static_cast<double>(positions[j * 2u + 0u]);
            sy += mij * static_cast<double>(positions[j * 2u + 1u]);
        }
        out[i * 2u + 0u] = static_cast<float>(sx);
        out[i * 2u + 1u] = static_cast<float>(sy);
    }
    return out;
}

}  // namespace ohao::diff
