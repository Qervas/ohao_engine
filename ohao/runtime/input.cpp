#include "runtime/input.hpp"

namespace ohao::runtime {

void InputMap::bindAction(std::string_view action, Binding b) {
    auto& v = m_actions[std::string(action)];
    for (const auto& e : v)
        if (e == b) return;
    v.push_back(b);
}

void InputMap::clearAction(std::string_view action) {
    if (auto it = m_actions.find(action); it != m_actions.end()) it->second.clear();
}

void InputMap::bindAxis(std::string_view axis, Binding positive, Binding negative) {
    m_axes[std::string(axis)] = AxisDef{positive, negative};
}

void InputMap::beginFrame(std::span<const InputEvent> events) {
    std::set<Binding> wentDown, wentUp;
    bool focusLost = false;
    m_mouseDelta = glm::vec2(0.0f);

    for (const auto& e : events) {
        switch (e.type) {
        case InputEvent::Type::Down: {
            const Binding b{e.device, e.code};
            wentDown.insert(b);
            m_down.insert(b);
            break;
        }
        case InputEvent::Type::Up: {
            const Binding b{e.device, e.code};
            wentUp.insert(b);
            m_down.erase(b);
            break;
        }
        case InputEvent::Type::MouseMove: {
            const glm::dvec2 p{e.x, e.y};
            if (m_haveMouse) m_mouseDelta += glm::vec2(p - m_lastMouse);
            m_lastMouse = p;
            m_haveMouse = true;
            break;
        }
        case InputEvent::Type::FocusLost:
            focusLost = true;
            break;
        }
    }
    if (focusLost) {
        m_down.clear();
        m_haveMouse = false;   // the cursor may be anywhere on return; no jump
    }

    for (const auto& [name, bindings] : m_actions) {
        ActionState& s = m_state[name];
        const bool wasHeld = s.held;
        bool nowHeld = false, anyDown = false, anyUp = false;
        for (const auto& b : bindings) {
            nowHeld |= isDown(b);
            anyDown |= wentDown.count(b) != 0;
            anyUp   |= wentUp.count(b) != 0;
        }
        s.held     = nowHeld;
        // Pressed: went down this frame and was not already held at frame start
        // (OS key-repeat Downs must not re-fire it).
        s.pressed  = anyDown && !wasHeld;
        // Released: stopped being held, or a tap went down and up inside this
        // frame without ever being observed as held.
        s.released = (wasHeld && !nowHeld) || (anyDown && anyUp && !nowHeld);
    }
}

bool InputMap::pressed(std::string_view a) const {
    auto it = m_state.find(a);
    return it != m_state.end() && it->second.pressed;
}

bool InputMap::held(std::string_view a) const {
    auto it = m_state.find(a);
    return it != m_state.end() && it->second.held;
}

bool InputMap::released(std::string_view a) const {
    auto it = m_state.find(a);
    return it != m_state.end() && it->second.released;
}

float InputMap::axis(std::string_view a) const {
    auto it = m_axes.find(a);
    if (it == m_axes.end()) return 0.0f;
    return (isDown(it->second.positive) ? 1.0f : 0.0f) - (isDown(it->second.negative) ? 1.0f : 0.0f);
}

} // namespace ohao::runtime
