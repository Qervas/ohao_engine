// STAGE 5 -- THE FACADE THAT MAKES THIS MODULE SOMETHING THE ENGINE CAN USE.
//
// Everything through Stage 3 is validated against buffers the TEST HARNESS
// creates. `ohao_diff` is linked by `diff_unit_tests` and `diff_gpu_probe` and
// by nothing else. The mathematics is not what this stage risks; what it risks
// is everything the probe was quietly providing -- lifetime, ownership, and the
// question of who may write a buffer while somebody else reads it.
//
// PATHTRACER IS THE SHAPE THIS COPIES, because it solved the same problem
// already. It is handed a command buffer and an acceleration structure; it does
// not own a submit, does not own the frame, and does not decide when it runs.
// `DiffRenderer` is its sibling and is deliberately indistinguishable in those
// respects.
//
//   * A step RECORDS into a caller-supplied VkCommandBuffer. The caller chooses
//     whether that is the frame's buffer or a separate submit, and the choice
//     can differ between the editor and a headless fit without this class
//     changing. NOTHING HERE SUBMITS OR WAITS.
//
//   * THE ENGINE OBJECT OWNS THE PARAMETER'S VALUE. This registry owns the
//     gradient and the optimiser state, and never the value itself.
//     `PathTracer::setMaterialData` takes a CPU-side array and uploads it, so
//     the GPU material buffer is a DERIVED artefact of a CPU authority the
//     scene already maintains. A step is therefore: read the gradient, let
//     Adam update the CPU-side value, re-upload through the engine's own path.
//
//     The alternative is the obvious design and it is wrong. A registry holding
//     the value and writing the GPU buffer directly would leave the scene's CPU
//     copy stale, and the next ordinary material edit -- a user dragging a
//     slider -- would silently overwrite the optimised value with the
//     pre-optimisation one. Two authorities for one number always resolve to
//     whichever wrote last.
//
//   * The caller must not step while a frame reading those resources is in
//     flight. Stated here because a contract not written down is not a
//     contract.
//
// THE LIFECYCLE IS A THREE-STATE MACHINE, NOT A BOOL, and that is forced by
// the arena rather than chosen: `GradientArena::build` consumes an
// `ArenaLayout`, and the layout is finished only once every parameter is
// registered. So registration and stepping are not merely ordered, they are
// mutually exclusive phases:
//
//     Uninitialised --init()--> Configuring --build()--> Ready
//          ^                                               |
//          +-------------------- shutdown() ---------------+
//
// Registering after `build()` is REFUSED rather than silently relaid out: a
// late registration would move every block that follows it, and the gradient
// a kernel is mid-flight writing would land in a different parameter. That is
// the failure check 61's null test exists to catch, and it is cheaper to make
// it unreachable.
#pragma once

#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace ohao::diff {

class DiffRenderer {
public:
    enum class State : std::uint8_t {
        Uninitialised,  ///< No device. Nothing but init() is legal.
        Configuring,    ///< Parameters may be registered; nothing may be recorded.
        Ready,          ///< The arena exists; parameters are frozen.
    };

    DiffRenderer() = default;
    ~DiffRenderer();

    // Not copyable or movable, for GradientArena's reason: the compiler
    // versions would leave two objects each believing they own one arena.
    DiffRenderer(const DiffRenderer&) = delete;
    DiffRenderer& operator=(const DiffRenderer&) = delete;
    DiffRenderer(DiffRenderer&&) = delete;
    DiffRenderer& operator=(DiffRenderer&&) = delete;

    /// Uninitialised -> Configuring. Both handles must be non-null; this does
    /// not create a device, exactly as PathTracer::init does not.
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physicalDevice);

    /// Configuring -> Ready. Builds the arena from the registered layout.
    /// Fails if nothing has been registered: an empty arena is a caller error,
    /// not an empty optimisation.
    [[nodiscard]] bool build(GpuAllocator& allocator);

    /// Any state -> Uninitialised. Idempotent.
    ///
    /// TAKES A POINTER, and it may be null. Two reasons, and the second is the
    /// one that matters: a DiffRenderer that never reached Ready has no arena
    /// and therefore nothing an allocator could release, so requiring one would
    /// make the whole state machine untestable without a device -- and a
    /// lifecycle that can only be exercised on a GPU is a lifecycle that will
    /// be exercised rarely.
    ///
    /// Returns FALSE, and leaves the state alone, if an arena exists and no
    /// allocator was supplied. That combination is a caller error which would
    /// otherwise leak a device buffer silently; refusing to complete makes the
    /// leak visible at the call site instead of at the next allocation
    /// failure.
    [[nodiscard]] bool shutdown(GpuAllocator* allocator);

    /// Registration. Legal ONLY in Configuring -- see the header note on why a
    /// late registration is refused rather than accommodated. The returned
    /// RegisterResult carries `ok`, the id, and an error string; a call in the
    /// wrong state returns ok == false with the state named in `error`.
    RegisterResult registerTexture(std::string name, ParamShape shape, VkFormat primalFormat);
    RegisterResult registerScalarBlock(std::string name, std::uint32_t floatCount);
    RegisterResult registerVertexPositions(std::string name, std::uint32_t vertexCount,
                                           std::uint32_t componentsPerVertex);

    /// RECORDS a clear of every gradient block into `cmd`. Does not submit.
    ///
    /// Called at the start of each step, unconditionally, because
    /// `GradientArena::build` does NOT zero and engine buffers are recycled far
    /// more aggressively than the probe's ever were. Check 61 read a destroyed
    /// position buffer's contents back as a gradient exactly once, which was
    /// enough.
    [[nodiscard]] bool zeroGradients(VkCommandBuffer cmd);

    /// Kingma & Ba's hyperparameters. The defaults are the paper's.
    struct AdamSettings {
        float alpha{1e-3f};
        float beta1{0.9f};
        float beta2{0.999f};
        float epsilon{1e-8f};
    };

    /// RECORDS one Adam step for a registered parameter into `cmd`. Does not
    /// submit, does not wait.
    ///
    /// `values` is the buffer holding the parameter's primal floats, and it is
    /// updated IN PLACE by the kernel. The gradient and the optimiser state
    /// come from THE ARENA, at this parameter's registered block offsets --
    /// which is the whole reason optimizer_adam.comp grew gradOffset and
    /// stateOffset, and what makes this a two-binding-and-two-integers
    /// operation rather than three buffer copies.
    ///
    /// `stepIndex` is Kingma & Ba's t and is 1-BASED: the bias correction
    /// divides by 1 - beta^t, which is zero at t = 0.
    ///
    /// WHOSE BUFFER `values` IS is the caller's decision and a consequential
    /// one -- see the note on ownership at the top of this file. Passing the
    /// engine's own live buffer is spec 4.3's model and costs no readback;
    /// passing a scratch buffer seeded from a CPU-side authority costs a
    /// readback per step but keeps one authority for the number. For a dozen
    /// scalars the readback is free and the second is right; for a texture it
    /// is not. This class deliberately does not choose for you.
    [[nodiscard]] bool recordAdamStep(VkCommandBuffer cmd, ParamId id, VkBuffer values,
                                      const AdamSettings& settings, std::uint32_t stepIndex);

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool ready() const noexcept { return m_state == State::Ready; }
    [[nodiscard]] const ParamRegistry& registry() const noexcept { return m_registry; }
    [[nodiscard]] const GradientArena& arena() const noexcept { return m_arena; }
    [[nodiscard]] GradientArena& arena() noexcept { return m_arena; }

    /// The name of a state, for error messages and for tests to print.
    [[nodiscard]] static const char* stateName(State s) noexcept;

private:
    [[nodiscard]] RegisterResult refuseUnlessConfiguring(const char* what) const;

    /// Built lazily on the first recordAdamStep and kept: rebuilding a
    /// pipeline per optimiser step would dominate the step itself.
    [[nodiscard]] bool ensureAdamStage();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    State m_state{State::Uninitialised};
    ParamRegistry m_registry;
    GradientArena m_arena;
    WavefrontStage m_adamStage;
    bool m_adamStageBuilt{false};
};

}  // namespace ohao::diff
