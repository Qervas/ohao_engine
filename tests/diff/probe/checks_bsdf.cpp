// BSDF checks 20-23: the lobe itself against the CPU oracle, then the
// three furnace tests at the three values of the lobe probability.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_bsdf.hpp"

#include "probe/oracle_bsdf.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkBsdfTerms(ohao::diff::GpuProbeContext& ctx) {
    // 20. THE BSDF ITSELF (Stage 0b-2b Task 2), term by term, against the
    // independent CPU oracle at the top of this file -- whose formulas come
    // from Walter et al. 2007, Heitz 2014, Heitz 2018, Schlick 1994 and
    // PBRT, NOT from the GLSL under test. See that section's header for what
    // is paper-derived (all of f; the physics inside pdf) and what is
    // instead the documented sampling contract (the lobe probability), and
    // for why the distinction matters to how much this check can prove.
    //
    // Three separate assertions per case, all against the oracle:
    //   (a) f(N,V,L)   -- the BSDF value at a host-chosen L.
    //   (b) pdf(N,V,L) -- the sampling density at that same L.
    //   (c) the SAMPLER's returned weight equals oracle_f(L') * (N.L') /
    //       oracle_pdf(L') at the direction L' the GPU actually sampled.
    // (c) is what ties the sampler to the evaluator: a sampler that draws
    // from one distribution and divides by another's density passes (a) and
    // (b) and fails here. The oracle recomputes f and pdf at L' itself -- it
    // never reuses the GPU's own f/pdf outputs -- so (c) cannot agree by
    // construction either.
    // Tied to bsdf_probe.comp's own `pc.outIndex * <N>u` at startup by
    // checkBsdfShaderConstantTies -- not merely commented as matching it.
    constexpr uint32_t kFloatsPerCase = kBsdfProbeFloatsPerCase;

    struct MaterialSpec {
        const char* name;
        double roughness;
        double metallic;
        double specularWeight;
        double baseColor[3];
    };
    // Deliberately spans: the pure-Lambert configuration the wavefront
    // probes run with (so this check covers the exact material checks 14
    // and 17 depend on), a glossy dielectric, a sharp conductor, a rough
    // conductor, and a half-weight dielectric where the lobe mixture is
    // genuinely a mixture rather than degenerate at one end.
    const MaterialSpec kMaterials[] = {
        {"lambert", 1.00, 0.0, 0.0, {0.5, 0.5, 0.5}},
        {"dielectric-glossy", 0.35, 0.0, 1.0, {0.8, 0.6, 0.2}},
        {"conductor-sharp", 0.15, 1.0, 1.0, {0.9, 0.85, 0.5}},
        {"conductor-rough", 0.80, 1.0, 1.0, {0.3, 0.4, 0.9}},
        {"dielectric-half", 0.50, 0.0, 0.5, {0.2, 0.7, 0.3}},
    };
    // Two normals: the axis-aligned one every earlier probe sees, and a
    // tilted one, so a BSDF that silently assumed N = +Z (as the Stage
    // 0b-1 scatter placeholder effectively did) cannot pass.
    const OracleVec3 kNormals[] = {{0.0, 0.0, 1.0},
                                   oracleNormalize(OracleVec3{0.3, -0.5, 0.81})};
    const double kViewThetas[] = {0.20, 0.70, 1.20};
    const double kLightAngles[][2] = {{0.30, 0.0}, {0.90, 2.0}, {1.30, 4.5}};
    // The view's AZIMUTH about N, cycled independently of everything
    // else. An isotropic BSDF must be invariant to it, and until this was
    // a list the whole table shared one value (0.6) -- so nothing tested
    // that invariance, and a tangent-frame-dependent bug that only
    // showed up at some azimuths could not be seen. 5 is coprime with the
    // 7 sample triples below, so the pairing does not lock into a short
    // cycle over the 90 cases.
    const double kViewPhis[] = {0.6, 1.9, 3.3, 4.7, 5.9};
    // Sample values cycled through the cases so both lobes get chosen
    // somewhere in the table (uLobe below/above the specular probability)
    // and the VNDF sampler is exercised across its unit square. Seven
    // triples, not three: with three, 90 cases carried only three
    // distinct (u1, u2, uLobe) points, so the sampler was being asked the
    // same question thirty times over. The first three are the original
    // ones, kept so the coverage this table already had is not traded
    // away for the new coverage.
    const float kSamples[][3] = {{0.13f, 0.77f, 0.05f},
                                 {0.61f, 0.24f, 0.45f},
                                 {0.89f, 0.52f, 0.95f},
                                 {0.03f, 0.41f, 0.28f},
                                 {0.37f, 0.95f, 0.68f},
                                 {0.72f, 0.09f, 0.11f},
                                 {0.96f, 0.63f, 0.82f}};
    constexpr uint32_t kSampleCount = sizeof(kSamples) / sizeof(kSamples[0]);
    constexpr uint32_t kViewPhiCount = sizeof(kViewPhis) / sizeof(kViewPhis[0]);

    constexpr uint32_t kMaterialCount = sizeof(kMaterials) / sizeof(kMaterials[0]);
    constexpr uint32_t kNormalCount = 2;
    constexpr uint32_t kViewCount = 3;
    constexpr uint32_t kLightCount = 3;
    constexpr uint32_t kCaseCount = kMaterialCount * kNormalCount * kViewCount * kLightCount;

    ohao::diff::ArenaLayout bsdfLayout;
    const std::size_t bsdfBlock = bsdfLayout.add(kCaseCount * kFloatsPerCase);
    ohao::diff::GradientArena bsdfArena;
    if (!bsdfArena.build(ctx.allocator(), bsdfLayout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: bsdf probe arena build\n");
        return false;
    }
    ctx.runImmediate([&](VkCommandBuffer cmd) { bsdfArena.zero(cmd); });

    // Host-side record of each case, so the oracle can be evaluated
    // after every dispatch has landed.
    struct CaseRecord {
        const char* materialName;
        OracleVec3 N;
        OracleVec3 V;
        OracleVec3 L;
        OracleMaterial material;
        double u1;
        double u2;
        double uLobe;
    };
    std::vector<CaseRecord> records;
    records.reserve(kCaseCount);

    bool bsdfDispatchOk = true;
    uint32_t caseIndex = 0;
    for (uint32_t mi = 0; mi < kMaterialCount && bsdfDispatchOk; ++mi) {
        for (uint32_t ni = 0; ni < kNormalCount && bsdfDispatchOk; ++ni) {
            for (uint32_t vi = 0; vi < kViewCount && bsdfDispatchOk; ++vi) {
                for (uint32_t li = 0; li < kLightCount && bsdfDispatchOk; ++li) {
                    const MaterialSpec& ms = kMaterials[mi];
                    const OracleVec3 N = kNormals[ni];
                    const double viewPhi = kViewPhis[caseIndex % kViewPhiCount];
                    const OracleVec3 V = oracleDirFromAngles(N, kViewThetas[vi], viewPhi);
                    const OracleVec3 L =
                        oracleDirFromAngles(N, kLightAngles[li][0], kLightAngles[li][1]);

                    OracleMaterial mat;
                    mat.baseColor = {ms.baseColor[0], ms.baseColor[1], ms.baseColor[2]};
                    mat.roughness = ms.roughness;
                    mat.metallic = ms.metallic;
                    mat.specularWeight = ms.specularWeight;

                    const float* smp = kSamples[caseIndex % kSampleCount];

                    ohao::diff::BsdfProbeCase probeCase;
                    probeCase.normal[0] = static_cast<float>(N.x);
                    probeCase.normal[1] = static_cast<float>(N.y);
                    probeCase.normal[2] = static_cast<float>(N.z);
                    probeCase.roughness = static_cast<float>(ms.roughness);
                    probeCase.view[0] = static_cast<float>(V.x);
                    probeCase.view[1] = static_cast<float>(V.y);
                    probeCase.view[2] = static_cast<float>(V.z);
                    probeCase.metallic = static_cast<float>(ms.metallic);
                    probeCase.light[0] = static_cast<float>(L.x);
                    probeCase.light[1] = static_cast<float>(L.y);
                    probeCase.light[2] = static_cast<float>(L.z);
                    probeCase.specularWeight = static_cast<float>(ms.specularWeight);
                    probeCase.baseColor[0] = static_cast<float>(ms.baseColor[0]);
                    probeCase.baseColor[1] = static_cast<float>(ms.baseColor[1]);
                    probeCase.baseColor[2] = static_cast<float>(ms.baseColor[2]);
                    probeCase.u1 = smp[0];
                    probeCase.u2 = smp[1];
                    probeCase.uLobe = smp[2];
                    probeCase.outIndex = caseIndex;

                    if (!ctx.runBsdfProbe(bsdfArena, probeCase)) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: bsdf probe dispatch for case %u\n",
                                     caseIndex);
                        bsdfDispatchOk = false;
                        break;
                    }
                    records.push_back(CaseRecord{ms.name, N, V, L, mat, smp[0], smp[1],
                                                 smp[2]});
                    ++caseIndex;
                }
            }
        }
    }
    if (!bsdfDispatchOk) {
        bsdfArena.destroy(ctx.allocator());
        return false;
    }

    const std::vector<float> bsdfOut = bsdfArena.readback(ctx.allocator(), bsdfBlock);
    if (bsdfOut.size() < static_cast<std::size_t>(kCaseCount) * kFloatsPerCase) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: bsdf probe readback returned %zu floats, expected "
                     "at least %u\n",
                     bsdfOut.size(), kCaseCount * kFloatsPerCase);
        bsdfArena.destroy(ctx.allocator());
        return false;
    }

    // TOLERANCES, and why they are not one number. The GPU works in
    // float32 and the oracle in double, so some slack is unavoidable;
    // how much depends on the CONDITIONING of the quantity, which is
    // very different for the value and for the density.
    //
    //   f and the sampler weight tolerate 1e-4. Both are smooth in their
    //   inputs at every case in the table, and the measured worst case
    //   over the whole table is ~6e-6 -- a factor of 16 of headroom.
    //
    //   pdf tolerates 1e-3. At the sharpest material here (roughness
    //   0.15, alpha^2 ~ 5e-4) the GGX lobe's angular width is comparable
    //   to alpha, so d(ln D)/d(N.H) ~ 4/alpha^2 ~ 8e3: a float32
    //   direction carrying ~1e-7 of relative error comes back with
    //   ~8e-4 of relative error in D, and therefore in the density.
    //   That is arithmetic conditioning, not a modelling difference, and
    //   the measured worst case (~3.9e-4) sits where the estimate
    //   predicts.
    //
    // Neither number was tuned until it passed. Both are printed with
    // the observed maxima on the OK: line, so a tolerance that has
    // quietly started to absorb a real error is visible rather than
    // silent.
    //
    // For scale, the smallest modelling error this is meant to catch --
    // swapping Schlick's exponent 5 for 4 -- was measured by making that
    // edit: the run aborts at case 20 (dielectric-glossy) with a
    // deviation of 4.8e-4, i.e. 4.8x the f tolerance and ~140x the
    // observed float32 noise floor of ~3.4e-6. It is NOT true that every
    // case moves by that much: cases 18 and 19 are the same material and
    // moved by LESS than the tolerance, so they did not register at all.
    // What catches this class of error is the BREADTH of the table -- the
    // spread of view, light and normal geometry means some case is
    // sensitive -- not the sensitivity of any individual case. Shrinking
    // the table would weaken the check even with the tolerance untouched.
    constexpr double kBsdfValueRelTol = 1e-4;
    constexpr double kBsdfPdfRelTol = 1e-3;

    double maxFErr = 0.0;
    double maxPdfErr = 0.0;
    double maxWeightErr = 0.0;
    uint32_t specularSampledCount = 0;
    uint32_t diffuseSampledCount = 0;
    uint32_t rejectedSampleCount = 0;
    uint32_t grazingRejectionCount = 0;
    uint32_t branchAssertedCount = 0;
    // How close uLobe may sit to q before the branch-agreement assertion
    // below stands down. At |uLobe - q| under this the GPU's float32 q
    // and the oracle's double q can legitimately land on opposite sides
    // of the comparison, and asserting there would be asserting float
    // rounding. The count of cases actually asserted is printed, so a
    // margin that quietly disabled the assertion for the whole table
    // would be visible.
    constexpr double kLobeDecisionMargin = 1e-3;

    for (uint32_t i = 0; i < kCaseCount; ++i) {
        const CaseRecord& rec = records[i];
        const float* out = &bsdfOut[static_cast<std::size_t>(i) * kFloatsPerCase];

        OracleVec3 refF;
        double refPdf = 0.0;
        oracleBsdfEval(rec.N, rec.V, rec.L, rec.material, refF, refPdf);

        const double gpuF[3] = {out[0], out[1], out[2]};
        const double refFv[3] = {refF.x, refF.y, refF.z};
        for (int c = 0; c < 3; ++c) {
            const double e = oracleRelDiff(refFv[c], gpuF[c]);
            if (e > maxFErr) maxFErr = e;
            if (e > kBsdfValueRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF f mismatch, case %u (%s), channel "
                             "%d: GPU %.9g, CPU oracle %.9g (relative %.3g > %.3g). N = "
                             "(%.6f,%.6f,%.6f), V = (%.6f,%.6f,%.6f), L = (%.6f,%.6f,%.6f), "
                             "roughness %.3f, metallic %.3f, specularWeight %.3f\n",
                             i, rec.materialName, c, gpuF[c], refFv[c], e, kBsdfValueRelTol,
                             rec.N.x, rec.N.y, rec.N.z, rec.V.x, rec.V.y, rec.V.z, rec.L.x,
                             rec.L.y, rec.L.z, rec.material.roughness, rec.material.metallic,
                             rec.material.specularWeight);
                bsdfArena.destroy(ctx.allocator());
                return false;
            }
        }

        const double gpuPdf = out[3];
        const double pdfErr = oracleRelDiff(refPdf, gpuPdf);
        if (pdfErr > maxPdfErr) maxPdfErr = pdfErr;
        if (pdfErr > kBsdfPdfRelTol) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: BSDF pdf mismatch, case %u (%s): GPU %.9g, "
                         "CPU oracle %.9g (relative %.3g > %.3g). roughness %.3f, metallic "
                         "%.3f, specularWeight %.3f\n",
                         i, rec.materialName, gpuPdf, refPdf, pdfErr, kBsdfPdfRelTol,
                         rec.material.roughness, rec.material.metallic,
                         rec.material.specularWeight);
            bsdfArena.destroy(ctx.allocator());
            return false;
        }

        // (c) The sampler's own weight, at the direction the GPU drew.
        // Slots 4..6 hold that direction EXACTLY as drawn -- including
        // the GGX VNDF's below-horizon tail -- because diffBsdfSample
        // deliberately does not substitute a usable direction for a
        // rejected sample. That is what lets the oracle confirm a zero
        // weight was legitimate instead of having to take it on trust.
        const OracleVec3 sampledL = oracleNormalize({out[4], out[5], out[6]});
        const double gpuWeight[3] = {out[7], out[8], out[9]};
        const double gpuSampPdf = out[10];
        OracleVec3 sampF;
        double sampPdf = 0.0;
        oracleBsdfEval(rec.N, rec.V, sampledL, rec.material, sampF, sampPdf);
        const double sampNdotL = oracleDot(rec.N, sampledL);

        // WHICH LOBE THE GPU ACTUALLY SAMPLED, decided from the returned
        // direction rather than predicted from the material table. The
        // diffuse branch draws diffCosineHemisphere(N, uDir); the
        // specular branch draws a VNDF half-vector and reflects. So if
        // the returned direction IS the cosine-hemisphere direction for
        // this case's uDir, the diffuse branch ran. (The two agreeing by
        // accident is a measure-zero coincidence, and the tolerance here
        // is float32 noise, not a window.) This previously classified by
        // oracleSpecProb(...) > 0.5, which reads only the hardcoded
        // material table and the hardcoded normals: no GPU output entered
        // it, so it was deterministic and could not fail while its FAIL
        // message claimed to describe which branch the sampler took.
        const OracleVec3 cosineL = oracleCosineHemisphere(rec.N, rec.u1, rec.u2);
        const bool tookDiffuseBranch = oracleDistance(sampledL, cosineL) < 1e-4;
        if (tookDiffuseBranch) {
            ++diffuseSampledCount;
        } else {
            ++specularSampledCount;
        }

        // And the branch it took must be the branch the documented
        // strategy asks for: uLobe < q, with q the lobe probability this
        // oracle computes independently. This is the only place the
        // lobe-selection rule itself is asserted per-case -- see the
        // oracle header's note on where q is guarded.
        const double caseQ = oracleSpecProb(rec.material, oracleDot(rec.N, rec.V));
        const bool expectSpecularBranch =
            (rec.uLobe < caseQ) && (oracleDot(rec.N, rec.V) > kShaderGrazingCos);
        if (std::abs(rec.uLobe - caseQ) > kLobeDecisionMargin) {
            ++branchAssertedCount;
            if (expectSpecularBranch == tookDiffuseBranch) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) took the %s "
                             "branch, but the contract's lobe probability q = %.9g with "
                             "uLobe = %.9g asks for the %s branch. The branch actually taken "
                             "was read off the returned direction L = (%.6f,%.6f,%.6f)\n",
                             i, rec.materialName, tookDiffuseBranch ? "diffuse" : "specular",
                             caseQ, rec.uLobe, expectSpecularBranch ? "specular" : "diffuse",
                             sampledL.x, sampledL.y, sampledL.z);
                bsdfArena.destroy(ctx.allocator());
                return false;
            }
        }

        if (gpuSampPdf <= 0.0) {
            // The GPU says it rejected this sample. Two things have to
            // hold, or the rejection is a bug rather than a tail: the
            // ORACLE must independently agree the direction carries no
            // energy, and the weight must be exactly zero. A sampler
            // that rejected everything would fail the first of those on
            // its very first accepted-looking case.
            //
            // One documented exception, and it is a real threshold
            // difference rather than slack: bsdf.glsl refuses the
            // specular math at N.L <= DIFF_BSDF_MIN_COS (1e-4) while this
            // oracle refuses it at N.L <= 0, because 1e-4 is an
            // implementation guard against dividing by (N.L) twice and
            // has no place in the physics. A sample landing in the band
            // (0, 1e-4] is therefore rejected by the shader and accepted
            // by the oracle, legitimately. Such cases are counted and
            // reported rather than being allowed to fail the run; every
            // case in the current table sits orders of magnitude above
            // the band, so the count is expected to be 0 and a nonzero
            // one is worth looking at.
            if (sampNdotL > 0.0 && sampNdotL <= kShaderGrazingCos) {
                ++grazingRejectionCount;
            } else if (sampNdotL > 0.0 && sampPdf > 0.0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) rejected its "
                             "own sample (pdf 0) at L = (%.6f,%.6f,%.6f), but the oracle says "
                             "that direction is perfectly valid (N.L = %.9g, pdf = %.9g) and "
                             "is not inside the shader's documented grazing band "
                             "(0, %.0e]\n",
                             i, rec.materialName, sampledL.x, sampledL.y, sampledL.z,
                             sampNdotL, sampPdf, kShaderGrazingCos);
                bsdfArena.destroy(ctx.allocator());
                return false;
            }
            for (int c = 0; c < 3; ++c) {
                if (gpuWeight[c] != 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported a "
                                 "zero density but a non-zero weight %.9g -- a zero-BRDF "
                                 "sample must carry a zero weight\n",
                                 i, rec.materialName, gpuWeight[c]);
                    bsdfArena.destroy(ctx.allocator());
                    return false;
                }
            }
            ++rejectedSampleCount;
            continue;
        }

        if (sampPdf <= 0.0 || sampNdotL <= 0.0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) accepted a sample "
                         "(pdf %.9g) at L = (%.6f,%.6f,%.6f) that the oracle says carries no "
                         "energy (N.L = %.9g)\n",
                         i, rec.materialName, gpuSampPdf, sampledL.x, sampledL.y, sampledL.z,
                         sampNdotL);
            bsdfArena.destroy(ctx.allocator());
            return false;
        }

        // The density the sampler says it drew with must be the density
        // the evaluator assigns to that direction. This is what a
        // "samples one distribution, divides by another" bug shows up
        // as, and it is checked against the ORACLE's density, not
        // against the GPU's own eval output.
        const double sampPdfErr = oracleRelDiff(sampPdf, gpuSampPdf);
        if (sampPdfErr > maxPdfErr) maxPdfErr = sampPdfErr;
        if (sampPdfErr > kBsdfPdfRelTol) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported density "
                         "%.9g at its own sampled direction, CPU oracle %.9g (relative %.3g "
                         "> %.3g)\n",
                         i, rec.materialName, gpuSampPdf, sampPdf, sampPdfErr,
                         kBsdfPdfRelTol);
            bsdfArena.destroy(ctx.allocator());
            return false;
        }

        const double refWeight[3] = {sampF.x * sampNdotL / sampPdf,
                                     sampF.y * sampNdotL / sampPdf,
                                     sampF.z * sampNdotL / sampPdf};
        for (int c = 0; c < 3; ++c) {
            const double e = oracleRelDiff(refWeight[c], gpuWeight[c]);
            if (e > maxWeightErr) maxWeightErr = e;
            if (e > kBsdfValueRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler weight mismatch, case %u "
                             "(%s), channel %d: GPU %.9g, CPU oracle f*cos/pdf %.9g "
                             "(relative %.3g > %.3g). Sampled L = (%.6f,%.6f,%.6f), "
                             "oracle pdf there %.9g -- the sampler and the evaluator "
                             "disagree about which density the direction was drawn from\n",
                             i, rec.materialName, c, gpuWeight[c], refWeight[c], e,
                             kBsdfValueRelTol, sampledL.x, sampledL.y, sampledL.z, sampPdf);
                bsdfArena.destroy(ctx.allocator());
                return false;
            }
        }
    }

    if (specularSampledCount == 0 || diffuseSampledCount == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: over %u cases the sampler took only one of its "
                     "two branches (%u specular, %u diffuse, counted from the direction the "
                     "GPU returned) -- the table no longer covers the mixture it claims to\n",
                     kCaseCount, specularSampledCount, diffuseSampledCount);
        bsdfArena.destroy(ctx.allocator());
        return false;
    }
    if (branchAssertedCount * 4u < kCaseCount) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the lobe-branch agreement assertion stood down "
                     "on all but %u of %u cases (uLobe within %.0e of q) -- it is no longer "
                     "asserting the lobe-selection rule over a meaningful part of the "
                     "table\n",
                     branchAssertedCount, kCaseCount, kLobeDecisionMargin);
        bsdfArena.destroy(ctx.allocator());
        return false;
    }

    // Non-vacuity guard on assertion (c): every rejected sample skips the
    // weight comparison, so a sampler that rejected most of the table
    // could pass (c) having compared almost nothing. The rejected cases
    // are still individually verified against the oracle above -- this
    // bounds how much of the table that verification is allowed to be.
    if (rejectedSampleCount * 4u >= kCaseCount) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: BSDF sampler rejected %u of %u cases -- the "
                     "weight comparison is no longer exercising most of the table\n",
                     rejectedSampleCount, kCaseCount);
        bsdfArena.destroy(ctx.allocator());
        return false;
    }

    std::printf("[diff_gpu_probe] OK: BSDF f, pdf and sampler weight match an independent "
                "CPU oracle (Walter 2007 / Heitz 2014 / Heitz 2018 / Schlick 1994) over %u "
                "cases x %u materials x %u sample triples x %u view azimuths; the GPU took "
                "the specular branch %u times and the diffuse branch %u times (measured from "
                "the returned direction), and the branch matched the contract's uLobe < q on "
                "all %u cases where the assertion applied; %u below-horizon rejections, %u "
                "inside the shader's grazing band; max relative error f %.3g, weight %.3g "
                "(tolerance %.3g), pdf %.3g (tolerance %.3g)\n",
                kCaseCount, kMaterialCount, kSampleCount, kViewPhiCount,
                specularSampledCount, diffuseSampledCount, branchAssertedCount,
                rejectedSampleCount, grazingRejectionCount, maxFErr, maxWeightErr,
                kBsdfValueRelTol, maxPdfErr, kBsdfPdfRelTol);

    bsdfArena.destroy(ctx.allocator());
    return true;
}

bool checkFurnaces(ohao::diff::GpuProbeContext& ctx) {
    // 21-23. THE FURNACE TEST, on its own deliberately trivial scene.
    //
    // Check 20 compares the BSDF term by term. It cannot catch an error in
    // how the terms are COMBINED into a path -- a missing cosine, a pdf
    // divided in the wrong place, a lobe probability that does not match the
    // branch it gates -- because every one of those is a property of the
    // whole sample-evaluate-weight loop rather than of any single term. The
    // furnace test is the global counterpart: it runs the real
    // wf_scatter.comp dispatch and asks whether energy is conserved.
    //
    // SCENE. Its own, not Task 1's box. Task 1's scene is geometry-bearing
    // and carries a provable four-bounce survival bound; a furnace scene is
    // supposed to be trivial, and making one scene serve both would force a
    // constraint on Task 1's that it was never designed for. This one is the
    // single full quad every intersect check already uses: generate a path
    // per pixel, trace it once so every path has a real hit point and a real
    // geometric normal, then run exactly ONE scatter dispatch. Throughput
    // starts at 1 (wf_generate.comp writes it), so after that dispatch the
    // Throughput field IS the BSDF estimator weight, path by path.
    //
    // ENVIRONMENT. Constant radiance L0 = 1 in every direction, evaluated
    // ANALYTICALLY here on the host -- no CDF, no importance sampling, no
    // env_sampling.glsl. Environment importance sampling is Task 3; a
    // furnace needs nothing more than a constant. Because L0 is constant,
    // the radiance an escaping path would deposit is throughput * L0 =
    // throughput, so reading Throughput back IS reading the furnace estimate
    // and no radiance-accumulation stage is needed to run this.
    //
    // ------------------------------------------------------------------
    // 21. WHITE FURNACE, and its error bound, derived
    // ------------------------------------------------------------------
    //
    // Material: base colour rho = 1, no absorption, specular lobe scaled out
    // (specularWeight = 0, metallic = 0), so f = rho/pi on the hemisphere.
    //
    // The quantity being estimated is the outgoing radiance
    //     Lo = integral over the hemisphere of f * L0 * cos(theta) dw
    //        = L0 * rho * integral of cos(theta)/pi dw
    //        = L0 * rho = 1.
    //
    // The estimator is one cosine-weighted sample, w ~ p(w) = cos(theta)/pi:
    //     X = f(w) * L0 * cos(theta) / p(w)
    //       = (rho/pi) * L0 * cos(theta) * pi / cos(theta)
    //       = rho * L0 = 1,   for EVERY w.
    //
    // X is therefore a CONSTANT random variable. Var[X] = 0, so the Monte
    // Carlo standard error sigma/sqrt(N) is exactly 0 at every sample count
    // -- there is no noise term to bound. This is not a weakness of the
    // test: perfect importance sampling of a constant integrand is precisely
    // what a white furnace is, and it means the check can be made TIGHT
    // rather than statistical.
    //
    // What remains is float32 rounding. The shader takes the analytic
    // cancellation (see diffBsdfSample's pure-Lambert branch), so the weight
    // is the single product baseColor * (1 - metallic) = 1.0 * 1.0, with no
    // division and no transcendental: exactly representable, exactly 1.
    // Allowing for a compiler that contracts that product differently, the
    // bound asserted is 4 units in the last place of 1.0f,
    //     4 * 2^-23 = 4.768e-7,
    // and the OBSERVED maximum deviation is printed on the OK: line, so a
    // bound that has quietly started absorbing a real error is visible
    // rather than silent. Nothing about this tolerance was tuned until it
    // passed: it was derived first and the observed value came in at 0.
    //
    // ------------------------------------------------------------------
    // 22. GLOSSY ENERGY BOUND -- why it is NOT also 1.0
    // ------------------------------------------------------------------
    //
    // Running the same furnace with a white ROUGH CONDUCTOR (base colour 1,
    // metallic 1) must NOT be asserted to give 1.0. This BSDF models
    // single-scattering GGX only: light that would have bounced a second
    // time between microfacets is dropped, and the resulting energy deficit
    // is a well-known, published property of the model (Heitz et al.,
    // "Multiple-Scattering Microfacet BSDFs with the Smith Model",
    // SIGGRAPH 2016), not a bug in this implementation. Asserting 1.0 here
    // would be asserting something false.
    //
    // What IS provable pointwise: with base colour 1 and metallic 1,
    // F0 = 1, so Schlick gives F = 1 + (1-1)(...) = 1 identically, and the
    // lobe probability q = mix(..., 1.0, metallic) = 1 exactly, so the
    // mixture density collapses to the pure VNDF density and the diffuse
    // lobe vanishes. The weight is then
    //     f*cos/pdf = [D G2 / (4 (N.V)(N.L))] (N.L) / [G1(V) D / (4 (N.V))]
    //               = G2(V,L) / G1(V),
    // and height-correlated Smith gives
    //     G2/G1 = (1 + Lambda(V)) / (1 + Lambda(V) + Lambda(L)) <= 1,
    // with equality only when Lambda(L) = 0, i.e. L exactly along N
    // (Heitz 2014 Eq. 43 and 99). So EVERY path's weight is in [0, 1], and
    // the mean is strictly below 1. Both halves are asserted: an upper bound
    // no sample may exceed (energy gain), and a mean strictly under it
    // (the deficit is really there rather than having been papered over).
    //
    // ------------------------------------------------------------------
    // 23. INTERMEDIATE q -- the run that actually exercises the mixture
    // ------------------------------------------------------------------
    //
    // WHY A THIRD RUN. Checks 21 and 22 sit at the two lobe probabilities
    // that cannot bias anything: 21 is {roughness 1, metallic 0,
    // specularWeight 0}, i.e. q = 0 exactly, and 22 is a conductor, i.e.
    // q = 1 exactly. Worse, at q = 0 diffBsdfSample takes an early-return
    // branch that never calls diffBsdfEval, never forms f, never forms pdf
    // and never divides -- so check 21's derivation above describes
    // arithmetic the GPU does not execute. It verifies that one
    // multiplication returns baseColor. That is worth having, and it is not
    // a statement about the mixture density or about f*cos/pdf.
    //
    // MATERIAL. baseColor 1, roughness 0.30, metallic 0.5, specularWeight 1.
    // A pure dielectric cannot reach an intermediate q in this model: its
    // F0 is 0.04, so q = specularWeight * F_max(|N.V|) * (1 - 0.9*roughness)
    // is capped near 0.04 at the near-normal incidence this scene provides.
    // Raising metallic instead is what moves q into the middle -- here to
    // about 0.69 -- while keeping BOTH lobes materially present in f
    // (kd = 1 - metallic = 0.5). Every path now goes through diffBsdfEval,
    // forms the full mixture density, and divides. (DESIGN CALL: the review
    // asked for "a dielectric with specularWeight > 0"; no dielectric in
    // this parameterisation has an intermediate q, so a half-metal is used
    // instead and the reason is recorded here.)
    //
    // WHAT IS ASSERTED, and how each bound is derived.
    //
    // (a) A POINTWISE upper bound on the weight. Unlike check 22 the weight
    //     is not G2/G1 and is not bounded by 1 -- it is a mixture estimator,
    //     and a mixture estimator's weight is not bounded by the material's
    //     albedo. It IS bounded, though, and the bound is elementary. With
    //     f = f_d + f_s and pdf = (1-q) p_d + q p_s, dropping one term from
    //     each denominator gives
    //         f cos / pdf <= f_d cos / ((1-q) p_d)  +  f_s cos / (q p_s)
    //                      = rho(1-metallic)/(1-q)  +  F (G2/G1) / q
    //                     <= rho(1-metallic)/(1-q)  +  specScale / q,
    //     using F <= 1 and G2/G1 <= 1 (Heitz 2014 Eq. 99), and p_d, p_s
    //     being the cosine and VNDF densities the two branches draw from.
    //     q varies across the frame only through |N.V|, which this narrow
    //     frustum confines to a small interval, so the bound is evaluated at
    //     the worst q in that interval and is a constant. A weight above it
    //     is energy created out of nothing.
    //
    // (b) The MEAN, against a numerically integrated reference. The
    //     estimator is unbiased for the directional albedo whatever the
    //     sampling strategy is,
    //         E[f cos / pdf] = INT f(N,V,L) (N.L) dL = rho_dir(V),
    //     and rho_dir is computable here: oracleDirectionalAlbedo integrates
    //     THIS FILE'S paper-derived f by midpoint quadrature in double
    //     precision. There is no closed form for it -- that is the point of
    //     Heitz 2016 -- but there does not need to be one.
    //
    //     An earlier version of this check bracketed the mean instead, using
    //     F0 <= F <= 1 with check 22's measurement of the single-scattering
    //     GGX albedo. That bracket is correct but useless in one direction:
    //     Schlick's grazing term is (1 - V.H)^5, and at this geometry -- a
    //     narrow frustum onto a facing quad, alpha = 0.09 -- V.H sits within
    //     about 0.02 of 1 across the whole lobe, so F is F0 to four decimal
    //     places and the F <= 1 end is slack by a third of the answer. A q
    //     perturbation that biased the mean upward by 14% was measured
    //     passing it. The quadrature reference is two-sided and tight.
    //
    //     rho_dir depends on the view angle, and each path has its own,
    //     so the reference is evaluated across the frame's whole |N.V| range
    //     and the min and max are taken. Two error terms are added to that
    //     interval, both MEASURED rather than chosen: the quadrature's own
    //     discretisation error, estimated by redoing one evaluation at half
    //     resolution and taking the difference, and 5 standard errors of the
    //     GPU sample mean, computed from the 3072 samples themselves. Both
    //     are printed, as is the distance from the reference in units of the
    //     standard error, so a run drifting toward the bound is visible long
    //     before it crosses one.
    //
    // WHAT THIS DOES AND DOES NOT GUARD ABOUT q. A consistent change to q --
    // one that moves the branch probability and the mixture weight together
    // -- leaves the estimator unbiased, and this check will correctly not
    // fire: any q in (0,1) that is nonzero wherever f is nonzero is a legal
    // strategy. What it catches is q's coverage failing (a lobe stops being
    // reachable while f still has energy there) and the branch and the
    // density disagreeing about q, which is the bug this arrangement is
    // actually exposed to.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;
    constexpr uint32_t kCapacity = kW * kH;  // 3072
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;
    constexpr uint32_t kIterationSeed = 424242u;
    // Derived above: 4 units in the last place of 1.0f. Not tuned.
    constexpr float kFurnaceUlpBound = 4.0f * 1.1920929e-7f;

    struct FurnaceRun {
        const char* name;
        ohao::diff::WavefrontScatterMaterial material;
    };
    const FurnaceRun kRuns[] = {
        {"lambert", ohao::diff::WavefrontScatterMaterial{1.0f, 0.0f, 0.0f}},
        {"white rough conductor", ohao::diff::WavefrontScatterMaterial{0.30f, 1.0f, 1.0f}},
        {"half-metal, intermediate q",
         ohao::diff::WavefrontScatterMaterial{0.30f, 0.5f, 1.0f}},
    };
    constexpr uint32_t kFurnaceRunCount = sizeof(kRuns) / sizeof(kRuns[0]);
    constexpr uint32_t kMixtureRun = 2;

    double furnaceMean[kFurnaceRunCount] = {0.0, 0.0, 0.0};
    double furnaceMax[kFurnaceRunCount] = {0.0, 0.0, 0.0};
    double furnaceMin[kFurnaceRunCount] = {0.0, 0.0, 0.0};
    double furnaceStdErr[kFurnaceRunCount] = {0.0, 0.0, 0.0};
    double lambertMaxDeviation = 0.0;

    // The mixture run's material, restated for the host so the bounds
    // below are derived from the contract rather than from constants
    // typed twice. oracleSpecProb / oracleSpecScale / oracleF0 are the
    // same independent implementations check 20 uses.
    OracleMaterial mixMat;
    mixMat.baseColor = {1.0, 1.0, 1.0};  // furnace albedo is 1
    mixMat.roughness = kRuns[kMixtureRun].material.roughness;
    mixMat.metallic = kRuns[kMixtureRun].material.metallic;
    mixMat.specularWeight = kRuns[kMixtureRun].material.specularWeight;

    // |N.V| over the frame. The quad faces the camera, so N.V for the
    // pixel at (x,y) is 1/sqrt(1 + dx^2 + dy^2) with the same dx, dy the
    // closed-form ray in check 3 uses; it is 1 dead centre and smallest
    // in a corner.
    constexpr double kFurnaceAspect = static_cast<double>(kW) / static_cast<double>(kH);
    const double dxMax = (1.0 - 1.0 / kW) * kFurnaceAspect * kTanHalfFov;
    const double dyMax = (1.0 - 1.0 / kH) * kTanHalfFov;
    const double cosMin = 1.0 / std::sqrt(1.0 + dxMax * dxMax + dyMax * dyMax);
    const double qAtNormal = oracleSpecProb(mixMat, 1.0);
    const double qAtCorner = oracleSpecProb(mixMat, cosMin);
    const double qMin = std::min(qAtNormal, qAtCorner);
    const double qMax = std::max(qAtNormal, qAtCorner);
    const double mixDiffuseAlbedo = mixMat.baseColor.x * (1.0 - mixMat.metallic);
    const double mixSpecScale = oracleSpecScale(mixMat);
    // (a), derived above: rho(1-metallic)/(1-q) + specScale/q, at the
    // worst q in the frame's interval (the first term grows with q, the
    // second shrinks, so the extremes are taken independently).
    const double mixPointwiseBound = mixDiffuseAlbedo / (1.0 - qMax) + mixSpecScale / qMin;

    for (uint32_t run = 0; run < kFurnaceRunCount; ++run) {
        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace buffers build (%s)\n",
                         kRuns[run].name);
            return false;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_generate (%s)\n",
                         kRuns[run].name);
            wf.destroy(ctx.allocator());
            return false;
        }
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_intersect (%s)\n",
                         kRuns[run].name);
            wf.destroy(ctx.allocator());
            return false;
        }
        const std::uint32_t seeded =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (seeded != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: furnace setup (%s): %u of %u rays hit the "
                         "full quad, expected all of them -- the furnace estimate would be "
                         "averaged over paths that never scattered\n",
                         kRuns[run].name, seeded, kCapacity);
            wf.destroy(ctx.allocator());
            return false;
        }

        std::vector<uint32_t> outQueue;
        std::vector<float> outDraws;
        if (!ctx.runWavefrontScatterProbe(
                wf, /*srcQueueBase=*/kCapacity,
                ohao::diff::WavefrontBuffers::kNextCountSlot, /*dstQueueBase=*/0u,
                ohao::diff::WavefrontBuffers::kCurrentCountSlot, /*albedo=*/1.0f,
                kIterationSeed, outQueue, outDraws, kRuns[run].material)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace scatter dispatch (%s)\n",
                         kRuns[run].name);
            wf.destroy(ctx.allocator());
            return false;
        }

        const std::vector<float> tR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> tG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> tB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (tR.size() != kCapacity || tG.size() != kCapacity || tB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace throughput readback size "
                                  "mismatch (%s)\n",
                         kRuns[run].name);
            wf.destroy(ctx.allocator());
            return false;
        }

        double sum = 0.0;
        double sumSq = 0.0;
        double maxV = -1.0;
        double minV = 2.0;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Grey material and grey environment: the three channels must
            // agree exactly, and a check that looked at only one would
            // miss a per-channel error entirely.
            if (tR[i] != tG[i] || tG[i] != tB[i]) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                             "grey: (%.9g,%.9g,%.9g) from a grey base colour\n",
                             kRuns[run].name, i, static_cast<double>(tR[i]),
                             static_cast<double>(tG[i]), static_cast<double>(tB[i]));
                wf.destroy(ctx.allocator());
                return false;
            }
            const double v = tR[i];
            if (!std::isfinite(v)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                             "finite (%.9g)\n",
                             kRuns[run].name, i, v);
                wf.destroy(ctx.allocator());
                return false;
            }
            sum += v;
            sumSq += v * v;
            if (v > maxV) maxV = v;
            if (v < minV) minV = v;

            if (run == 0) {
                const double dev = std::abs(v - 1.0);
                if (dev > lambertMaxDeviation) lambertMaxDeviation = dev;
                if (dev > kFurnaceUlpBound) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: white furnace path %u returned %.9g, "
                                 "expected 1.0 within %.3g (4 ulp). With albedo 1 and no "
                                 "absorption the cosine-sampled Lambert estimator is a "
                                 "CONSTANT 1 for every direction -- zero variance -- so this "
                                 "is an energy-conservation error in the "
                                 "sample-evaluate-weight loop, not Monte Carlo noise\n",
                                 i, v, static_cast<double>(kFurnaceUlpBound));
                    wf.destroy(ctx.allocator());
                    return false;
                }
            } else if (run == 1) {
                if (v > 1.0 + kFurnaceUlpBound) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: glossy furnace path %u returned "
                                 "%.9g > 1 -- with F identically 1 and the lobe probability "
                                 "exactly 1, the weight is G2/G1, which height-correlated "
                                 "Smith bounds at 1 (Heitz 2014 Eq. 99). A value above 1 is "
                                 "energy created out of nothing\n",
                                 i, v);
                    wf.destroy(ctx.allocator());
                    return false;
                }
                if (v < 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: glossy furnace path %u returned a "
                                 "negative throughput %.9g\n",
                                 i, v);
                    wf.destroy(ctx.allocator());
                    return false;
                }
            } else {
                // (a): the pointwise mixture bound derived above. NOT 1 --
                // a mixture estimator's weight legitimately exceeds the
                // albedo on directions one lobe's density under-covers.
                if (v > mixPointwiseBound) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: mixture furnace path %u returned "
                                 "%.9g, above the derived pointwise bound %.9g = "
                                 "rho(1-metallic)/(1-q) + specScale/q with q in [%.6f, "
                                 "%.6f]. Every term in that bound comes from F <= 1 and "
                                 "G2/G1 <= 1, so exceeding it is energy created out of "
                                 "nothing\n",
                                 i, v, mixPointwiseBound, qMin, qMax);
                    wf.destroy(ctx.allocator());
                    return false;
                }
                if (v < 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: mixture furnace path %u returned a "
                                 "negative throughput %.9g\n",
                                 i, v);
                    wf.destroy(ctx.allocator());
                    return false;
                }
            }
        }
        furnaceMean[run] = sum / static_cast<double>(kCapacity);
        furnaceMax[run] = maxV;
        furnaceMin[run] = minV;
        const double meanSq = sumSq / static_cast<double>(kCapacity);
        const double var =
            std::max(0.0, meanSq - furnaceMean[run] * furnaceMean[run]);
        furnaceStdErr[run] = std::sqrt(var / static_cast<double>(kCapacity));

        wf.destroy(ctx.allocator());
    }

    std::printf("[diff_gpu_probe] OK: white furnace (albedo 1, no absorption, constant "
                "environment) returns 1.0 for all %u paths; mean %.9g, max |deviation| %.3g "
                "(derived bound %.3g = 4 ulp; Monte Carlo variance is exactly 0 -- see the "
                "derivation above this check)\n",
                kCapacity, furnaceMean[0], lambertMaxDeviation,
                static_cast<double>(kFurnaceUlpBound));

    // The deficit must be REAL, not just "<= 1". A mean of exactly 1
    // would mean the specular lobe silently degenerated to the Lambert
    // fast path; a mean at 0 would mean every sample was rejected.
    if (!(furnaceMean[1] < 1.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g, expected strictly "
                     "below 1 -- single-scattering GGX loses energy by construction, so a "
                     "mean of exactly 1 means the specular lobe was not the one evaluated\n",
                     furnaceMean[1]);
        return false;
    }
    if (!(furnaceMean[1] > 0.5)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g. At roughness 0.3 "
                     "the single-scattering Smith deficit is a few percent, not half the "
                     "energy -- this size of loss means samples are being rejected or "
                     "weighted with the wrong density, not that multiple scattering is "
                     "missing\n",
                     furnaceMean[1]);
        return false;
    }
    std::printf("[diff_gpu_probe] OK: glossy furnace (white conductor, roughness 0.30) "
                "conserves energy without creating any: every one of %u paths in [0, 1], "
                "mean %.9g (strictly below 1 -- the known single-scattering GGX deficit), "
                "min %.9g, max %.9g\n",
                kCapacity, furnaceMean[1], furnaceMin[1], furnaceMax[1]);

    // ------------------------------------------------------------------
    // 23. The intermediate-q run's mean, against a quadrature of the
    // oracle's own f. See the derivation above this block.
    // ------------------------------------------------------------------
    constexpr uint32_t kQuadTheta = 512;
    constexpr uint32_t kQuadPhi = 512;
    // rho_dir over the frame's |N.V| range. The range is narrow, so a
    // scan is enough to bracket it; the observed spread is printed, and
    // it is orders of magnitude under the Monte Carlo allowance.
    constexpr uint32_t kCosScan = 5;
    double rhoLo = 0.0;
    double rhoHi = 0.0;
    for (uint32_t k = 0; k < kCosScan; ++k) {
        const double t =
            static_cast<double>(k) / static_cast<double>(kCosScan - 1);
        const double c = cosMin + (1.0 - cosMin) * t;
        const double r = oracleDirectionalAlbedo(mixMat, c, kQuadTheta, kQuadPhi);
        if (k == 0 || r < rhoLo) rhoLo = r;
        if (k == 0 || r > rhoHi) rhoHi = r;
    }
    // Discretisation error, measured rather than assumed: the same
    // integral at half resolution in each dimension.
    const double rhoCoarse =
        oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta / 2u, kQuadPhi / 2u);
    const double rhoFine = oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta, kQuadPhi);
    const double quadError = std::abs(rhoFine - rhoCoarse);
    // 5 standard errors of the GPU sample mean, computed from the samples.
    const double mixSigma = furnaceStdErr[kMixtureRun];
    const double meanAllowance = 5.0 * mixSigma + quadError;
    const double mixLower = rhoLo - meanAllowance;
    const double mixUpper = rhoHi + meanAllowance;
    const double rhoMid = 0.5 * (rhoLo + rhoHi);
    const double sigmasOff =
        mixSigma > 0.0 ? (furnaceMean[kMixtureRun] - rhoMid) / mixSigma : 0.0;
    if (!(furnaceMean[kMixtureRun] >= mixLower) || !(furnaceMean[kMixtureRun] <= mixUpper)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: mixture furnace mean is %.9g, outside [%.9g, "
                     "%.9g]. The reference is rho_dir = INT f cos dL over the upper "
                     "hemisphere, integrated in double precision from the CPU oracle's own f "
                     "(not from the GLSL), and evaluated across this frame's |N.V| range: "
                     "[%.9g, %.9g]. The allowance is %.3g = 5 x the sample standard error "
                     "%.3g plus the measured quadrature error %.3g. The estimator is "
                     "unbiased for rho_dir for ANY lobe probability in (0,1) that covers f's "
                     "support, so landing outside means the mixture density and the branch "
                     "that was actually taken disagree, or a lobe has stopped being "
                     "reachable -- it is %+.1f sigma off\n",
                     furnaceMean[kMixtureRun], mixLower, mixUpper, rhoLo, rhoHi,
                     meanAllowance, mixSigma, quadError, sigmasOff);
        return false;
    }
    std::printf("[diff_gpu_probe] OK: mixture furnace (roughness 0.30, metallic 0.50, "
                "specularWeight 1.00 -- q in [%.4f, %.4f], so both branches run and every "
                "path forms the full mixture density and divides): all %u paths in [0, %.6f] "
                "(derived pointwise bound), mean %.9g matches the quadrature of the CPU "
                "oracle's own f, rho_dir in [%.6f, %.6f] over this frame's |N.V| range, to "
                "%+.2f sigma (allowance %.3g = 5 x sigma %.3g + quadrature error %.3g at "
                "%ux%u); min %.9g, max %.9g\n",
                qMin, qMax, kCapacity, mixPointwiseBound, furnaceMean[kMixtureRun], rhoLo,
                rhoHi, sigmasOff, meanAllowance, mixSigma, quadError, kQuadTheta, kQuadPhi,
                furnaceMin[kMixtureRun], furnaceMax[kMixtureRun]);
    return true;
}

}  // namespace ohao::diff::probe
