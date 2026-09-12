// THE SCATTER PASS'S OWN SCRATCH, and the first thing a record-only gradient
// entry point forces out of the test harness.
//
// `runWavefrontGradientProbe` allocates these inline and reads them back
// itself, which works only because it SUBMITS. A library entry point must not
// submit -- `DiffRenderer`'s contract is that it records into a
// caller-supplied command buffer, exactly as `PathTracer::render` does -- so
// the buffers the caller reads after its own submit cannot be locals of the
// recording function. They have to be an object the caller holds.
//
// TWO INDEPENDENT SETS, one per instantiation of the traversal. Nothing the
// REPLAY run writes may reach a byte the FORWARD run's film is read out of:
// the two are separate evaluations of the same paths, and sharing a sink would
// let one overwrite what the other's gradient was computed against. That is
// the invariant the replay probe was built to establish, and it is structural
// here rather than a comment -- there are simply two.
//
// `trace` and `film` are HOST-READABLE and persistently mapped; `env` and
// `nee` are not. The asymmetry is not an oversight: the frozen-direction
// measurement reads the forward run's vertex trace back and compares two
// renders' ray origins, directions and hit distances BIT FOR BIT, which is how
// the detached instrument's defining claim is measured rather than argued.
// `env` and `nee` are bound because a descriptor set must cover every binding
// the shader statically uses, and are never read.
#pragma once

#include "gpu/vulkan/gpu_allocator.hpp"

#include <cstdint>

namespace ohao::diff {

/// The four sinks one instantiation of the scatter traversal writes.
struct ScatterSinkSet {
    GpuBuffer trace{};  ///< per-path vertex trace, host-readable
    GpuBuffer env{};    ///< environment samples, bound but never read
    GpuBuffer nee{};    ///< NEE samples, bound but never read
    GpuBuffer film{};   ///< the radiance film, host-readable
};

/// Both instantiations' sinks, created and released together.
class ScatterSinks {
public:
    ScatterSinks() = default;
    ~ScatterSinks() = default;

    // Not copyable or movable, for GpuBuffer's reason: the compiler versions
    // would leave two objects each believing they own the same allocations.
    ScatterSinks(const ScatterSinks&) = delete;
    ScatterSinks& operator=(const ScatterSinks&) = delete;
    ScatterSinks(ScatterSinks&&) = delete;
    ScatterSinks& operator=(ScatterSinks&&) = delete;

    /// The per-path strides of the three non-film sinks, in floats.
    ///
    /// PASSED IN, NOT DEFINED HERE, and that is deliberate. These numbers are
    /// tied to the shader's own writes by runtime checks that parse
    /// wf_scatter.comp and refuse to run the probe if the two disagree
    /// (`checkNeeStrideTie`, `checkWfScatterSinkLayoutTie`). They were bare
    /// literals on both sides of that boundary once, with a silent
    /// wrong-slot read as the failure mode. A copy of them here would be a
    /// THIRD place the same fact lives and the one place nothing checks, so
    /// this class allocates what it is told and claims to know nothing about
    /// the layout.
    struct Strides {
        std::uint32_t traceFloats{0};
        std::uint32_t envFloats{0};
        std::uint32_t neeFloats{0};
        [[nodiscard]] bool valid() const noexcept {
            return traceFloats > 0 && envFloats > 0 && neeFloats > 0;
        }
    };

    /// `capacity` paths and a `width` x `height` film. Fails, and releases
    /// whatever it had allocated, if any buffer could not be created or any
    /// stride is zero.
    [[nodiscard]] bool create(GpuAllocator& allocator, std::uint32_t capacity,
                              std::uint32_t width, std::uint32_t height,
                              const Strides& strides);
    void destroy(GpuAllocator& allocator);

    [[nodiscard]] const ScatterSinkSet& forward() const noexcept { return m_forward; }
    [[nodiscard]] const ScatterSinkSet& replay() const noexcept { return m_replay; }
    [[nodiscard]] ScatterSinkSet& forward() noexcept { return m_forward; }
    [[nodiscard]] ScatterSinkSet& replay() noexcept { return m_replay; }
    [[nodiscard]] bool valid() const noexcept;

private:
    [[nodiscard]] bool createSet(GpuAllocator& allocator, ScatterSinkSet& set,
                                 std::uint32_t capacity, std::uint32_t width,
                                 std::uint32_t height, const Strides& strides);

    ScatterSinkSet m_forward;
    ScatterSinkSet m_replay;
};

}  // namespace ohao::diff
