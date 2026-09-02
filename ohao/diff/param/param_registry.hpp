#pragma once

#include "diff/grad/arena_layout.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::diff {

enum class ParamKind : std::uint8_t {
    Texture,         // shaped, bilinear scatter
    ScalarBlock,     // flat floats: BSDF scalars, SSAO knobs, MLP weights
    VertexPositions, // stage 3
};

struct ParamShape {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channels{0};

    /// Returns 0 if the product overflows uint32_t rather than wrapping --
    /// an undersized block from a wrapped multiply is silent wrongness, the
    /// exact failure class this subsystem exists to make impossible. A zero
    /// floatCount is already rejected by addParam, so this turns an
    /// overflowing shape into a clean rejection.
    [[nodiscard]] std::uint32_t floatCount() const noexcept {
        const std::uint64_t product = static_cast<std::uint64_t>(width) *
                                      static_cast<std::uint64_t>(height) *
                                      static_cast<std::uint64_t>(channels);
        if (product > static_cast<std::uint64_t>(UINT32_MAX)) return 0;
        return static_cast<std::uint32_t>(product);
    }

    /// THE ELEMENT ORDERING. Which float of this parameter's gradient block --
    /// and of the matching primal array the shader reads -- holds channel `c`
    /// of texel `(x, y)`.
    ///
    /// Row-major over texels, channels INTERLEAVED within a texel:
    ///
    ///     k = (y * width + x) * channels + c
    ///
    /// ESTABLISHED BY STAGE 1 TASK 5, AND IT IS NOT IMPLIED BY floatCount().
    /// floatCount() is w*h*c -- a COUNT, and a count implies no ordering
    /// whatsoever: row-major, column-major and channel-planar all have the
    /// same float count. An earlier version of the shader's arena comment
    /// claimed the ordering "ParamShape::floatCount() already implies", which
    /// was the whole of the tie and was not one. Until this function existed
    /// the mapping from (x, y, c) to an arena offset was asserted nowhere and
    /// tested nowhere, and a GLSL/C++ disagreement about it is a silent
    /// wrong-slot scatter: gradients that look plausible and are attributed to
    /// the wrong texel.
    ///
    /// TIED, NOT MERELY DOCUMENTED. `shaders/includes/diff/bsdf_adjoint.glsl`
    /// spells the same formula once, in `diffTexelElementIndex`, and
    /// `tests/diff/diff_gpu_probe.cpp`'s `checkTexelOrderingTie()` refuses to
    /// run the probe unless (a) that GLSL return statement is, modulo
    /// whitespace, `(y * width + x) * channels + c` and (b) THIS function
    /// agrees with the formula at every (x, y, c) of a non-degenerate,
    /// non-square, multi-channel shape -- so neither side can drift alone.
    /// Probe check 44 then MEASURES the same agreement on the GPU, by
    /// predicting from this function alone which arena floats a known bilinear
    /// footprint may touch and requiring every other float to be exactly 0.
    ///
    /// Out-of-range (x, y, c) are the caller's problem: this is arithmetic,
    /// not a bounds check. The SHADER's per-element guard
    /// (`k < gradArenaFloats - gradOffset`) is what stops an out-of-range k
    /// reaching memory.
    [[nodiscard]] std::uint32_t elementIndex(std::uint32_t x, std::uint32_t y,
                                             std::uint32_t c) const noexcept {
        return (y * width + x) * channels + c;
    }
};

struct ParamId {
    std::uint32_t value{0xFFFFFFFFu};
    [[nodiscard]] bool valid() const noexcept { return value != 0xFFFFFFFFu; }
};

struct DiffParam {
    std::string name;
    ParamKind kind{ParamKind::ScalarBlock};
    ParamShape shape{};
    std::uint32_t floatCount{0};
    std::size_t gradBlock{ArenaLayout::kInvalidBlock};
    std::size_t stateBlock{ArenaLayout::kInvalidBlock};  // Adam m and v, 2x floatCount
};

struct RegisterResult {
    bool ok{false};
    ParamId id{};
    std::string error;
};

/// Registry of everything being differentiated.
///
/// The primitive is a float buffer with a matching gradient block. Textures are
/// not a special case -- they carry shape metadata for the bilinear scatter.
class ParamRegistry {
public:
    /// Primal must be a float format. 8-bit storage is rejected: a gradient step
    /// below 1/255 rounds to nothing, so the optimizer stalls in a way that is
    /// indistinguishable from convergence.
    RegisterResult registerTexture(std::string name, ParamShape shape, VkFormat primalFormat);

    RegisterResult registerScalarBlock(std::string name, std::uint32_t floatCount);

    /// STAGE 3: a block of vertex positions, `componentsPerVertex` floats each
    /// (2 for the orthographic screen-space form, 3 in world).
    ///
    /// A DISTINCT KIND rather than a ScalarBlock of the same length, and the
    /// distinction is not cosmetic: spec 4.1 splits the derivative into an
    /// interior integral and a boundary one, and this is the only kind for
    /// which the SECOND is nonzero. Appearance parameters have no boundary
    /// term at all -- mathematically absent, not merely small -- so which
    /// kernels may write a block is a property of its kind. Registering
    /// geometry as a ScalarBlock would erase exactly the fact the split rests
    /// on.
    RegisterResult registerVertexPositions(std::string name, std::uint32_t vertexCount,
                                           std::uint32_t componentsPerVertex);

    [[nodiscard]] std::size_t count() const noexcept { return m_params.size(); }
    [[nodiscard]] const DiffParam* find(std::string_view name) const;
    [[nodiscard]] const DiffParam* get(ParamId id) const;
    [[nodiscard]] const ArenaLayout& layout() const noexcept { return m_layout; }

    /// True when the format stores enough precision to be optimized.
    [[nodiscard]] static bool isDifferentiableFormat(VkFormat format) noexcept;

private:
    RegisterResult addParam(std::string name, ParamKind kind, ParamShape shape,
                            std::uint32_t floatCount);

    std::vector<DiffParam> m_params;
    ArenaLayout m_layout;
};

}  // namespace ohao::diff
