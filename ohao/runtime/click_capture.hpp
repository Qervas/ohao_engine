#pragma once
// The click that takes the cursor (sub-project 7, ruling 1): in a play window the left mouse
// button both fires and captures the cursor. While the cursor is free, a mouse button press only
// asks for the cursor: the press and its release are taken out of the frame's input, so that
// click never fires a round. Once captured, buttons pass through.
#include "core/input.hpp"

#include <vector>

namespace ohao::runtime {

class ClickToCapture {
public:
    // Filters this frame's window events in place, before they reach a step. `captured`: the
    // cursor is captured now. True when a press asked for the cursor (capture it).
    bool filter(std::vector<InputEvent>& events, bool captured);
    [[nodiscard]] bool swallowing() const noexcept { return m_swallow >= 0; }

private:
    int m_swallow{-1};   // the button whose release is still to be taken out; -1: none
};

} // namespace ohao::runtime
