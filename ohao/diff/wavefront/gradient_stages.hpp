// THE FIVE PIPELINES OF THE GRADIENT PATH, and the binding tables that say
// what each shader declares.
//
// Item 1: these were built inside `runWavefrontGradientProbe`, which is the
// test harness. The pipelines are not test-side knowledge -- they are what the
// shaders in shaders/diff/ require of anyone who dispatches them -- so an
// engine caller would otherwise have to restate the tables, and two
// statements of the same fact drift.
//
// ONE DEFINITION OF EACH TABLE. That is the point of the move, not tidiness:
// a descriptor set built from a table that disagrees with the shader is a
// validation error at best and a wrong-binding read at worst, and
// `WavefrontStage::record` refuses to dispatch until every declared binding
// has been written precisely because a missing one is otherwise silent.
//
// THE PUSH-CONSTANT SIZES ARE PASSED IN, for the same reason
// `ScatterSinks::Strides` is: the caller fills the push blocks, so the caller
// owns their layout. Three of the four are already library types
// (`WavefrontLoop::PrepareIndirectPush`, `IntersectPush`, `ScatterPush`); the
// generate block is the caller's, and taking its size as an argument is what
// stops this header guessing at it.
#pragma once

#include "diff/wavefront/wavefront_stage.hpp"

#include <cstdint>

namespace ohao::diff {

class GradientStages {
public:
    GradientStages() = default;
    ~GradientStages() = default;

    // Not copyable or movable, because WavefrontStage is not.
    GradientStages(const GradientStages&) = delete;
    GradientStages& operator=(const GradientStages&) = delete;
    GradientStages(GradientStages&&) = delete;
    GradientStages& operator=(GradientStages&&) = delete;

    /// Push-constant block sizes, in bytes. The caller fills these blocks, so
    /// it owns their layout; this class only needs to size the ranges.
    struct PushSizes {
        std::uint32_t generate{0};
        std::uint32_t prepareIndirect{0};
        std::uint32_t intersect{0};
        std::uint32_t scatter{0};
        [[nodiscard]] bool valid() const noexcept {
            return generate > 0 && prepareIndirect > 0 && intersect > 0 && scatter > 0;
        }
    };

    /// Build all five. IDEMPOTENT: a set already built is left alone, so a
    /// caller may pass the same object to every call without testing
    /// anything. Releases whatever it managed on failure.
    [[nodiscard]] bool build(VkDevice device, const PushSizes& sizes);
    void destroy(VkDevice device);

    [[nodiscard]] bool built() const noexcept { return m_built; }

    [[nodiscard]] WavefrontStage& generate() noexcept { return m_generate; }
    [[nodiscard]] WavefrontStage& prepareIndirect() noexcept { return m_prepareIndirect; }
    [[nodiscard]] WavefrontStage& intersect() noexcept { return m_intersect; }
    /// `variant` 0 is the FORWARD scatter, 1 the REPLAY. Two instantiations of
    /// the same traversal source (spec 6.2), which is why they are indexed
    /// rather than named at the call sites that treat them alike.
    [[nodiscard]] WavefrontStage& scatter(std::uint32_t variant) noexcept {
        return variant == 0u ? m_scatterForward : m_scatterReplay;
    }

private:
    WavefrontStage m_generate;
    WavefrontStage m_prepareIndirect;
    WavefrontStage m_intersect;
    WavefrontStage m_scatterForward;
    WavefrontStage m_scatterReplay;
    bool m_built{false};
};

}  // namespace ohao::diff
