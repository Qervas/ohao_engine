// Direct-lighting and film checks 29-32: NEE/BSDF/MIS agreement, the
// per-sample MIS partition, the envIntegral/pdfEnvMap/routing claims, and
// radiance accumulation into the film.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkNeeMisAndRouting(ohao::diff::GpuProbeContext& ctx);

bool checkFilmAccumulation(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
