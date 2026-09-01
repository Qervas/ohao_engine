// The shared scene: one geometry, one environment, one camera.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/scene.hpp"

#include "probe/oracle_integrator.hpp"

#include <cstddef>

namespace ohao::diff::probe {

// ===========================================================================
// THE SHARED SCENE: one geometry, one environment, one camera
// ===========================================================================
//
// Checks 33-34 (the integrator parity gate) and the Stage 1 Task 2 gradient
// checks below run against the SAME configuration, and it is built HERE, once,
// rather than transcribed into each. Two copies of a test scene are two
// chances for a "quad wound the other way" or a "brightest at the wrong pole"
// to appear in one and not the other, at which point two checks that read as
// though they measure one scene silently measure two.
//
// WHY THIS SCENE. Every piece is placed for a stated reason:
//
//   * FLOOR, y = 0, |x|,|z| <= 8. Every primary ray lands on it: the camera
//     sits at y = 3 looking straight down with tanHalfFov 0.2 at aspect 8, so
//     the extreme ray lands at |x| = 3*0.984375*8*0.2 = 4.725 and
//     |z| = 0.525 -- 3.27 units inside the nearest edge. No primary ray is
//     near a silhouette, which matters because a pixel whose primary ray
//     grazed an edge could hit different triangles under a BVH and under
//     Moller-Trumbore.
//   * OVERHANG, y = 5, |x| <= 1.5. ABOVE the camera and every primary ray
//     travels strictly downward, so it is unreachable by a primary ray by
//     construction -- but it occludes the zenith (the most cosine-weighted
//     part of the hemisphere) for the middle of the image and catches
//     second-bounce rays. This is where most of the visibility signal and
//     most of the interreflection come from, which is what makes the
//     gradient's SECOND and THIRD bounce terms non-negligible rather than a
//     rounding correction on the first.
//   * SIDE WALL, x = 5.5, 0 <= y <= 4. Out of the primary frustum with margin
//     (a ray needs a horizontal slope of 5.5/3 = 1.833 to reach it and the
//     widest is 1.575), and it makes the occlusion vary ASYMMETRICALLY across
//     the image.
//
// Every quad is wound so that wf_intersect.comp's flip-to-oppose-the-ray step
// fires for the rays that actually reach it.
void buildParityScene(std::vector<float>& positions, std::vector<uint32_t>& indices) {
    positions.clear();
    indices.clear();
    // Floor: wound so the geometric normal is -Y, which is what a downward
    // primary ray must have FLIPPED to be shaded correctly.
    parityAddQuad(positions, indices,
                  {{{-8.0f, 0.0f, -8.0f},
                    {8.0f, 0.0f, -8.0f},
                    {8.0f, 0.0f, 8.0f},
                    {-8.0f, 0.0f, 8.0f}}});
    // Overhang at y = 5: wound normal +Y, so a ray arriving from below flips
    // it to -Y.
    parityAddQuad(positions, indices,
                  {{{-1.5f, 5.0f, -8.0f},
                    {-1.5f, 5.0f, 8.0f},
                    {1.5f, 5.0f, 8.0f},
                    {1.5f, 5.0f, -8.0f}}});
    // Side wall at x = 5.5: wound normal +X, so a ray arriving from -X flips
    // it to -X.
    parityAddQuad(positions, indices,
                  {{{5.5f, 0.0f, -8.0f},
                    {5.5f, 4.0f, -8.0f},
                    {5.5f, 4.0f, 8.0f},
                    {5.5f, 0.0f, 8.0f}}});
}

/// The environment both gates use: a smooth, strictly positive, doubly
/// asymmetric gradient (brightest at the +Y pole, where the floor can see it)
/// with a 5:1 contrast. That contrast is a DESIGN CALL: a strongly peaked
/// environment -- checks 29-31 use one with an 8x block -- inflates the
/// variance of the estimators, and the variance is what sets how small a
/// disagreement a gate can resolve. Environment importance sampling, pdfEnvMap
/// and the balance heuristic are all still exercised (the CDF is not uniform
/// in either axis).
///
/// `outRgba` is what EnvCDF::build consumes; `outLum` is the same values as
/// the grey luminance checks 33-34's reference integrator reads. Both come out
/// of ONE loop, so the CDF and the reference cannot see different environments.
void buildParityEnvironment(uint32_t envW, uint32_t envH, std::vector<float>& outRgba,
                            std::vector<double>& outLum) {
    const std::size_t texels = static_cast<std::size_t>(envW) * envH;
    outRgba.assign(texels * 4u, 0.0f);
    outLum.assign(texels, 0.0);
    for (uint32_t y = 0; y < envH; ++y) {
        for (uint32_t x = 0; x < envW; ++x) {
            const double L =
                0.4 + 1.2 * (static_cast<double>(envH - 1u - y) / static_cast<double>(envH - 1u)) +
                0.4 * (static_cast<double>(x) / static_cast<double>(envW - 1u));
            const std::size_t k = static_cast<std::size_t>(y) * envW + x;
            outLum[k] = L;
            outRgba[k * 4u + 0u] = static_cast<float>(L);
            outRgba[k * 4u + 1u] = static_cast<float>(L);
            outRgba[k * 4u + 2u] = static_cast<float>(L);
            outRgba[k * 4u + 3u] = 1.0f;
        }
    }
}

/// The camera both gates use: at y = 3 above the floor's centre, looking
/// straight down, with `up` along -Z so the basis is right-handed.
ohao::diff::WavefrontGenerateCamera parityCamera() {
    ohao::diff::WavefrontGenerateCamera camera;
    camera.origin[0] = 0.0f;
    camera.origin[1] = 3.0f;
    camera.origin[2] = 0.0f;
    camera.forward[0] = 0.0f;
    camera.forward[1] = -1.0f;
    camera.forward[2] = 0.0f;
    camera.right[0] = 1.0f;
    camera.right[1] = 0.0f;
    camera.right[2] = 0.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 0.0f;
    camera.up[2] = -1.0f;
    camera.tanHalfFov = 0.2f;
    return camera;
}

}  // namespace ohao::diff::probe
