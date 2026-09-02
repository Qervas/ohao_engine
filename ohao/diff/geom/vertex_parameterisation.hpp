// STAGE 3 -- SOMETHING THAT PRODUCES VERTEX POSITIONS.
//
// Spec 9: "Stage 3 parameterizes 'something that produces vertex positions,'
// NEVER vertex positions directly. Direct positions are the trivial case; the
// indirection leaves room for Laplacian preconditioning (Nicolet et al. 2021
// -- close to mandatory for usable geometry optimization) and for
// FlexiCubes-style differentiable iso-surface extraction later."
//
// The Stage 3 plan carried that as a Global Constraint, and the first
// geometry gate (check 60) broke it: it optimised screen-space positions
// directly. This is the indirection that constraint asked for, and the
// smallest one with a JACOBIAN THAT IS NOT THE IDENTITY -- which is the
// property that matters, because an identity Jacobian makes the pullback
// below a copy and tests nothing.
//
// THE PARAMETERISATION. A base shape, a translation and a log-scale:
//
//     positions(theta)_v = exp(s) * base_v + (tx, ty),   theta = (tx, ty, s)
//
// Three parameters for any number of vertices, so the reduction is real: six
// position components become three. LOG-scale rather than scale, because the
// optimiser then works in a space where the parameter is unbounded and a step
// cannot drive the shape through zero into a reflection -- the same reason
// log-space shows up wherever a positive quantity is optimised.
//
// THE PULLBACK is the chain rule, dL/dtheta = J^T (dL/dpositions):
//
//     dL/dtx = SUM_v  dL/dv_x
//     dL/dty = SUM_v  dL/dv_y
//     dL/ds  = SUM_v  exp(s) * (dL/dv_x * base_v_x + dL/dv_y * base_v_y)
//
// Nothing downstream changes: the boundary pass still receives an array of
// positions and still returns dL/d(position). The parameterisation sits
// entirely on the host, between the optimiser and the renderer, which is what
// makes a different one -- Laplacian, iso-surface -- a substitution rather
// than a rewrite.
#pragma once

#include <cstdint>
#include <vector>

namespace ohao::diff {

/// An affine parameterisation of a 2-D shape: translation plus uniform
/// log-scale about the origin.
class AffineVertexParameterisation {
public:
    static constexpr std::size_t kParamCount = 3;  // tx, ty, logScale

    /// `base` is 2 floats per vertex and is NOT a parameter -- it is the
    /// shape the parameters act on, fixed for the life of the optimisation.
    [[nodiscard]] bool setBase(const std::vector<float>& base);

    /// positions(theta). Empty on a malformed base or theta.
    [[nodiscard]] std::vector<float> apply(const std::vector<float>& theta) const;

    /// dL/dtheta from dL/d(positions). `positionGradients` is 2 floats per
    /// vertex, in the order `apply` produced. Returns kParamCount floats.
    [[nodiscard]] std::vector<float> pullback(const std::vector<float>& theta,
                                              const std::vector<float>& positionGradients) const;

    [[nodiscard]] std::size_t vertexCount() const noexcept { return m_base.size() / 2u; }

private:
    std::vector<float> m_base;
};

}  // namespace ohao::diff
