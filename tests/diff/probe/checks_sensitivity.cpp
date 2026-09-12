// Spec 10.2, check 73. The argument for why the gate is an identity rather
// than a comparison against a derived expectation is in the header.
#include "probe/checks_sensitivity.hpp"

#include "diff/diff_renderer.hpp"
#include "diff/wavefront/gradient_stages.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "render/rt/env_cdf.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kW = 64u, kH = 8u;
constexpr std::uint32_t kCapacity = kW * kH;
constexpr std::uint32_t kEnvW = 64u, kEnvH = 32u;
constexpr std::uint32_t kBounces = 1u;
constexpr std::uint32_t kSeed = 20260913u;
constexpr float kAlbedo = 0.5f;
constexpr float kEnvRadiance = 1.0f;

// THE CAMERA, and the plate edge that splits the frame. At y = 1 looking
// straight down with right = +X and up = -Z, a pixel's primary ray lands at
// x = ndcX * aspect * tanHalfFov (the camera height divides out; see
// camera_ray.glsl). With aspect 8 and tanHalfFov 0.2 that is 1.6 * ndcX, so
// the frame covers |x| < 1.6 and a plate starting at x = 0.5 leaves roughly
// two thirds of the columns with nothing to hit.
constexpr float kTanHalfFov = 0.2f;
constexpr float kCameraHeight = 1.0f;
constexpr float kPlateMinX = 0.5f;
constexpr float kPlateMaxX = 8.0f;
constexpr float kPlateHalfZ = 8.0f;
// THE CLOSEST COLUMN ON EITHER SIDE must clear the plate's edge by this
// much, or the split below is decided by floating-point luck at the edge
// rather than by geometry. The column pitch in x is 2/kW * 1.6 = 0.05, so
// half a pitch is 0.025 and this is the most any framing can offer.
constexpr double kMinEdgeClearance = 0.02;
// Both sets must be a real part of the frame. A null test over two pixels
// is not a null test.
constexpr std::uint32_t kMinColumnsEachSide = 8u;

// The identity's tolerance: two float summations of the same terms in
// different orders. The map is summed on the host in double; the arena was
// summed on the GPU by contended atomicAdd in float.
constexpr double kSumRelTol = 1e-5;

void buildPlate(std::vector<float>& positions, std::vector<std::uint32_t>& indices) {
    // Wound so the geometric normal is -Y, which wf_intersect.comp flips to
    // +Y for a downward primary ray -- buildParityScene's floor convention.
    positions = {kPlateMinX, 0.0f, -kPlateHalfZ, kPlateMaxX, 0.0f, -kPlateHalfZ,
                 kPlateMaxX, 0.0f, kPlateHalfZ,  kPlateMinX, 0.0f, kPlateHalfZ};
    indices = {0u, 1u, 2u, 0u, 2u, 3u};
}

void buildConstantEnvironment(std::vector<float>& outRgba) {
    outRgba.assign(static_cast<std::size_t>(kEnvW) * kEnvH * 4u, kEnvRadiance);
    for (std::size_t k = 3u; k < outRgba.size(); k += 4u) outRgba[k] = 1.0f;
}

ohao::diff::WavefrontGenerateCamera plateCamera() {
    ohao::diff::WavefrontGenerateCamera camera;
    camera.origin[1] = kCameraHeight;
    camera.forward[0] = 0.0f;
    camera.forward[1] = -1.0f;
    camera.forward[2] = 0.0f;
    camera.right[0] = 1.0f;
    camera.right[1] = 0.0f;
    camera.right[2] = 0.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 0.0f;
    camera.up[2] = -1.0f;
    camera.tanHalfFov = kTanHalfFov;
    return camera;
}

/// Where column `x` of the film lands on the plate's plane.
///
/// camera_ray.glsl's construction, transcribed. DERIVED HERE, before the
/// render, because the whole point of the null test is that the set of
/// pixels that cannot depend on the albedo is known independently of what
/// the map says.
double columnHitX(std::uint32_t column) {
    const double aspect = static_cast<double>(kW) / static_cast<double>(kH);
    const double ndcX =
        2.0 * (static_cast<double>(column) + 0.5) / static_cast<double>(kW) - 1.0;
    return static_cast<double>(kCameraHeight) * ndcX * aspect * static_cast<double>(kTanHalfFov);
}

}  // namespace

bool checkSensitivityMap(ohao::diff::GpuProbeContext& ctx) {
    // --- THE SPLIT, on paper. Which columns can see the plate at all, and
    // by how much the nearest column on each side clears its edge.
    std::vector<bool> columnHits(kW, false);
    std::uint32_t hitColumns = 0u, missColumns = 0u;
    double worstClearance = 1e30;
    for (std::uint32_t c = 0; c < kW; ++c) {
        const double x = columnHitX(c);
        columnHits[c] = (x >= static_cast<double>(kPlateMinX)) &&
                        (x <= static_cast<double>(kPlateMaxX));
        if (columnHits[c]) {
            ++hitColumns;
        } else {
            ++missColumns;
        }
        const double clearance = std::fabs(x - static_cast<double>(kPlateMinX));
        if (clearance < worstClearance) worstClearance = clearance;
    }
    if (hitColumns < kMinColumnsEachSide || missColumns < kMinColumnsEachSide) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- the framing gives %u columns that see "
                     "the plate and %u that do not, against a minimum of %u each. The null test "
                     "needs BOTH sets to be a real part of the frame: with none missing there is "
                     "nothing required to be exactly zero, and with none hitting there is no map "
                     "at all\n",
                     hitColumns, missColumns, kMinColumnsEachSide);
        return false;
    }
    if (!(worstClearance >= kMinEdgeClearance)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- the nearest column clears the plate's "
                     "edge by only %.6g, under the pre-registered %.6g. The split between the "
                     "two sets would then be decided by floating-point luck at the edge rather "
                     "than by geometry, and a pixel on the wrong side of it would look like a "
                     "defect in the map\n",
                     worstClearance, kMinEdgeClearance);
        return false;
    }

    // --- The facade owns the registry and the arena.
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 73 -- init\n");
        return false;
    }
    const ohao::diff::RegisterResult reg = renderer.registerScalarBlock("albedo", 1u);
    if (!reg.ok || !renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 73 -- register/build: %s\n",
                     reg.error.c_str());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = renderer.registry().get(reg.id);
    const std::uint32_t kArenaFloats =
        static_cast<std::uint32_t>(renderer.registry().layout().totalBytes() / sizeof(float));
    const std::uint32_t kOffset = static_cast<std::uint32_t>(
        renderer.registry().layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

    std::vector<float> envRgba;
    buildConstantEnvironment(envRgba);
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    buildPlate(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = plateCamera();

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    ohao::diff::WavefrontBuffers wf;
    if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 73 -- scene setup\n");
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    std::vector<float> film;
    std::vector<float> map;
    ohao::diff::WavefrontGradientOptions options;
    options.diffParam = 0u;  // DIFF_PARAM_BASECOLOR
    // NO ADJOINT SEED. See the header: the map means dpixel/dtheta only when
    // diffAdjointSeed returns 1.
    options.outSensitivity = &map;
    const bool ran = ctx.runWavefrontGradientProbe(
        wf, kW, kH, kBounces, camera, std::span<const float>(positions),
        std::span<const std::uint32_t>(indices), kAlbedo, kMaterial, kSeed, renderer.arena(),
        kArenaFloats, kOffset, film, options);
    const std::vector<float> gradBlock =
        ran ? renderer.arena().readback(ctx.allocator(), albedoParam->gradBlock)
            : std::vector<float>{};
    wf.destroy(ctx.allocator());
    (void)renderer.shutdown(&ctx.allocator());

    if (!ran || gradBlock.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 73 -- render or arena readback\n");
        return false;
    }
    if (map.size() != kCapacity) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- the sensitivity map came back with %zu "
                     "floats, expected %u (one per pixel)\n",
                     map.size(), kCapacity);
        return false;
    }

    // --- THE NULL TEST. Every pixel whose primary ray misses the plate has
    // no vertex in it, so nothing can have been scattered there.
    double sumHit = 0.0, sumMiss = 0.0;
    std::uint32_t nonZeroMisses = 0u, zeroHits = 0u;
    std::size_t firstBadMiss = 0u;
    double firstBadMissValue = 0.0;
    double minHit = 1e30, maxHit = -1e30;
    for (std::uint32_t p = 0; p < kCapacity; ++p) {
        // wf_generate.comp's pixel index is row-major over (width, height).
        const std::uint32_t column = p % kW;
        const double v = static_cast<double>(map[p]);
        if (!std::isfinite(v)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 73 -- map cell %u is %.9g, not finite\n", p,
                         v);
            return false;
        }
        if (columnHits[column]) {
            sumHit += v;
            if (v < minHit) minHit = v;
            if (v > maxHit) maxHit = v;
            // A hit pixel under a constant environment must have received
            // something: with albedo 0.5 and radiance 1 every lit vertex
            // contributes a strictly positive derivative.
            if (!(map[p] > 0.0f)) ++zeroHits;
        } else {
            sumMiss += v;
            // EXACT, compared as a float. Not "small": no vertex ever landed
            // in this pixel, so no atomicAdd ever named it, and the buffer was
            // filled with zero before the dispatch.
            if (map[p] != 0.0f) {
                if (nonZeroMisses == 0u) {
                    firstBadMiss = p;
                    firstBadMissValue = v;
                }
                ++nonZeroMisses;
            }
        }
    }

    if (nonZeroMisses != 0u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- %u of the %u map cells whose primary ray "
                     "MISSES the plate are not exactly zero; the first is cell %zu at %.9g "
                     "(column %zu, which lands at x = %.6g against a plate starting at %.6g).\n"
                     "  The set of missing pixels is derived from the camera basis and the "
                     "plate's edge BEFORE the render, so this is not a disagreement about which "
                     "pixels those are. A nonzero cell there means the map is being indexed by "
                     "something other than the vertex's own pixel -- suspect v.pixelIndex in "
                     "wf_scatter_replay.comp's hook, or a map bound at the wrong size.\n",
                     nonZeroMisses, missColumns * kH, firstBadMiss, firstBadMissValue,
                     firstBadMiss % kW, columnHitX(static_cast<std::uint32_t>(firstBadMiss % kW)),
                     static_cast<double>(kPlateMinX));
        return false;
    }

    // --- NON-VACUITY: the lit half is actually lit. All-zeros passes the
    // null test perfectly.
    if (zeroHits != 0u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- %u of the %u map cells whose primary ray "
                     "HITS the plate are not strictly positive. Under a constant environment of "
                     "radiance %.6g every lit vertex contributes a positive d(film)/d(albedo), so "
                     "a zero there means nothing was scattered -- and an all-zero map satisfies "
                     "the null test above perfectly\n",
                     zeroHits, hitColumns * kH, static_cast<double>(kEnvRadiance));
        return false;
    }

    // --- THE IDENTITY. The map is the arena's scalar, binned by pixel.
    const double arenaGradient = static_cast<double>(gradBlock[0]);
    const double mapSum = sumHit + sumMiss;
    const double rel = std::fabs(mapSum - arenaGradient) / std::fabs(arenaGradient);
    if (!(std::fabs(arenaGradient) > 0.0) || !(rel <= kSumRelTol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 73 -- THE MAP IS NOT THE GRADIENT.\n"
                     "  sum over the map      %.12g\n"
                     "  the arena's scalar    %.12g\n"
                     "  relative difference   %.6g against a tolerance of %.6g\n"
                     "  These are the SAME TERMS: the replay hook computes one scalar per hit "
                     "vertex, adds it to the arena, and adds it again at that vertex's own "
                     "pixel. They can differ only by the order two float summations accumulated "
                     "them in. A larger disagreement means the map is not receiving every term "
                     "the arena does -- look at the guards around the map's atomicAdd first: it "
                     "is deliberately NOT gated on the arena's element-range check, because that "
                     "is a question about a parameter's element and not about a pixel.\n",
                     mapSum, arenaGradient, rel, kSumRelTol);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] check 73 OK -- THE SENSITIVITY MAP IS THE GRADIENT, BINNED BY PIXEL "
        "(spec 10.2). Summing the %ux%u map gives %.9g against the arena's %.9g, a relative "
        "%.3g -- the same terms in a different summation order, which is what makes this gate an "
        "IDENTITY rather than a comparison against a hand-derived number, and what lets the map "
        "inherit check 37's finite-difference gate on the scalar instead of needing an oracle of "
        "its own. AND THE NULL TEST, which the identity cannot see: a map holding the same 1/N "
        "of the total in every cell would satisfy the sum exactly, so the %u columns whose "
        "primary rays MISS the plate -- %u cells -- are required to be EXACTLY 0.0f, compared as "
        "floats. THE MISS SET IS DERIVED FROM THE CAMERA AND THE PLATE'S EDGE BEFORE THE RENDER, "
        "never read off the map, because a check whose expectation comes from the same source as "
        "its measurement cannot fail; the nearest column clears that edge by %.4g. The %u lit "
        "cells run [%.6g, %.6g] and are each separately required to be strictly positive, "
        "without which an all-zero map would pass the null test perfectly.\n",
        kW, kH, mapSum, arenaGradient, rel, missColumns, missColumns * kH, worstClearance,
        hitColumns * kH, minHit, maxHit);
    return true;
}

}  // namespace ohao::diff::probe
