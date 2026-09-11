// Stage 5, check 66: WHO OWNS THE NUMBER.
//
// `DiffRenderer`'s header claims that the engine object owns a parameter's
// value and the registry owns only the gradient and the optimiser state, and
// it names the failure that claim avoids. This check stops that being a
// paragraph.
//
// THE MECHANICS BEING GATED ARE REAL, and they are PathTracer's
// (`path_tracer_images.cpp`):
//
//     void PathTracer::setMaterialData(const std::vector<glm::vec4>& materials) {
//         for (size_t i = 0; i < materials.size() && i < m_materialData.size(); i++)
//             m_materialData[i] = materials[i];
//         ... vkMapMemory + memcpy(mapped, m_materialData.data(), size) ...
//     }
//
// `m_materialData` is a CPU-side vector and is the authority; the GPU buffer
// is a memcpy of it, refreshed on every call. So ANY later edit -- a user
// dragging one slider, touching one unrelated element -- rewrites the WHOLE
// buffer from the CPU array.
//
// That is what makes writing the GPU buffer directly a silent data-loss bug
// rather than a style preference: the optimised value survives only until the
// next ordinary edit of any other element, and then reverts with nothing
// logged and nothing failing.
//
// The check runs both protocols against a faithful reproduction of those
// mechanics and requires them to DIFFER: the correct one survives an
// unrelated edit, the direct-write one is reverted by it. The second is the
// demonstrated failure.
//
// WHAT THIS DOES NOT CLAIM. It gates the PROTOCOL against a reproduction of
// setMaterialData's map-and-memcpy, not against PathTracer itself -- standing
// one up needs its images and pipelines. The same protocol against the real
// class is still owed, and is Task 4's business.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkOwnership(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
