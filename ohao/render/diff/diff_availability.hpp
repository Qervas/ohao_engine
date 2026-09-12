// Can this device run the differentiable renderer, and if not, what is
// missing? Stage 5, item 1: the engine's first call into `ohao_diff`.
//
// WHY THIS IS THE FIRST THING THE ENGINE ASKS. The differentiable renderer
// needs two optional Vulkan features -- VK_KHR_ray_query and
// shaderBufferFloat32AtomicAdd -- and neither is guaranteed. An engine that
// discovers that by dispatching a compute shader and getting nothing back
// has already built five pipelines, an acceleration structure and a
// gradient arena for a device that was never going to run them. So the
// question comes first, it is cheap, and its answer is a sentence a user
// can act on rather than a boolean.
//
// AND IT MAKES ohao_diff A DEPENDENCY OF ohao_renderer, which is the point
// of the slice. Until this file existed, every line of the differentiable
// renderer was linked only by its own tests: the library built, its gates
// passed, and nothing that ships called it. A subsystem in that state is
// not integrated however good its tests are, because the link itself --
// include paths, symbol visibility, the order the static libraries resolve
// in -- had never been exercised by the thing that has to do it.
//
// WHAT THIS IS NOT. It does not create a DiffRenderer, own an arena, or
// render anything. Those come with the features that need them (spec's
// sensitivity maps and renderer fitting); building their plumbing now, with
// no caller, would be inventing an integration rather than doing one.
#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace ohao {

/// What `ohao::diff` needs from a physical device, as the engine reports it.
struct DiffAvailability {
    /// VK_KHR_ray_query. The wavefront's intersect and shadow stages are
    /// both ray queries from compute; there is no raster fallback.
    bool rayQuery{false};
    /// shaderBufferFloat32AtomicAdd. Gradients are scattered by atomicAdd
    /// into a buffer, and the scatter is the whole backward pass.
    ///
    /// BUFFER, not image, and the distinction is not pedantry: image float
    /// atomics are materially less available -- the Intel iGPU this was
    /// developed against reports buffer true and image false -- and
    /// `ohao::diff` never uses them, because spec 4.3 puts gradients in
    /// buffers precisely so it would not have to.
    bool bufferFloat32AtomicAdd{false};
    /// Both of the above. There is no partial mode: a device missing either
    /// cannot run the backward pass at all.
    bool available{false};
    /// Empty when available. Otherwise names what is missing, in a form
    /// worth putting in front of a person.
    std::string reason;
};

/// Ask the device. Safe to call on a null handle, which reports unavailable
/// with a reason rather than crashing -- an engine may well query before it
/// has picked a device, and a capability probe that requires the thing it is
/// probing for is not much of a probe.
[[nodiscard]] DiffAvailability queryDiffAvailability(VkPhysicalDevice physicalDevice);

}  // namespace ohao
