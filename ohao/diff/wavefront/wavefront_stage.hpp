#pragma once

#include "diff/wavefront/compute_pipeline.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace ohao::diff {

/// One dispatchable stage of the wavefront loop: a pipeline, its current
/// push-constant blob, and where its workgroup count comes from.
///
/// record() issues exactly the four calls every wavefront dispatch needs --
/// bind pipeline, bind descriptor set, push constants, dispatch -- and
/// nothing else. In particular it records NO BARRIERS. A stage records
/// *work*; WavefrontLoop (Task 3) records *ordering*, so that the barrier
/// set for the whole fused bounce loop is reviewable in one file. That
/// matters more than usual in this subsystem: synchronization validation
/// has been measured not to catch the compute storage-buffer and
/// indirect-command-read hazards this loop depends on getting right (see
/// the plan's "safety property" section), so the barriers are verified by a
/// human reading them, not by a tool. Do not add a barrier here even where
/// it would be convenient -- e.g. do not have record() order its own
/// dispatch against the previous one. That ordering belongs to whoever
/// calls record() twice in a row.
///
/// Ownership mirrors ComputePipeline (which this class holds by value, not
/// by pointer/reference -- see the class comment on why: composing a
/// non-copyable, non-movable member forces WavefrontStage to build its own
/// pipeline in place through the forwarding build()/bindBuffers()/
/// bindAccelerationStructure()/destroy() below, rather than being handed an
/// already-built one). Copy and move are deleted for the same reason
/// ComputePipeline deletes them: the compiler-generated versions would copy
/// raw Vulkan handles, leaving two objects that each believe they own (and
/// will each destroy) the same pipeline/descriptor set.
class WavefrontStage {
public:
    /// Dispatch a workgroup count known when record() is called (e.g.
    /// generate: fixed at width*height / 64 for the whole probe).
    ///
    /// Three components, not one: `wf_generate.comp` is `local_size(8,8)`,
    /// so a genuinely 2-D pixel grid (width x height, not just a single row
    /// of local_size_y pixels) needs a 2-D dispatch. `groupsY`/`groupsZ`
    /// default to 1, so every existing `Fixed{n}` call site -- which meant
    /// "dispatch (n,1,1)" -- keeps meaning exactly that; this is a strict
    /// widening of the type, not a behaviour change for 1-D callers.
    struct Fixed {
        uint32_t groups{0};
        uint32_t groupsY{1};
        uint32_t groupsZ{1};
    };

    /// Dispatch a workgroup count computed on the GPU by a prior stage
    /// (wf_prepare_indirect.comp) and read from `buffer` at byte `offset`
    /// -- the {groupCountX,1,1} VkDispatchIndirectCommand triple written
    /// into WavefrontBuffers' counter buffer at kIndirectArgsSlot. Ordering
    /// that write before this dispatch reads it is the caller's job
    /// (WavefrontLoop's SHADER_WRITE -> INDIRECT_COMMAND_READ barrier);
    /// WavefrontStage never records barriers itself (see class comment).
    struct Indirect {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceSize offset{0};
    };

    using GroupCountSource = std::variant<Fixed, Indirect>;

    WavefrontStage() = default;
    ~WavefrontStage() = default;  // m_pipeline (ComputePipeline) is its own backstop.

    WavefrontStage(const WavefrontStage&) = delete;
    WavefrontStage& operator=(const WavefrontStage&) = delete;
    WavefrontStage(WavefrontStage&&) = delete;
    WavefrontStage& operator=(WavefrontStage&&) = delete;

    // Forward directly to the owned ComputePipeline -- see
    // compute_pipeline.hpp for the full contract (SPV search order,
    // binding-index convention, reverse-order failure-path cleanup).
    // WavefrontStage adds nothing to this half of the lifecycle; it only
    // adds the push-constant blob and group-count source record() reads.
    [[nodiscard]] bool build(VkDevice device, const char* spvName,
                             std::span<const VkDescriptorType> bindings,
                             uint32_t pushConstantSize);

    /// Idempotent (ComputePipeline::destroy() is); also clears the
    /// push-constant blob and resets the group-count source to Fixed{0},
    /// so a destroyed-then-rebuilt stage cannot record with stale state
    /// from a previous build().
    void destroy(VkDevice device);

    [[nodiscard]] bool bindBuffers(VkDevice device, std::span<const VkBuffer> buffers);
    [[nodiscard]] bool bindAccelerationStructure(VkDevice device, uint32_t binding,
                                                 VkAccelerationStructureKHR accel);

    /// Copies `size` bytes from `data` into storage this stage owns. The
    /// copy (rather than storing the pointer) is what lets record() be
    /// called safely more than once -- e.g. once per bounce, with
    /// WavefrontLoop overwriting the ping-ponged queue/counter-slot fields
    /// between calls -- without requiring the caller to keep its source
    /// struct alive for as long as the stage might still be recorded.
    /// Pass size == 0 for a stage with no push constants; record() then
    /// skips vkCmdPushConstants entirely (a zero-size push is invalid
    /// Vulkan usage against a pipeline layout with no push-constant range,
    /// which is what build() produces when it was given pushConstantSize
    /// == 0 -- see compute_pipeline.cpp).
    void setPushConstants(const void* data, uint32_t size);

    /// Replaces the group-count source. Called once for a stage dispatched
    /// with a compile-time-known count (generate), or before every
    /// record() for a stage whose count a prior wf_prepare_indirect.comp
    /// dispatch just computed (intersect, scatter) -- see WavefrontLoop.
    void setGroupCount(GroupCountSource source);

    /// Binds this stage's pipeline and descriptor set, pushes its current
    /// push-constant blob (if non-empty), then dispatches -- directly for
    /// Fixed, via vkCmdDispatchIndirect for Indirect. Records ONLY work,
    /// never a barrier (see class comment).
    ///
    /// Precondition: the most recent build() on this object succeeded and
    /// was not followed by destroy(), AND a bindBuffers()/
    /// bindAccelerationStructure() call has succeeded since that build()
    /// (see m_bound below -- build() allocates the descriptor set but does
    /// not write it, so a built-but-never-bound stage has a non-null
    /// descriptor set whose bindings were never populated by
    /// vkUpdateDescriptorSets). Calling record() on a stage that was never
    /// built, was destroyed, or was built but never bound, is a caller bug:
    /// this method returns void rather than bool because every vkCmd* call
    /// it wraps is itself void and fails only through validation layers or
    /// device loss, and a bool return here would imply a caller could
    /// recover mid-command-buffer, which Vulkan does not support once one
    /// invalid call has been recorded into it. In every build (debug and
    /// release) the precondition is enforced twice: first by `assert` (a
    /// debug-time diagnostic that names the violated invariant; compiled
    /// out under NDEBUG, which this repo's Release test targets define),
    /// then unconditionally by an early return -- mirroring
    /// ArenaLayout::block()'s "assert, then unconditional guard" pattern
    /// (arena_layout.cpp) -- so a Release build that violates the
    /// precondition gets a safe no-op instead of calling
    /// vkCmdBindPipeline/vkCmdBindDescriptorSets with VK_NULL_HANDLE or
    /// against an unwritten descriptor set, both of which are undefined by
    /// the Vulkan spec and may or may not be caught by validation.
    void record(VkCommandBuffer cmd) const;

private:
    ComputePipeline m_pipeline;
    std::vector<std::byte> m_pushConstants;
    GroupCountSource m_groupCount{Fixed{0}};

    // Set true by a successful bindBuffers()/bindAccelerationStructure();
    // cleared by destroy() and by build() (even a successful one -- a
    // freshly (re)built stage's descriptor set is unwritten regardless of
    // whether a previous build()/bind() cycle on this object had set this
    // true). record() fails closed on this being false the same way it
    // fails closed on a null pipeline/layout/descriptor-set handle -- see
    // record()'s precondition doc above.
    bool m_bound{false};
};

}  // namespace ohao::diff
