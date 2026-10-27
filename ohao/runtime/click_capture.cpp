#include "runtime/click_capture.hpp"

#include <algorithm>

namespace ohao::runtime {

bool ClickToCapture::filter(std::vector<InputEvent>& events, bool captured) {
    bool capture = false;
    std::vector<InputEvent> kept;
    kept.reserve(events.size());
    for (InputEvent& e : events) {
        const bool button = e.device == InputDevice::MouseButton &&
                            (e.type == InputEvent::Type::Down || e.type == InputEvent::Type::Up);
        if (button && e.type == InputEvent::Type::Up && m_swallow == e.code) {
            m_swallow = -1;   // the capturing click's release: out
            continue;
        }
        if (button && e.type == InputEvent::Type::Down && !captured && !capture && m_swallow < 0) {
            m_swallow = e.code;   // a press on a free cursor only asks for it
            capture = true;
            continue;
        }
        kept.push_back(std::move(e));
    }
    events = std::move(kept);
    return capture;
}

} // namespace ohao::runtime
