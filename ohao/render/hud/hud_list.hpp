#pragma once
// The HUD (sub-project 7 spec, "Presentation"): what the `hud` render pass draws over the final
// LDR image, before present and readback. A list of items in pixels (origin top left, y down),
// drawn in order with alpha blending: solid rectangles, glyphs of a built-in 5x7 bitmap font, and
// a vignette (a rectangle whose alpha grows from 0 at its centre to `color.a` at its edges). The
// host builds the list each frame (ecs/combat_hud.hpp) and hands it to the renderer; an empty
// list draws nothing and records no pass.
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ohao::render {

enum class HudKind : uint32_t { Rect = 0, Glyph = 1, Vignette = 2 };

// One item, as the pass reads it (std430, 48 bytes).
struct HudItem {
    glm::vec4 rect{0.0f};    // x0, y0, x1, y1 in pixels
    glm::vec4 color{1.0f};   // rgba, blended over the image by alpha
    uint32_t kind{0};        // HudKind
    uint32_t glyph{0};       // Glyph: the font's glyph index
    uint32_t pad0{0}, pad1{0};
};
static_assert(sizeof(HudItem) == 48);

struct HudList {
    std::vector<HudItem> items;
    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
};

// The most items a frame draws; the rest are dropped.
constexpr std::size_t kHudMaxItems = 2048;

// The font: digits, A-Z, '/' and '.', 5 columns by 7 rows. Glyph g's row r is hudFontRows()[g * 7 + r],
// bit 4 its leftmost column.
constexpr int kHudGlyphWidth = 5;
constexpr int kHudGlyphHeight = 7;
[[nodiscard]] int hudGlyph(char c) noexcept;   // lower case maps to upper; -1: not in the font
[[nodiscard]] std::span<const uint32_t> hudFontRows() noexcept;
[[nodiscard]] uint32_t hudGlyphRow(int glyph, int row) noexcept;

void hudRect(HudList& list, glm::vec4 rect, glm::vec4 color);
void hudVignette(HudList& list, glm::vec4 rect, glm::vec4 color);
// `text` at `topLeft`, each font pixel `px` pixels square, one font pixel between glyphs; a
// character outside the font is a space. Returns the width drawn.
float hudText(HudList& list, std::string_view text, glm::vec2 topLeft, float px, glm::vec4 color);
[[nodiscard]] float hudTextWidth(std::string_view text, float px) noexcept;

} // namespace ohao::render
