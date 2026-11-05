#pragma once

// shaders/rt/manifold.slang on the CPU (tools/caustics/manifold_kernel.slang, compiled to C++ by slangc): what
// tests/renderer/manifold_tests calls, so the tests hold the path tracer's own solver.

#include <array>

namespace ohao::caustics {

using V3 = std::array<float, 3>;
using V2 = std::array<float, 2>;

struct Tri {
    V3 p0, e1, e2;   // corners: p0, p1 - p0, p2 - p0
    V3 n0, a1, a2;   // vertex normals: n0, n1 - n0, n2 - n0
    V3 c{1.0f, 0.0f, 0.0f};   // the frame's reference (unit)
};

struct MapResult {
    bool ok{false};
    V2 sigma{}, du{}, dv{};
    V3 y{}, n{};
};

MapResult manifoldMap(const Tri& tri, const V3& p, const V3& L, V2 uv);
// False when Newton did not converge; uv is the root otherwise.
bool manifoldSolve(const Tri& tri, const V3& p, const V3& L, V2 target, float tol, V2& uv);
// The drawn slope and its density; the core's disc is |sigma|^2 <= max(slope2 a^2, minSlope2).
V2 manifoldDrawSlope(V2 alphas, float slope2, float minSlope2, const V3& u, float& density);
float manifoldSlopeDensity(V2 alphas, float slope2, float minSlope2, V2 sigma);
bool manifoldTriangleTest(const Tri& tri, const V3& p, const V3& L, V2 sigma);
bool manifoldDiscTest(const V3& boxMin, const V3& boxMax, const V3& axis, float halfAngle, const V3& c, V2 sigma,
                      const V3& p, const V3& L);
// Every root of sigma = target on the triangle, certified by the Krawczyk operator (rt/manifold_cert.slang).
struct CertResult {
    int roots{0}, undecided{0}, boxes{0};
    V2 uv[4]{};
};
CertResult manifoldCertify(const Tri& tri, const V3& p, const V3& L, V2 target, V2 lo = {0, 0}, V2 hi = {0, 0});
// The interval map over the box [lo, hi] (u, v): sigma's and its Jacobian's enclosures; defined 1, 0 (unknown), -1.
struct Enclosure {
    int defined{0};
    V2 sx{}, sy{};               // (lo, hi)
    V2 dsxdu{}, dsxdv{}, dsydu{}, dsydv{};
};
// constraint: F = (A - target.x C, B - target.y C) and its Jacobian instead of sigma's.
Enclosure manifoldEnclose(const Tri& tri, const V3& p, const V3& L, V2 lo, V2 hi, bool constraint = false,
                          V2 target = {});
// Every path sun -> refraction into a solid at t1 -> refraction out at t2 -> X whose slopes are s1 at the entry and s2
// at the exit, certified (rt/manifold_cert.slang's manifoldCertifyPair); eta the solid's relative index, the normals
// outward.
struct PairResult {
    int roots{0}, undecided{0}, boxes{0};
    std::array<float, 4> x[4]{};   // (u1, v1, u2, v2)
};
PairResult manifoldCertifyPair(const Tri& t1, const Tri& t2, const V3& X, const V3& L, float eta, V2 s1, V2 s2);
// Whether a set of entry triangles (box 1, normals within half1 of axis1) and a set of exit triangles can hold a path
// with slopes s1, s2 (conservative; manifoldPairNodeTest).
bool manifoldPairNodeTest(const V3& min1, const V3& max1, const V3& axis1, float half1, const V3& min2, const V3& max2,
                          const V3& axis2, float half2, const V3& X, const V3& L, float eta, V2 s1, V2 s2);
// Its weight for sampling (manifoldPairNodeScore): 0 where it prunes, else in [kPairScoreFloor, 1].
float manifoldPairNodeScore(const V3& min1, const V3& max1, const V3& axis1, float half1, const V3& min2,
                            const V3& max2, const V3& axis2, float half2, const V3& X, const V3& L, float eta, V2 s1,
                            V2 s2);
bool manifoldConeTest(const V3& boxMin, const V3& boxMax, const V3& axis, float halfAngle, const V3& p, const V3& L,
                      float thetaSigma);

} // namespace ohao::caustics
