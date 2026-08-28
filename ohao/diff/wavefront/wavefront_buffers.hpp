#pragma once

#include "diff/wavefront/path_state_layout.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ohao::diff {

/// The buffers a wavefront bounce dispatch reads and writes: three it reads
/// AND writes (state, queue, counter) plus the two READ-ONLY environment CDF
/// buffers the scatter stage importance-samples.
///
/// Path state cannot live in registers across a dispatch boundary (each
/// bounce is a separate `vkCmdDispatch`), so it lives in `state`. The set of
/// paths still alive going into a bounce is handed off via `queue`, and dead
/// paths are compacted out so the next dispatch does not pay for them --
/// `counter` holds the live counts that drive that compaction and (in a
/// later task) an indirect dispatch group count.
///
/// Ownership mirrors GradientArena: build()/destroy() are the explicit
/// lifecycle, the destructor is a backstop for early-return error paths, and
/// copy/move are deleted because the compiler-generated versions would copy
/// the raw GpuBuffer handles, leaving two objects that each believe they own
/// (and will each destroy) the same buffers.
class WavefrontBuffers {
public:
    WavefrontBuffers() = default;
    ~WavefrontBuffers();

    WavefrontBuffers(const WavefrontBuffers&) = delete;
    WavefrontBuffers& operator=(const WavefrontBuffers&) = delete;
    WavefrontBuffers(WavefrontBuffers&&) = delete;
    WavefrontBuffers& operator=(WavefrontBuffers&&) = delete;

    /// Number of uint slots reserved in the counter buffer. Slots 0 and 1
    /// are the current/next queue live-path counts; slots 2-4 hold the
    /// {groupCountX, groupCountY, groupCountZ} vkCmdDispatchIndirect triple
    /// wf_prepare_indirect.comp writes (Task 5) -- VkDispatchIndirectCommand
    /// is a plain uint32 {x,y,z} triple, so this offset is bound directly as
    /// the indirect buffer with no separate copy. Slot 5 is a canary counter
    /// wf_intersect.comp increments on every invocation that actually runs,
    /// used to prove an indirect dispatch sized from a live-count of 0
    /// launches zero workgroups. Slots 6-7 remain reserved for future
    /// stages.
    static constexpr std::uint32_t kCounterSlotCount = 8;
    static constexpr std::uint32_t kCurrentCountSlot = 0;
    static constexpr std::uint32_t kNextCountSlot = 1;
    static constexpr std::uint32_t kIndirectArgsSlot = 2;
    static constexpr std::uint32_t kCanarySlot = 5;

    /// Dimensions the environment CDF buffers are allocated for when a
    /// caller does not name any. A 1x1 map is the "no environment worth
    /// speaking of" placeholder: it exists so that every scatter descriptor
    /// set has something valid bound at bindings 4 and 5 whether or not the
    /// caller cares about environment sampling, NOT because a 1x1
    /// equirectangular map is a sensible environment. It is not: the
    /// midpoint solid angle of its single texel is 2*pi^2, not the sphere's
    /// 4*pi, so the pdf `sampleEnvMap` returns for it does not integrate to
    /// 1. Nothing may treat the default as a uniform environment.
    static constexpr std::uint32_t kDefaultEnvWidth = 1;
    static constexpr std::uint32_t kDefaultEnvHeight = 1;

    /// `envWidth`/`envHeight` size the environment CDF buffers (see
    /// uploadEnvironment). They are fixed for the lifetime of the build:
    /// changing the map's resolution means a rebuild, which is the same
    /// contract `capacity` has.
    [[nodiscard]] bool build(GpuAllocator& allocator, std::uint32_t capacity,
                             std::uint32_t envWidth = kDefaultEnvWidth,
                             std::uint32_t envHeight = kDefaultEnvHeight);
    void destroy(GpuAllocator& allocator);

    /// Replaces the environment CDFs the scatter stage importance-samples.
    ///
    /// This class does NOT build the CDFs. `ohao::EnvCDF`
    /// (ohao/render/rt/env_cdf.cpp) already builds exactly the pair
    /// `shaders/includes/rt/env_sampling.glsl` binary-searches -- inclusive
    /// per-row-normalised conditional, inclusive marginal, both weighted by
    /// sin(theta) -- and it is what the RT pipeline uploads. Taking the
    /// finished arrays here rather than re-deriving them means the two
    /// pipelines cannot disagree about the CDF convention, and it keeps
    /// `ohao_diff` from having to link `ohao_renderer` for one function.
    ///
    /// `marginalCdf` must hold exactly `envHeight()` floats and
    /// `conditionalCdf` exactly `envWidth() * envHeight()`; a mismatch is
    /// rejected rather than truncated, because a short upload would leave
    /// the tail of a CDF at whatever the buffer previously held and the
    /// binary search would still return an in-range index for it.
    /// `integral` is passed through to the shader for Task 4's benefit --
    /// `sampleEnvMap` itself does not read it.
    [[nodiscard]] bool uploadEnvironment(std::span<const float> marginalCdf,
                                         std::span<const float> conditionalCdf, float integral);

    /// Records a fill over the state, queue and counter buffers plus a
    /// TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier, mirroring
    /// GradientArena::zero.
    ///
    /// The environment CDF buffers are deliberately NOT filled: they are
    /// read-only input data uploaded once, not per-run scratch, and zeroing
    /// them would leave a CDF that is 0 everywhere -- which
    /// `searchMarginal`/`searchConditional` answer with an in-range index
    /// and a zero pdf rather than an error. A caller that re-zeroes between
    /// runs (every probe here does) keeps its environment across them.
    void zero(VkCommandBuffer cmd);

    [[nodiscard]] VkBuffer stateBuffer() const noexcept { return m_stateBuffer.buffer; }
    [[nodiscard]] VkBuffer queueBuffer() const noexcept { return m_queueBuffer.buffer; }
    [[nodiscard]] VkBuffer counterBuffer() const noexcept { return m_counterBuffer.buffer; }

    /// The two environment CDF buffers, bound READ-ONLY by the scatter
    /// stage (bindings 4 and 5). No dispatch writes them, which is why they
    /// do not belong in `WavefrontLoop::record`'s `extraBarrierBuffers` --
    /// that parameter orders buffers the dispatches WRITE.
    [[nodiscard]] VkBuffer envMarginalBuffer() const noexcept {
        return m_envMarginalBuffer.buffer;
    }
    [[nodiscard]] VkBuffer envConditionalBuffer() const noexcept {
        return m_envConditionalBuffer.buffer;
    }
    [[nodiscard]] std::uint32_t envWidth() const noexcept { return m_envWidth; }
    [[nodiscard]] std::uint32_t envHeight() const noexcept { return m_envHeight; }
    [[nodiscard]] float envIntegral() const noexcept { return m_envIntegral; }

    [[nodiscard]] const PathStateLayout& layout() const noexcept { return m_layout; }

    /// Host-visible copy of one field's floats (exactly `layout().capacity()`
    /// elements). Caller must have waited for the GPU work that wrote it.
    [[nodiscard]] std::vector<float> readbackField(GpuAllocator& allocator, PathStateField field);

    /// Host-visible copy of one counter slot. Returns 0 for an out-of-range
    /// slot (there are kCounterSlotCount of them).
    [[nodiscard]] std::uint32_t readbackCounter(GpuAllocator& allocator, std::uint32_t slot);

private:
    PathStateLayout m_layout{0};
    GpuBuffer m_stateBuffer;
    GpuBuffer m_queueBuffer;
    GpuBuffer m_counterBuffer;
    GpuBuffer m_envMarginalBuffer;
    GpuBuffer m_envConditionalBuffer;
    std::uint32_t m_envWidth{0};
    std::uint32_t m_envHeight{0};
    float m_envIntegral{0.0f};
    GpuAllocator* m_allocator{nullptr};
};

}  // namespace ohao::diff
