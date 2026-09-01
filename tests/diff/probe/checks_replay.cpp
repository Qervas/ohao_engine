// Replay equivalence, checks 35-36: the forward vertex trace is real and
// independently correct, and the replay instantiation walks it bit for bit.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_replay.hpp"

#include "diff/rng/diff_rng.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace ohao::diff::probe {

bool checkReplayEquivalence(ohao::diff::GpuProbeContext& ctx) {
    // =======================================================================
    // 35-36. REPLAY EQUIVALENCE (Stage 1 Task 1). NO GRADIENT IS INVOLVED.
    // =======================================================================
    //
    // Stage 1 makes this renderer differentiable by path replay
    // backpropagation, and the single property everything else in it rests on
    // is this: the BACKWARD kernel must walk the identical path the FORWARD
    // kernel walked, consuming the identical RNG values in the identical
    // order. Spec section 6.2: "Divergence by a single RNG call means the
    // replayed path is a different path and every gradient is silently wrong
    // -- no crash, no NaN." Established here, BEFORE any adjoint exists,
    // because found later it would look like a bad df/dtheta derivation
    // rather than like a stream that went out of step.
    //
    // THE TWO RUNS. shaders/diff/wf_scatter.comp and
    // shaders/diff/wf_scatter_replay.comp are two instantiations of ONE
    // source (shaders/includes/diff/traverse.glsl), differing only in the
    // body of `diffVertexHook` -- which checkTraverseInstantiationTie()
    // above has already established structurally. Each is dispatched in the
    // same position of the same WavefrontLoop over the same closed-box scene,
    // in two INDEPENDENT runs from re-zeroed buffers with the same seed, and
    // each writes its own binding-3 vertex trace into its own buffer.
    //
    // THE ORACLE IS NOT THE OTHER RUN. Check 35 first validates the FORWARD
    // trace on its own, against ohao::diff::PathRng replayed on the CPU and
    // against analytic properties of the scene -- so that check 36's
    // bit-exact comparison cannot pass by both runs having written nothing.
    // The replay is handed no value the forward produced: it re-zeroes path
    // state, re-dispatches generate, and re-derives every vertex from
    // (pixelIndex, sampleIndex, iterationSeed) and the `bounce` it reads back
    // out of path state. That is the seed invariant (spec 4.5) and it is the
    // whole reason "replay" is possible without storing the path.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kCapacity = kW * kH;  // 512
    constexpr float kAlbedo = 0.5f;
    constexpr uint32_t kIterationSeed = 20260828u;
    constexpr uint32_t kBounces = 4;
    // NOT a literal: getting this wrong makes the CPU oracle below walk a
    // different stream than the shader, which is the very failure this
    // pair of checks exists to detect -- so it would be an expected value
    // and a measured value positioned by the same mistake. It is the host
    // constant TIED to the traversal's own declaration by
    // checkDrawsPerBounceTie() above, which the probe refuses to run
    // without. (Checks 15 and 18 predate the tie and still state it as a
    // local literal; they are left untouched, and this constant being
    // tied is what makes their number checkable too.)
    constexpr uint32_t kDrawsPerBounce = ohao::diff::kDrawsPerBounce;
    constexpr uint32_t kStride = ohao::diff::kDebugDrawFloats;

    // Slot names, for diagnostics only. Kept beside TraceSlot rather than
    // derived from it because a printed name is not a check; the TIE that
    // makes these offsets mean anything is checkWfScatterSinkLayoutTie's
    // per-slot expectation table, which is parsed out of the shader.
    struct TraceSlotName {
        uint32_t offset;
        const char* name;
    };
    const TraceSlotName kSlotNames[] = {
        {ohao::diff::kTraceSlotU1, "u1"},
        {ohao::diff::kTraceSlotU2, "u2"},
        {ohao::diff::kTraceSlotDrawCount, "drawCount"},
        {ohao::diff::kTraceSlotULobe, "uLobe"},
        {ohao::diff::kTraceSlotUEnv1, "uEnv1"},
        {ohao::diff::kTraceSlotUEnv2, "uEnv2"},
        {ohao::diff::kTraceSlotOrigin + 0u, "origin.x"},
        {ohao::diff::kTraceSlotOrigin + 1u, "origin.y"},
        {ohao::diff::kTraceSlotOrigin + 2u, "origin.z"},
        {ohao::diff::kTraceSlotDir + 0u, "dir.x"},
        {ohao::diff::kTraceSlotDir + 1u, "dir.y"},
        {ohao::diff::kTraceSlotDir + 2u, "dir.z"},
        {ohao::diff::kTraceSlotThroughput + 0u, "throughput.r"},
        {ohao::diff::kTraceSlotThroughput + 1u, "throughput.g"},
        {ohao::diff::kTraceSlotThroughput + 2u, "throughput.b"},
        {ohao::diff::kTraceSlotHitT, "hitT"},
        {ohao::diff::kTraceSlotBounce, "bounce"},
        {ohao::diff::kTraceSlotPixelIndex, "pixelIndex"},
    };
    static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == kStride,
                  "every trace slot must have a name for diagnostics");

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: replay probe buffers build\n");
        return false;
    }

    std::vector<std::vector<float>> fwdTrace;
    std::vector<std::vector<float>> repTrace;
    if (!ctx.runWavefrontReplayProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed, fwdTrace,
                                     repTrace)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: replay probe dispatch. Until "
                     "shaders/diff/wf_scatter_replay.comp exists there is no second "
                     "instantiation of the traversal, and this is the expected failure\n");
        wf.destroy(ctx.allocator());
        return false;
    }
    if (fwdTrace.size() != kBounces || repTrace.size() != kBounces) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: replay probe returned %zu forward and %zu replay "
                     "traces, expected %u of each\n",
                     fwdTrace.size(), repTrace.size(), kBounces);
        wf.destroy(ctx.allocator());
        return false;
    }

    // -------------------------------------------------------------------
    // 35. The FORWARD trace, validated against an oracle that is not the
    //     replay run: ohao::diff::PathRng on the CPU, plus the analytic
    //     properties of the closed-box scene. This is what stops check 36
    //     from being able to pass on two buffers of zeros -- or on two
    //     buffers of anything else that happens to match.
    // -------------------------------------------------------------------
    std::vector<uint32_t> pixelHits(kCapacity, 0u);
    // Every record's reported pixel index, kept so that the pixel-index
    // IDENTITY assertion can run AFTER the coverage histogram rather than
    // before it. See the note at the histogram itself: asserting the
    // identity first is what made the histogram unable to fail.
    std::vector<uint32_t> recordedPixel(static_cast<std::size_t>(kCapacity) * kBounces, 0u);
    double minU1 = 2.0;
    double maxU1 = -1.0;
    double maxDirLenErr = 0.0;
    float minHitT = std::numeric_limits<float>::infinity();
    for (uint32_t b = 0; b < kBounces; ++b) {
        if (fwdTrace[b].size() != static_cast<std::size_t>(kCapacity) * kStride) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: forward trace for bounce %u holds %zu floats, "
                         "expected %u\n",
                         b, fwdTrace[b].size(), kCapacity * kStride);
            wf.destroy(ctx.allocator());
            return false;
        }
        // The arrival throughput at bounce b is exactly albedo^b: the
        // material is Config's pure-Lambertian default, whose per-bounce
        // estimator weight f*cos/pdf is exactly `albedo`, and the closed
        // box keeps every path alive so nothing skips a decay. Formed by
        // repeated float multiplication, not powf, for the same reason
        // check 17's 0.0625 is: it must be the SAME arithmetic the GPU
        // did, and at albedo 0.5 both are exact anyway.
        float expectedThroughput = 1.0f;
        for (uint32_t i = 0; i < b; ++i) expectedThroughput *= kAlbedo;

        for (uint32_t path = 0; path < kCapacity; ++path) {
            const float* rec = &fwdTrace[b][static_cast<std::size_t>(path) * kStride];

            // Every slot must be a finite number. A record full of NaN
            // would compare unequal to itself and so could not sneak
            // through check 36 -- but it would sneak through as
            // "different", and this says so with the right diagnosis.
            for (uint32_t i = 0; i < kStride; ++i) {
                if (!std::isfinite(rec[i])) {
                    // Slots 2, 16 and 17 are bit-cast uints, whose float
                    // reinterpretation is meaningless; exclude them.
                    if (i == ohao::diff::kTraceSlotDrawCount ||
                        i == ohao::diff::kTraceSlotBounce ||
                        i == ohao::diff::kTraceSlotPixelIndex) {
                        continue;
                    }
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u slot "
                                 "%u (%s) is not finite (%g)\n",
                                 b, path, i, kSlotNames[i].name, static_cast<double>(rec[i]));
                    wf.destroy(ctx.allocator());
                    return false;
                }
            }

            // --- The five draws and the draw count, against PathRng.
            //
            // pathIndex == pixelIndex here: wf_generate.comp writes one
            // path per pixel at sampleIndex 0 and indexes path state by
            // the pixel index, which is also the one-sample-per-pixel
            // condition asserted by the histogram below.
            ohao::diff::PathRng cpu = ohao::diff::PathRng::forPath(path, /*sampleIndex=*/0u,
                                                                   kIterationSeed);
            for (uint32_t i = 0; i < b * kDrawsPerBounce; ++i) (void)cpu.next1D();
            const float expected[5] = {cpu.next1D(), cpu.next1D(), cpu.next1D(), cpu.next1D(),
                                       cpu.next1D()};
            const uint32_t expectedDraws = cpu.drawCount();
            const uint32_t drawSlots[5] = {
                ohao::diff::kTraceSlotU1, ohao::diff::kTraceSlotU2,
                ohao::diff::kTraceSlotULobe, ohao::diff::kTraceSlotUEnv1,
                ohao::diff::kTraceSlotUEnv2};
            for (uint32_t i = 0; i < 5; ++i) {
                if (rec[drawSlots[i]] != expected[i]) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u draw "
                                 "%u (%s): GPU %.9g, ohao::diff::PathRng %.9g. The shader's "
                                 "reconstruction from (pixelIndex, sampleIndex, "
                                 "iterationSeed) fast-forwarded by %u*%u draws does not "
                                 "reproduce the CPU stream, so there is no forward stream for "
                                 "a replay to be equal TO\n",
                                 b, path, i, kSlotNames[drawSlots[i]].name,
                                 static_cast<double>(rec[drawSlots[i]]),
                                 static_cast<double>(expected[i]), b, kDrawsPerBounce);
                    wf.destroy(ctx.allocator());
                    return false;
                }
            }
            uint32_t gpuDraws = 0;
            std::memcpy(&gpuDraws, &rec[ohao::diff::kTraceSlotDrawCount], sizeof(gpuDraws));
            if (gpuDraws != expectedDraws || gpuDraws != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace bounce %u path %u drawCount "
                             "= %u, PathRng says %u, arithmetic says %u\n",
                             b, path, gpuDraws, expectedDraws, (b + 1) * kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return false;
            }

            // --- The vertex the traversal reached.
            uint32_t gpuBounce = 0;
            std::memcpy(&gpuBounce, &rec[ohao::diff::kTraceSlotBounce], sizeof(gpuBounce));
            if (gpuBounce != b) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace slot `bounce` = %u in the "
                             "run of %u bounces, expected %u. The host is not comparing the "
                             "bounce it thinks it is\n",
                             gpuBounce, b + 1, b);
                wf.destroy(ctx.allocator());
                return false;
            }
            uint32_t gpuPixel = 0;
            std::memcpy(&gpuPixel, &rec[ohao::diff::kTraceSlotPixelIndex], sizeof(gpuPixel));
            // RANGE first (the histogram below indexes by this value, so
            // an out-of-range one would be a write past the end rather
            // than a diagnosis), then RECORD. The `gpuPixel == path`
            // identity is asserted after the loop, NOT here -- see the
            // histogram for why the order is the whole point.
            if (gpuPixel >= kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace bounce %u path %u carries "
                             "pixelIndex %u, which is not a pixel of the %u-pixel film this "
                             "run allocates\n",
                             b, path, gpuPixel, kCapacity);
                wf.destroy(ctx.allocator());
                return false;
            }
            recordedPixel[static_cast<std::size_t>(b) * kCapacity + path] = gpuPixel;
            if (b == 0) ++pixelHits[gpuPixel];

            const float hitT = rec[ohao::diff::kTraceSlotHitT];
            if (!(hitT > 0.0f)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace bounce %u path %u has hitT "
                             "= %g. Every ray from strictly inside a CLOSED box leaves it "
                             "through a face, so a non-positive hit distance means the record "
                             "is not describing a real vertex\n",
                             b, path, static_cast<double>(hitT));
                wf.destroy(ctx.allocator());
                return false;
            }
            minHitT = std::min(minHitT, hitT);

            for (uint32_t c = 0; c < 3; ++c) {
                const float t = rec[ohao::diff::kTraceSlotThroughput + c];
                if (t != expectedThroughput) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u "
                                 "arrival throughput component %u = %.9g, expected EXACTLY "
                                 "%.9g (albedo^%u). The traced throughput is not the path's, "
                                 "so the comparison below would be ranging over something "
                                 "other than the vertex it names\n",
                                 b, path, c, static_cast<double>(t),
                                 static_cast<double>(expectedThroughput), b);
                    wf.destroy(ctx.allocator());
                    return false;
                }
            }

            const double dx = rec[ohao::diff::kTraceSlotDir + 0u];
            const double dy = rec[ohao::diff::kTraceSlotDir + 1u];
            const double dz = rec[ohao::diff::kTraceSlotDir + 2u];
            const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            maxDirLenErr = std::max(maxDirLenErr, std::abs(len - 1.0));
            minU1 = std::min(minU1, static_cast<double>(rec[ohao::diff::kTraceSlotU1]));
            maxU1 = std::max(maxU1, static_cast<double>(rec[ohao::diff::kTraceSlotU1]));
        }
    }
    if (maxDirLenErr > 1e-5) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: a forward-trace direction is off unit length by "
                     "%.3g. The recorded `dir` is not a ray direction\n",
                     maxDirLenErr);
        wf.destroy(ctx.allocator());
        return false;
    }
    // NON-VACUITY of the whole comparison, stated as a measurement rather
    // than assumed: if every path drew the same u1 the trace would carry
    // no information and two runs would agree for a reason that has
    // nothing to do with replay. 512 independent streams spread over
    // [0,1) is what makes agreement meaningful.
    if (!(maxU1 - minU1 > 0.9)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the forward trace's u1 spans only [%.6f, %.6f] "
                     "across %u paths x %u bounces. A trace with no variety cannot "
                     "distinguish a real replay from a stage that wrote a constant\n",
                     minU1, maxU1, kCapacity, kBounces);
        wf.destroy(ctx.allocator());
        return false;
    }
    // THE FILM HAZARD, MEASURED (spec 4.5; the choice and its reasoning
    // are stated in wf_scatter.comp's hook). This subsystem resolved the
    // hazard by option (a): ONE SAMPLE PER PIXEL PER DISPATCH, so that
    // several paths of one pixel never atomicAdd the same three film
    // floats within a dispatch and the accumulation order stops depending
    // on the scheduler. runWavefrontReplayProbe refuses to run without
    // width*height == capacity; this is the other half -- the histogram
    // of what the paths ACTUALLY carried.
    //
    // WHY THIS RUNS BEFORE THE IDENTITY ASSERTION (review Finding 3).
    // This histogram used to be built AFTER a per-record
    // `gpuPixel == path` hard failure, which made it a tautology: given
    // that assertion, every record's pixel index was already known to be
    // its own path index, so incrementing a bin per path in [0, capacity)
    // could not produce anything but all-ones. It was structurally
    // incapable of failing, while this check's own OK line credited it
    // with measuring the coverage. The property was still enforced -- by
    // the identity assertion, which is a real comparison against a GPU
    // record -- but by the other assertion, not this one.
    //
    // The fix is ORDER, not deletion: the identity assertion is still
    // made, in full, immediately below, and nothing was weakened. What
    // changed is that the whole trace is now read into `recordedPixel`
    // and `pixelHits` FIRST, so this loop is a measurement of what 512
    // GPU records actually said before anything has asserted what they
    // must say. A stage that wrote `pixelIndex = pathIndex / 2` now fails
    // HERE, with "pixel 0 is covered by 2 paths", rather than being
    // intercepted one record earlier by the identity check. Both
    // assertions remain; only the vacuous one became capable of firing.
    for (uint32_t pix = 0; pix < kCapacity; ++pix) {
        if (pixelHits[pix] != 1u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: pixel %u is covered by %u paths, expected "
                         "exactly 1. More than one sample per pixel inside one dispatch makes "
                         "the film's atomicAdd order scheduler-dependent, and float addition "
                         "is not associative -- which is the accumulation-order artefact spec "
                         "4.5 warns would present as an almost-right, irreproducible "
                         "gradient. This subsystem's resolution is that this never happens\n",
                         pix, pixelHits[pix]);
            wf.destroy(ctx.allocator());
            return false;
        }
    }
    // THE PIXEL-INDEX IDENTITY, deferred out of the record loop above so
    // that the coverage histogram measures something (see the note on
    // it). Unchanged in what it asserts: every record, every bounce.
    for (uint32_t b = 0; b < kBounces; ++b) {
        for (uint32_t path = 0; path < kCapacity; ++path) {
            const uint32_t gpuPixel =
                recordedPixel[static_cast<std::size_t>(b) * kCapacity + path];
            if (gpuPixel != path) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace bounce %u path %u carries "
                             "pixelIndex %u. At one sample per pixel wf_generate.comp indexes "
                             "path state BY the pixel index, so these must be equal\n",
                             b, path, gpuPixel);
                wf.destroy(ctx.allocator());
                return false;
            }
        }
    }
    std::printf(
        "[diff_gpu_probe] OK: check 35 -- the FORWARD traversal's vertex trace is real and "
        "independently correct: for all %u paths x %u bounces, all %u RNG draws per bounce "
        "and the draw count match ohao::diff::PathRng replayed on the CPU (a stream the GPU "
        "never sees), the recorded bounce and pixel indices are self-consistent, every "
        "arrival throughput is EXACTLY albedo^bounce, every hit distance is positive (min "
        "%.4f) and every direction is unit length to %.1e. Non-vacuous: u1 spans [%.4f, "
        "%.4f] across the %u streams, and every one of the %u pixels is covered by exactly "
        "one path -- the one-sample-per-pixel resolution of the film hazard (spec 4.5), "
        "histogrammed over the recorded indices BEFORE the pixelIndex == pathIndex identity "
        "is asserted, so the coverage is measured and not implied by that assertion\n",
        kCapacity, kBounces, kDrawsPerBounce, static_cast<double>(minHitT), maxDirLenErr,
        minU1, maxU1, kCapacity, kCapacity);

    // -------------------------------------------------------------------
    // 36. REPLAY EQUIVALENCE, bit for bit.
    //
    // Every slot of every record, for every path and every bounce, must
    // be BIT-identical between the two instantiations -- compared as raw
    // 32-bit patterns, not as floats, so that a difference of one ulp is
    // a failure and not a rounding excuse. The draws and the draw count
    // are the RNG stream; the origin, direction, throughput and hit
    // distance are the vertex that stream produced. "Same draws" without
    // "same vertex" would be a replay that consumed the right numbers and
    // went somewhere else; "same vertex" without "same draws" would be a
    // path that happened to land in the same place with a stream that has
    // already gone out of step for the NEXT bounce.
    // -------------------------------------------------------------------
    std::size_t comparedSlots = 0;
    for (uint32_t b = 0; b < kBounces; ++b) {
        if (repTrace[b].size() != fwdTrace[b].size()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: replay trace for bounce %u holds %zu floats, "
                         "forward holds %zu\n",
                         b, repTrace[b].size(), fwdTrace[b].size());
            wf.destroy(ctx.allocator());
            return false;
        }
        for (uint32_t path = 0; path < kCapacity; ++path) {
            const std::size_t base = static_cast<std::size_t>(path) * kStride;
            for (uint32_t i = 0; i < kStride; ++i) {
                uint32_t fwdBits = 0;
                uint32_t repBits = 0;
                std::memcpy(&fwdBits, &fwdTrace[b][base + i], sizeof(fwdBits));
                std::memcpy(&repBits, &repTrace[b][base + i], sizeof(repBits));
                ++comparedSlots;
                if (fwdBits == repBits) continue;
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: REPLAY DIVERGED. Bounce %u, path %u, trace slot "
                    "%u (%s): forward 0x%08x (%.9g), replay 0x%08x (%.9g).\n"
                    "  The replay kernel did NOT walk the path the forward kernel walked. "
                    "Under path replay backpropagation this is not a tolerance question: a "
                    "replayed path that differs anywhere is a DIFFERENT path, every "
                    "df/dtheta accumulated along it is evaluated at the wrong vertex, and "
                    "the result is a gradient that is silently wrong with no crash, no NaN "
                    "and no diagnostic (spec 6.2). If the diverging slot is one of the five "
                    "draws or the draw count, one instantiation consumed a different number "
                    "of RNG values than the other -- check that nothing outside "
                    "shaders/includes/diff/traverse.glsl draws, and that neither hook does.\n",
                    b, path, i, kSlotNames[i].name, fwdBits,
                    static_cast<double>(fwdTrace[b][base + i]), repBits,
                    static_cast<double>(repTrace[b][base + i]));
                wf.destroy(ctx.allocator());
                return false;
            }
        }
    }
    std::printf(
        "[diff_gpu_probe] OK: check 36 -- the REPLAY instantiation walks the IDENTICAL path: "
        "%zu trace slots (%u paths x %u bounces x %u slots) are bit-identical between two "
        "independent runs of ohao::diff::WavefrontLoop, one through "
        "shaders/diff/wf_scatter.comp and one through "
        "shaders/diff/wf_scatter_replay.comp. Same five RNG draws per bounce, same draw "
        "count, same origin, same direction, same throughput, same hit distance -- compared "
        "as raw bit patterns, so one ulp is a failure. The replay run was handed nothing the "
        "forward run produced: it re-zeroed path state, re-dispatched generate and "
        "re-derived every vertex from (pixelIndex, sampleIndex, iterationSeed) and the "
        "bounce it read back out of path state\n",
        comparedSlots, kCapacity, kBounces, kStride);

    wf.destroy(ctx.allocator());
    return true;
}

}  // namespace ohao::diff::probe
