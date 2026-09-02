// STAGE 3 TASK 3 -- THE BOUNDARY INTEGRAND, at one straight edge.
//
// Spec 4.1 splits the derivative in two:
//
//   dI/dtheta = INTEGRAL_interior (df/dtheta) dx + INTEGRAL_boundary f (v.n) dl
//
// Stages 1 and 2 built the first term. This is the second, in the smallest
// setting where it has a closed form: a straight edge crossing a pixel under
// a box filter, with constant radiance on each side.
//
// THE DERIVATION, stated because the code is a transcription of it.
//
// The pixel is the unit square and the edge is the segment p0->p1 clipped to
// it. Radiance is L_in on the negative side of the line's normal and L_out on
// the positive side, so
//
//     I(theta) = L_in * A(theta) + L_out * (1 - A(theta))
//     dI/dtheta = (L_in - L_out) * dA/dtheta
//
// and the area's rate of change is the classic sweep: a boundary moving with
// normal speed (v.n) sweeps area at (v.n) per unit length,
//
//     dA/dtheta = INTEGRAL_chord (v . n) dl.
//
// WHY THE PARAMETER IS AN ENDPOINT AND NOT THE LINE'S OFFSET. Translating the
// whole line gives (v.n) = 1 everywhere, so the integral collapses to the
// chord length and the "estimator" is the closed form -- a check that cannot
// fail. Moving ONE ENDPOINT makes the velocity BARYCENTRIC: a point at
// fraction u along p0->p1 moves at (1-u)*d when p0 moves at d. That (1-u) is
// not incidental -- it IS the weight with which the edge's gradient scatters
// to its two vertices (spec 7.2), so the case that makes the check
// non-trivial is also the case Stage 3 actually needs.
//
// With the chord occupying u in [uA, uB] of the full segment of length
// `len`, and n the line's unit normal:
//
//     dA/d(p0 along d) = (d.n) * len * [ (uB - uA) - (uB^2 - uA^2)/2 ]
//
// THE ORACLE SHARES NONE OF THAT. The unit tests compare it against a finite
// difference on the AREA, computed by supersampling the pixel and counting
// points on each side of the line -- no chord, no normal, no barycentric
// weight, no calculus. Two computations of one quantity with nothing in
// common but the answer.
#pragma once

#include <cstdint>

namespace ohao::diff {

/// A straight edge crossing the unit-square pixel [0,1]^2.
struct PixelEdge {
    float p0[2]{};
    float p1[2]{};
};

/// The clipped chord, as fractions along p0->p1. `uA < uB` when the edge
/// crosses the pixel at all.
struct EdgeChord {
    double uA{0.0};
    double uB{0.0};
    bool crosses{false};
};

/// Clip `edge` to the unit square, in the segment's own parameter.
[[nodiscard]] EdgeChord clipEdgeToPixel(const PixelEdge& edge);

/// The fraction of the unit square on the NEGATIVE side of the edge's normal,
/// by supersampling on an n x n grid. This is the oracle's primitive: it
/// knows only "which side of a line is this point on", and nothing about
/// chords, normals or derivatives.
[[nodiscard]] double areaInsideBySupersampling(const PixelEdge& edge, std::uint32_t n);

/// dI/dtheta for theta = moving p0 along `d`, from the closed form above.
[[nodiscard]] double boundaryTermMovingP0(const PixelEdge& edge, const float d[2], double lIn,
                                          double lOut);

/// The same, for the OTHER endpoint. A point at fraction u along p0->p1
/// moves at u*d when p1 moves at d, so the weight is u where p0's is (1-u).
///
/// THE PAIR CARRIES A CONSERVATION IDENTITY, and it is the reason both exist
/// as separate functions rather than one with a flag: (1-u) + u = 1
/// identically, so moving BOTH endpoints by the same d -- a rigid
/// translation of the edge, with velocity d everywhere -- must give exactly
/// the sum of the two. That is checkable without knowing either weight, and
/// it is the analogue of check 44's bilinear conservation identity: the
/// scatter distributes the edge's contribution over its two vertices and
/// must neither create nor destroy any of it.
[[nodiscard]] double boundaryTermMovingP1(const PixelEdge& edge, const float d[2], double lIn,
                                          double lOut);

/// The boundary term for translating the WHOLE edge by `d`. The identity
/// above says this equals movingP0 + movingP1.
[[nodiscard]] double boundaryTermTranslating(const PixelEdge& edge, const float d[2],
                                             double lIn, double lOut);

/// The same quantity by SAMPLING the chord -- the form the GPU pass will
/// take (spec 7.2: each sample evaluates both sides and weights by the
/// edge's velocity). Converges to `boundaryTermMovingP0` as `samples` grows,
/// and is a separate computation of it rather than a rearrangement.
[[nodiscard]] double boundaryTermMovingP0Sampled(const PixelEdge& edge, const float d[2],
                                                 double lIn, double lOut,
                                                 std::uint32_t samples);

}  // namespace ohao::diff
