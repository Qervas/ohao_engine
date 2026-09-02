#include "diff/geom/boundary_integrand.hpp"

#include <algorithm>
#include <cmath>

namespace ohao::diff {
namespace {

struct Line {
    double nx, ny;  // unit normal
    double len;     // |p1 - p0|
    double dx, dy;  // p1 - p0
};

Line lineOf(const PixelEdge& e) {
    const double dx = static_cast<double>(e.p1[0]) - static_cast<double>(e.p0[0]);
    const double dy = static_cast<double>(e.p1[1]) - static_cast<double>(e.p0[1]);
    const double len = std::sqrt(dx * dx + dy * dy);
    // The normal is the direction rotated by +90 degrees. Which of the two
    // normals is chosen is a CONVENTION, and it is the same one
    // `areaInsideBySupersampling` uses below -- "inside" means the negative
    // side of THIS normal. The two must agree or the oracle measures the
    // complement of what the closed form does, and the comparison would be
    // off by a sign that looks like a derivative error.
    return {len > 0.0 ? -dy / len : 0.0, len > 0.0 ? dx / len : 0.0, len, dx, dy};
}

double signedDistance(const Line& l, const PixelEdge& e, double x, double y) {
    return l.nx * (x - static_cast<double>(e.p0[0])) + l.ny * (y - static_cast<double>(e.p0[1]));
}

}  // namespace

EdgeChord clipEdgeToPixel(const PixelEdge& edge) {
    // Liang-Barsky against [0,1]^2, in the segment's own parameter.
    const double x0 = edge.p0[0], y0 = edge.p0[1];
    const double dx = static_cast<double>(edge.p1[0]) - x0;
    const double dy = static_cast<double>(edge.p1[1]) - y0;
    double uA = 0.0, uB = 1.0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0 - 0.0, 1.0 - x0, y0 - 0.0, 1.0 - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return {0.0, 0.0, false};  // parallel and outside
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0) {
            uA = std::max(uA, t);
        } else {
            uB = std::min(uB, t);
        }
    }
    if (uA >= uB) return {0.0, 0.0, false};
    return {uA, uB, true};
}

double areaInsideBySupersampling(const PixelEdge& edge, std::uint32_t n) {
    if (n == 0u) return 0.0;
    const Line l = lineOf(edge);
    if (l.len <= 0.0) return 0.0;
    std::uint64_t inside = 0;
    for (std::uint32_t j = 0; j < n; ++j) {
        // Cell CENTRES, so no sample ever lands exactly on a pixel boundary.
        const double y = (static_cast<double>(j) + 0.5) / static_cast<double>(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            const double x = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
            if (signedDistance(l, edge, x, y) < 0.0) ++inside;
        }
    }
    return static_cast<double>(inside) / (static_cast<double>(n) * static_cast<double>(n));
}

double boundaryTermMovingP0(const PixelEdge& edge, const float d[2], double lIn, double lOut) {
    const Line l = lineOf(edge);
    if (l.len <= 0.0) return 0.0;
    const EdgeChord chord = clipEdgeToPixel(edge);
    if (!chord.crosses) return 0.0;

    const double dDotN = static_cast<double>(d[0]) * l.nx + static_cast<double>(d[1]) * l.ny;
    // INTEGRAL over the chord of (1-u), in ARC LENGTH: du * len.
    const double weight = (chord.uB - chord.uA) -
                          0.5 * (chord.uB * chord.uB - chord.uA * chord.uA);
    // dA/dtheta, then the jump.
    //
    // THE SIGN WAS WRONG HERE AND THE ORACLE IS WHAT FOUND IT. An earlier
    // version negated this, reasoning that "a positive (d.n) grows the
    // POSITIVE side, so it shrinks the inside". That is backwards: moving the
    // line along +n sweeps the BOUNDARY into the positive region, so the
    // negative side -- the inside -- GROWS. dA/dtheta is therefore positive
    // for a positive (d.n).
    //
    // Worth recording HOW it was found. The sampled estimator below agreed
    // with this function perfectly at every sample count, because both
    // carried the same minus; the test comparing them could not have caught
    // it. Only the supersampled area difference, which shares no formula with
    // either, disagreed -- by exactly a factor of -1, with the magnitudes
    // matching to 9e-5.
    const double dArea = dDotN * l.len * weight;
    return (lIn - lOut) * dArea;
}

double boundaryTermMovingP1(const PixelEdge& edge, const float d[2], double lIn,
                            double lOut) {
    const Line l = lineOf(edge);
    if (l.len <= 0.0) return 0.0;
    const EdgeChord chord = clipEdgeToPixel(edge);
    if (!chord.crosses) return 0.0;
    const double dDotN = static_cast<double>(d[0]) * l.nx + static_cast<double>(d[1]) * l.ny;
    // INTEGRAL of u over the chord, in arc length. p0's is the same integral
    // of (1-u); the two sum to (uB - uA), which is the rigid case.
    const double weight = 0.5 * (chord.uB * chord.uB - chord.uA * chord.uA);
    return (lIn - lOut) * dDotN * l.len * weight;
}

double boundaryTermTranslating(const PixelEdge& edge, const float d[2], double lIn,
                               double lOut) {
    const Line l = lineOf(edge);
    if (l.len <= 0.0) return 0.0;
    const EdgeChord chord = clipEdgeToPixel(edge);
    if (!chord.crosses) return 0.0;
    const double dDotN = static_cast<double>(d[0]) * l.nx + static_cast<double>(d[1]) * l.ny;
    // Velocity d everywhere, so the weight is the chord's own length in the
    // segment's parameter. Written INDEPENDENTLY of the two above rather
    // than as their sum -- a conservation identity checked against the sum of
    // its own parts is not a check.
    const double weight = chord.uB - chord.uA;
    return (lIn - lOut) * dDotN * l.len * weight;
}

double boundaryTermMovingP0Sampled(const PixelEdge& edge, const float d[2], double lIn,
                                   double lOut, std::uint32_t samples) {
    if (samples == 0u) return 0.0;
    const Line l = lineOf(edge);
    if (l.len <= 0.0) return 0.0;
    const EdgeChord chord = clipEdgeToPixel(edge);
    if (!chord.crosses) return 0.0;

    const double dDotN = static_cast<double>(d[0]) * l.nx + static_cast<double>(d[1]) * l.ny;
    const double chordLen = (chord.uB - chord.uA) * l.len;
    // Midpoint rule along the chord. Each sample stands for chordLen/samples
    // of arc, carries the jump (spec 7.2's "both sides", which here is the
    // constant difference), and is weighted by that point's own velocity.
    double total = 0.0;
    for (std::uint32_t s = 0; s < samples; ++s) {
        const double frac = (static_cast<double>(s) + 0.5) / static_cast<double>(samples);
        const double u = chord.uA + frac * (chord.uB - chord.uA);
        const double vDotN = (1.0 - u) * dDotN;
        total += (lIn - lOut) * vDotN;
    }
    return total * chordLen / static_cast<double>(samples);
}

}  // namespace ohao::diff
