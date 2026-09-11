// LAPLACIAN PRECONDITIONING (Nicolet, Jacobson & Jakob 2021), as the
// substitution the parameterisation layer was built to accept.
//
// Spec §9 calls this "close to mandatory for usable geometry optimization",
// and `vertex_parameterisation.hpp` says in as many words that a different
// parameterisation -- "Laplacian, iso-surface" -- should be a substitution
// rather than a rewrite. This is that claim being cashed: same `apply` and
// `pullback` pair, nothing downstream changes, and the boundary pass still
// receives an array of positions and still returns dL/d(position).
//
// WHAT PROBLEM IT SOLVES. Gradient descent on vertex positions treats every
// vertex as independent, so the gradient of an image loss -- which is
// concentrated at silhouettes -- moves boundary vertices and leaves interior
// ones behind. The mesh acquires exactly the spikes and self-intersections
// that make unpreconditioned geometry optimisation unusable. The fix is not
// to smooth the RESULT (which fights the objective) but to descend in a
// space where a step is smooth by construction.
//
// THE PARAMETERISATION. With the graph Laplacian L of the mesh and a
// stiffness lambda,
//
//     M = I + lambda * L,        positions = M^-1 u
//
// so the latent variable `u` is what the optimiser moves, and a step in `u`
// spreads across the mesh through M^-1 rather than landing on one vertex.
// lambda = 0 gives M = I and the identity parameterisation -- which is the
// unpreconditioned case, and is what the gate uses as its control.
//
// THE PULLBACK IS THE SAME SOLVE. dL/du = (M^-1)^T dL/dv = M^-1 dL/dv,
// because M is symmetric: L is symmetric for an undirected graph and I is.
// That symmetry is worth stating rather than leaving implicit, since it is
// the reason `apply` and `pullback` share a factorisation and the reason a
// test can check one against the other.
//
// DENSE, AND DELIBERATELY. A Cholesky factorisation of a dense M is O(n^3)
// and the meshes here have tens of vertices, so it is instant and obviously
// correct. Nicolet et al. use a sparse factorisation because their meshes
// have hundreds of thousands; that is a substitution inside this class, and
// the gate above it would not notice.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ohao::diff {

class LaplacianVertexParameterisation {
public:
    /// Build M = I + lambda*L from an undirected edge list. `edges` is two
    /// vertex indices per edge. Rejects an out-of-range index, a self-edge
    /// (which would corrupt the degree), a negative lambda, and a zero
    /// vertex count.
    ///
    /// A DUPLICATED EDGE IS ALSO REJECTED. Listing an edge twice doubles its
    /// off-diagonal and its contribution to both degrees, which is a
    /// different operator and a silent one -- the solve still succeeds and
    /// the mesh is merely stiffer in one place than the caller believes.
    [[nodiscard]] bool build(std::uint32_t vertexCount,
                             const std::vector<std::uint32_t>& edges, double lambda);

    /// positions = M^-1 u, solved per component. `latent` is 2 floats per
    /// vertex and so is the result; empty on a length mismatch.
    [[nodiscard]] std::vector<float> apply(const std::vector<float>& latent) const;

    /// dL/du = M^-1 (dL/dpositions), the same solve, because M is symmetric.
    /// Takes the latent point for interface symmetry with
    /// AffineVertexParameterisation; M does not depend on it, and a comment
    /// says so rather than the parameter quietly going unused.
    [[nodiscard]] std::vector<float> pullback(const std::vector<float>& latent,
                                              const std::vector<float>& positionGradients) const;

    /// The latent value that produces `positions`: u = M v. The forward
    /// multiply, needed to START an optimisation from a known shape.
    [[nodiscard]] std::vector<float> latentFor(const std::vector<float>& positions) const;

    [[nodiscard]] std::uint32_t vertexCount() const noexcept { return m_vertexCount; }
    [[nodiscard]] std::size_t paramCount() const noexcept { return m_vertexCount * 2u; }
    [[nodiscard]] double lambda() const noexcept { return m_lambda; }
    [[nodiscard]] bool valid() const noexcept { return m_vertexCount > 0 && !m_chol.empty(); }

private:
    /// Solve M x = b for one component using the stored factorisation.
    void solveInPlace(std::vector<double>& x) const;

    std::uint32_t m_vertexCount{0};
    double m_lambda{0.0};
    std::vector<double> m_m;     ///< M, row-major, n*n. Kept for latentFor.
    std::vector<double> m_chol;  ///< Lower Cholesky factor of M, row-major.
};

}  // namespace ohao::diff
