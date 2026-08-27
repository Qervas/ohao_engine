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
