// The independent CPU BSDF oracle (Stage 0b-2b Task 2, check 20).
//
// Lifted verbatim out of diff_gpu_probe.cpp. The provenance of every formula
// -- which published equation it was written from, and why that matters --
// is stated in oracle_bsdf.cpp, immediately above the code it describes.
// That commentary is what makes this an ORACLE rather than a transcription of
// the GLSL it validates, so it lives with the code, not here.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ohao::diff::probe {

inline constexpr double kOraclePi = 3.14159265358979323846;

struct OracleVec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

/// The material exactly as bsdf.glsl's header states it.
struct OracleMaterial {
    OracleVec3 baseColor{1.0, 1.0, 1.0};
    double roughness{1.0};
    double metallic{0.0};
    double specularWeight{0.0};
};

/// One direction binned back to an equirectangular texel, HOST-SIDE.
struct OracleEnvTexel {
    /// Polar and azimuthal angle of the QUERY direction.
    double theta{0.0};
    double phi{0.0};
    /// Texel-space coordinates: fx in [0, W], fy in [0, H]. A direction that
    /// equirectPixelToDir emitted lands on a texel CENTRE, i.e. a half-
    /// integer in both.
    double fx{0.0};
    double fy{0.0};
    /// The texel those coordinates fall in, clamped into range.
    int ix{0};
    int iy{0};
    /// Row-major index of that texel: iy * W + ix.
    std::size_t index{0};
    /// Chebyshev distance from the centre of that texel, in texel units.
    /// Zero exactly when the direction is a texel centre.
    double centreError{0.0};
};

/// bsdf.glsl's DIFF_BSDF_MIN_COS. The shader treats a view or light cosine at
/// or below this as grazing and refuses the specular math; this oracle
/// rejects at 0, because the physics does. The two thresholds are therefore
/// NOT the same, and the band (0, 1e-4] is a documented disagreement rather
/// than a bug -- see the rejection branch in check 20, which names it
/// explicitly instead of letting it surface as a spurious failure.
///
/// At namespace scope in a HEADER (rather than the file-static it was) so
/// that ties.cpp's checkBsdfShaderConstantTies, now a translation unit away,
/// still ties the shader's spelling against this one value. That is a
/// linkage change, not a value change.
inline constexpr double kShaderGrazingCos = 1e-4;

/// bsdf_probe.comp's output stride: floats per CASE in its binding-0 sink.
/// Shared with checkBsdfShaderConstantTies so the shader's own
/// `pc.outIndex * <N>u` has one host-side value to tie against -- it was a
/// `12` on each side of the GLSL/C++ boundary with only a trailing comment
/// between them.
inline constexpr std::uint32_t kBsdfProbeFloatsPerCase = 12;

OracleVec3 oracleAdd(const OracleVec3& a, const OracleVec3& b);
OracleVec3 oracleScale(const OracleVec3& a, double s);
double oracleDot(const OracleVec3& a, const OracleVec3& b);
OracleVec3 oracleCross(const OracleVec3& a, const OracleVec3& b);
OracleVec3 oracleNormalize(const OracleVec3& a);

double oracleGgxD(double NdotH, double alpha);
double oracleSmithLambda(double cosTheta, double alpha);
double oracleSmithG1(double cosTheta, double alpha);
double oracleSmithG2(double NdotV, double NdotL, double alpha);
double oracleSchlick(double f0, double cosTheta);

OracleVec3 oracleF0(const OracleMaterial& m);
double oracleSpecScale(const OracleMaterial& m);
double oracleSpecProb(const OracleMaterial& m, double NdotV);

void oracleBsdfEval(const OracleVec3& N, const OracleVec3& V, const OracleVec3& L,
                    const OracleMaterial& m, OracleVec3& outF, double& outPdf);
double oracleDirectionalAlbedo(const OracleMaterial& m, double cosThetaV, std::uint32_t nTheta,
                               std::uint32_t nPhi);

void oracleFrame(const OracleVec3& n, OracleVec3& t, OracleVec3& b);
OracleVec3 oracleDirFromAngles(const OracleVec3& n, double theta, double phi);
OracleVec3 oracleCosineHemisphere(const OracleVec3& n, double u1, double u2);
double oracleDistance(const OracleVec3& a, const OracleVec3& b);

OracleEnvTexel oracleEnvTexelOf(double dx, double dy, double dz, std::uint32_t envW,
                                std::uint32_t envH);

double oracleRelDiff(double reference, double measured);

}  // namespace ohao::diff::probe
