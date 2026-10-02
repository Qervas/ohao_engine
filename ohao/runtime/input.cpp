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
    m_mouseDelta = glm::vec2(0.0f);
    for (auto& [name, bindings] : m_actions) {
        ActionState& s = m_state[name];
        s.pressed = s.released = false;
    }

    // Re-evaluate every action after each event that changes what is down, in event
    // order. pressed / released latch on any held transition during the frame, so a
    // tap (down, up), a re-press (up, down) and a press after focus loss all count.
    // A repeated Down on a key already down changes nothing and fires nothing.
    auto settle = [&] {
        for (const auto& [name, bindings] : m_actions) {
            ActionState& s = m_state[name];
            bool nowHeld = false;
            for (const auto& b : bindings) nowHeld |= isDown(b);
            if (nowHeld && !s.held) s.pressed = true;
            if (!nowHeld && s.held) s.released = true;
            s.held = nowHeld;
        }
    };

    for (const auto& e : events) {
        switch (e.type) {
        case InputEvent::Type::Down:
            m_down.insert(Binding{e.device, e.code});
            settle();
            break;
        case InputEvent::Type::Up:
            m_down.erase(Binding{e.device, e.code});
            settle();
            break;
        case InputEvent::Type::MouseMove: {
            const glm::dvec2 p{e.x, e.y};
            if (m_haveMouse) m_mouseDelta += glm::vec2(p - m_lastMouse);
            m_lastMouse = p;
            m_haveMouse = true;
            break;
        }
        case InputEvent::Type::FocusLost:
            m_down.clear();
            m_haveMouse = false;   // the cursor may be anywhere on return; no jump
            settle();
            break;
        }
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
