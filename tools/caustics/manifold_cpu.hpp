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
};

struct MapResult {
    bool ok{false};
    V2 sigma{}, du{}, dv{};
    V3 y{}, n{};
};

MapResult manifoldMap(const Tri& tri, const V3& p, const V3& L, V2 uv);
// False when Newton did not converge; uv is the root otherwise.
bool manifoldSolve(const Tri& tri, const V3& p, const V3& L, V2 target, float tol, V2& uv);
// The drawn slope and its density.
V2 manifoldDrawSlope(V2 alphas, float slope2, const V3& u, float& density);
float manifoldSlopeDensity(V2 alphas, float slope2, V2 sigma);
bool manifoldConeTest(const V3& boxMin, const V3& boxMax, const V3& axis, float halfAngle, const V3& p, const V3& L,
                      float thetaSigma);

} // namespace ohao::caustics
