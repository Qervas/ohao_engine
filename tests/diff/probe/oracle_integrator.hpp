// The independent CPU reference integrator (Stage 0b-2b Task 6, checks
// 33-34), and the small scene/statistics helpers it is built out of.
//
// Lifted verbatim out of diff_gpu_probe.cpp. The long argument for WHY this
// is a reference rather than ohao::PathTracer, and the exhaustive statement
// of what it shares with the GPU side and what it does not, lives in
// oracle_integrator.cpp above the code it describes.
#pragma once

#include "probe/oracle_bsdf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace ohao::diff {
// Defined in gpu_probe_context.hpp. Forward-declared here so that this
// header stays free of Vulkan; parityCameraRay's definition includes the
// real thing.
struct WavefrontGenerateCamera;
}  // namespace ohao::diff

namespace ohao::diff::probe {

/// One triangle in the reference intersector's own form.
struct ParityTriangle {
    OracleVec3 v0;
    OracleVec3 e1;  // v1 - v0
    OracleVec3 e2;  // v2 - v0
};

/// Mirror the shaders' ray constants: kParityRayTMax mirrors BOTH
/// wf_intersect.comp's kTraceTMax (the path ray) and wf_scatter.comp's
/// kShadowTMax (the shadow ray), which are the same number so that "this ray
/// found geometry" means the same thing for both; kParitySurfaceOffset
/// mirrors wf_scatter.comp's kSurfaceOffset. Named here (rather than inlined
/// into ParityRefScene's member initializers below) so
/// checkParityRefConstantsTie has a single C++-side value each to tie against
/// the shader sources at runtime -- the same enforcement checkNeeStrideTie
/// gives ohao::diff::kNeeSampleFloats. One difference from that check:
/// kNeeSampleFloats shares one NAME with the GLSL side, so a single regex
/// search finds both ends of the tie. The shader constants here do not share
/// a name with kParityRayTMax/kParitySurfaceOffset, so
/// checkParityRefConstantsTie names both ends explicitly (one regex per
/// shader constant, matched against one C++ constant each) rather than
/// searching for one shared identifier -- still a compiled, runtime-checked
/// tie, just not a name-derived one.
///
/// In a HEADER, `inline`, because checkParityRefConstantsTie now lives in
/// ties.cpp, a translation unit away. That is a linkage change, not a value
/// change: both are still the one C++-side number that tie reads.
inline constexpr double kParityRayTMax = 1000.0;
inline constexpr double kParitySurfaceOffset = 1e-4;

/// Everything the reference integrator needs about the scene, gathered so
/// the sample function takes one argument instead of nine.
struct ParityRefScene {
    std::vector<ParityTriangle> tris;
    const std::vector<double>* envLum{nullptr};
    uint32_t envW{0};
    uint32_t envH{0};
    OracleMaterial material{};
    uint32_t bounces{0};
    /// wf_intersect.comp's kTraceTMax and wf_scatter.comp's kShadowTMax --
    /// the same 1000 in both stages, so "this ray found geometry" means the
    /// same thing for a path ray and for a shadow ray. BOTH are tied to this
    /// value at runtime by checkParityRefConstantsTie, via kParityRayTMax.
    double rayTMax{kParityRayTMax};
    /// wf_scatter.comp's kSurfaceOffset, used for BOTH the shadow ray's
    /// origin and the continuation ray's, exactly as that shader derives
    /// both from one constant. Tied to the shader source at runtime by
    /// checkParityRefConstantsTie, via kParitySurfaceOffset.
    double surfaceOffset{kParitySurfaceOffset};
};

bool parityRayTriangle(const OracleVec3& origin, const OracleVec3& dir, const ParityTriangle& tri,
                       double tMax, double& outT);
int parityTraceNearest(const std::vector<ParityTriangle>& tris, const OracleVec3& origin,
                       const OracleVec3& dir, double tMax, double& outT);
bool parityOccluded(const std::vector<ParityTriangle>& tris, const OracleVec3& origin,
                    const OracleVec3& dir, double tMax);

void parityBasis(const OracleVec3& n, OracleVec3& t, OracleVec3& b);
OracleVec3 parityCosineSample(const OracleVec3& n, double u1, double u2);

double parityEnvRadiance(const OracleVec3& dir, const std::vector<double>& envLum, uint32_t envW,
                         uint32_t envH);
double parityNextU(std::mt19937_64& rng);

double parityReferenceSample(const ParityRefScene& scene, const OracleVec3& camOrigin,
                             const OracleVec3& camDir, std::mt19937_64& rng);

OracleVec3 parityCameraRay(uint32_t px, uint32_t py, uint32_t width, uint32_t height,
                           const ohao::diff::WavefrontGenerateCamera& cam);

void parityAddQuad(std::vector<float>& positions, std::vector<uint32_t>& indices,
                   const std::array<std::array<float, 3>, 4>& corners);
std::vector<ParityTriangle> parityTrianglesFromSoup(const std::vector<float>& positions,
                                                    const std::vector<uint32_t>& indices);

void parityMoments(const double* values, std::size_t n, double& outMean, double& outVar);
void parityMomentsFromSums(double sum, double sumSq, std::size_t n, double& outMean,
                           double& outVar);

}  // namespace ohao::diff::probe
