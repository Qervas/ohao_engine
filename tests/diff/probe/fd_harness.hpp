// The finite-difference harnesses: Task 2's CRN instrument, Task 3's
// detached instrument, and Task 4/5's emission and emission-texture
// instruments.
//
// They are one module because they share the `h`-derivation model: each
// derives its step size from the same two error terms (a roundoff term set by
// the film's relative accuracy and a truncation term set by the order of the
// difference), and each reports the bound that derivation gives alongside the
// measurement. fd_harness.cpp states each derivation in full, above the code
// that implements it.
//
// Lifted verbatim out of diff_gpu_probe.cpp.
#pragma once

#include "gpu_probe_context.hpp"

#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ohao::diff::probe {

struct CrnFdMeasurement {
    double jMinus{0.0};
    double jCenter{0.0};
    double jPlus{0.0};
    /// Half the difference of the two floats ACTUALLY pushed, in double. Used
    /// as the divisor instead of the requested step so that the representation
    /// error of (a +/- h) cancels out of the quotient exactly.
    double hActual{0.0};
    double finiteDiff{0.0};
    double analytic{0.0};
    double absError{0.0};
    double relError{0.0};
    double roundoffBound{0.0};
    double truncationBound{0.0};
    double errorBound{0.0};
    /// Floats of the trace's (origin, dir, hitT) slots that differ between a
    /// perturbed render and the centre one, summed over every perturbed
    /// render and every path -- Task 3's `traceGeometryMismatches`, applied
    /// here by `measureCrnEmissionGradient` to a PLAIN perturbation (no
    /// sampling override) to MEASURE rather than assume that plain CRN is
    /// valid for a new parameter. 0 for every caller that never asked for it
    /// (`measureCrnAlbedoGradient` does not fill this field), so its
    /// presence on this struct changes nothing about check 37/38.
    std::size_t traceMismatches{0};
};

struct GgxFdMeasurement {
    double jMinus2{0.0};
    double jMinus{0.0};
    double jCenter{0.0};
    double jPlus{0.0};
    double jPlus2{0.0};
    /// Half the difference of the two floats ACTUALLY pushed at +/-h, in
    /// double -- the divisor, so the representation error of theta +/- h
    /// cancels out of the quotient. Task 2's `hActual`, same reason.
    double hActual{0.0};
    double hActual2{0.0};
    double finiteDiff{0.0};    // D(h)
    double finiteDiff2h{0.0};  // D(2h)
    double analytic{0.0};
    double absError{0.0};
    double relError{0.0};
    double roundoffBound{0.0};
    double truncationBound{0.0};
    double errorBound{0.0};
    /// Floats of the trace's (origin, dir, hitT) slots that differ between a
    /// perturbed render and the centre one, summed over all four perturbed
    /// renders and every path. 0 <=> every render walked the identical path.
    std::size_t traceMismatches{0};
    /// The naive counterpart of `finiteDiff`, filled only when the caller
    /// asked for the un-frozen measurement.
    bool sampledDetached{true};
};

/// THE HOST'S BILINEAR FOOTPRINT -- the texels a uv touches and the weight
/// each gets. Written from the convention, not called out of the shader; see
/// fd_harness.cpp, where hostBilinearFootprint's own comment states why.
struct HostBilinearFootprint {
    std::uint32_t x0, x1, y0, y1;
    float w00, w10, w01, w11;
};

/// Sum of every float in a film, in double.
double filmTotal(const std::vector<float>& film);

/// Compares the (origin, dir, hitT) slots of two vertex traces and returns
/// the number of floats that differ, compared as BITS.
std::size_t traceGeometryMismatches(const std::vector<float>& a, const std::vector<float>& b,
                                    uint32_t capacity);

/// Task 2's CRN instrument: three renders, one central difference.
bool measureCrnAlbedoGradient(ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf,
                              uint32_t width, uint32_t height, uint32_t bounces,
                              const ohao::diff::WavefrontGenerateCamera& camera,
                              const std::vector<float>& positions,
                              const std::vector<uint32_t>& indices, float albedo, float step,
                              const ohao::diff::WavefrontScatterMaterial& material, uint32_t seed,
                              ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                              uint32_t gradArenaFloats, uint32_t gradAlbedoOffset,
                              double filmRelativeEps, CrnFdMeasurement& out);

/// Task 3's detached instrument: five renders, D(h) and D(2h).
bool measureDetachedGgxGradient(ohao::diff::GpuProbeContext& ctx,
                                ohao::diff::WavefrontBuffers& wf, uint32_t width, uint32_t height,
                                uint32_t bounces, const ohao::diff::WavefrontGenerateCamera& camera,
                                const std::vector<float>& positions,
                                const std::vector<uint32_t>& indices, float albedo,
                                const ohao::diff::WavefrontScatterMaterial& material,
                                uint32_t param, float step, bool freezeSampling, uint32_t seed,
                                ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                                uint32_t gradArenaFloats, uint32_t gradOffset,
                                double filmRelativeEps, GgxFdMeasurement& out);

/// Task 4's emission instrument.
bool measureCrnEmissionGradient(ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf,
                                uint32_t width, uint32_t height, uint32_t bounces,
                                const ohao::diff::WavefrontGenerateCamera& camera,
                                const std::vector<float>& positions,
                                const std::vector<uint32_t>& indices, float albedo,
                                const ohao::diff::WavefrontScatterMaterial& material,
                                float emission, float step, uint32_t seed,
                                ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                                uint32_t gradArenaFloats, uint32_t gradEmissionOffset,
                                double filmRelativeEps, CrnFdMeasurement& out);

/// Fills WavefrontGradientOptions' seven Task 5 fields, so that the three
/// places that configure an emission-texture render cannot drift into
/// configuring three slightly different ones.
ohao::diff::WavefrontGradientOptions emissionTextureOptions(
    const std::vector<float>& texels, const ohao::diff::ParamShape& shape, float uvScaleU,
    float uvScaleV, float uvBiasU, float uvBiasV);

HostBilinearFootprint hostBilinearFootprint(float u, float v, std::uint32_t width,
                                            std::uint32_t height);

/// Task 5's emission-texel instrument.
bool measureCrnEmissionTexelGradient(
    ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf, uint32_t width,
    uint32_t height, uint32_t bounces, const ohao::diff::WavefrontGenerateCamera& camera,
    const std::vector<float>& positions, const std::vector<uint32_t>& indices, float albedo,
    const ohao::diff::WavefrontScatterMaterial& material, const std::vector<float>& baseTexels,
    const ohao::diff::ParamShape& shape, float uvScaleU, float uvScaleV, float uvBiasU,
    float uvBiasV, uint32_t element, float step, uint32_t seed, ohao::diff::GradientArena& arena,
    std::size_t gradBlockIndex, uint32_t gradArenaFloats, uint32_t gradTexOffset,
    double filmRelativeEps, CrnFdMeasurement& out);

}  // namespace ohao::diff::probe
