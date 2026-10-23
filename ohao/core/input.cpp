#include "core/input.hpp"

#include <array>

namespace ohao {

namespace {

constexpr KeyName kKeys[] = {
    {"None", Key::None},
    {"Space", Key::Space}, {"Apostrophe", Key::Apostrophe}, {"Comma", Key::Comma}, {"Minus", Key::Minus},
    {"Period", Key::Period}, {"Slash", Key::Slash},
    {"Digit0", Key::Digit0}, {"Digit1", Key::Digit1}, {"Digit2", Key::Digit2}, {"Digit3", Key::Digit3},
    {"Digit4", Key::Digit4}, {"Digit5", Key::Digit5}, {"Digit6", Key::Digit6}, {"Digit7", Key::Digit7},
    {"Digit8", Key::Digit8}, {"Digit9", Key::Digit9},
    {"Semicolon", Key::Semicolon}, {"Equal", Key::Equal},
    {"A", Key::A}, {"B", Key::B}, {"C", Key::C}, {"D", Key::D}, {"E", Key::E}, {"F", Key::F}, {"G", Key::G},
    {"H", Key::H}, {"I", Key::I}, {"J", Key::J}, {"K", Key::K}, {"L", Key::L}, {"M", Key::M}, {"N", Key::N},
    {"O", Key::O}, {"P", Key::P}, {"Q", Key::Q}, {"R", Key::R}, {"S", Key::S}, {"T", Key::T}, {"U", Key::U},
    {"V", Key::V}, {"W", Key::W}, {"X", Key::X}, {"Y", Key::Y}, {"Z", Key::Z},
    {"LeftBracket", Key::LeftBracket}, {"Backslash", Key::Backslash}, {"RightBracket", Key::RightBracket},
    {"GraveAccent", Key::GraveAccent},
    {"Escape", Key::Escape}, {"Enter", Key::Enter}, {"Tab", Key::Tab}, {"Backspace", Key::Backspace},
    {"Insert", Key::Insert}, {"Delete", Key::Delete}, {"Right", Key::Right}, {"Left", Key::Left},
    {"Down", Key::Down}, {"Up", Key::Up}, {"PageUp", Key::PageUp}, {"PageDown", Key::PageDown},
    {"Home", Key::Home}, {"End", Key::End},
    {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4}, {"F5", Key::F5}, {"F6", Key::F6},
    {"F7", Key::F7}, {"F8", Key::F8}, {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12},
    {"LeftShift", Key::LeftShift}, {"LeftControl", Key::LeftControl}, {"LeftAlt", Key::LeftAlt},
    {"RightShift", Key::RightShift}, {"RightControl", Key::RightControl}, {"RightAlt", Key::RightAlt},
    {"MouseLeft", Key::MouseLeft}, {"MouseRight", Key::MouseRight}, {"MouseMiddle", Key::MouseMiddle},
};

} // namespace

std::span<const KeyName> keyNames() { return kKeys; }

std::optional<Key> keyFromName(std::string_view name) {
    for (const KeyName& k : kKeys)
        if (k.name == name) return k.key;
    return std::nullopt;
}

std::string_view keyName(Key key) {
    for (const KeyName& k : kKeys)
        if (k.key == key) return k.name;
    return {};
}

std::optional<Binding> keyBinding(Key key) {
    const auto v = static_cast<int32_t>(key);
    if (key == Key::None) return std::nullopt;
    if (v >= kMouseKeyBase) return Binding{InputDevice::MouseButton, v - kMouseKeyBase};
    return Binding{InputDevice::Key, v};
}

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

void InputMap::clearBindings() {
    m_actions.clear();
    m_axes.clear();
}

// Re-evaluates every action after each event that changes what is held, in event order.
// pressed / released latch on any held transition during the step, so a tap (down, up), a
// re-press (up, down) and a press after focus loss all count. A repeated Down on a key
// already down changes nothing and fires nothing.
void InputMap::settle() {
    const auto update = [&](const std::string& name, bool nowHeld) {
        ActionState& s = m_state[name];
        if (nowHeld && !s.held) s.pressed = true;
        if (!nowHeld && s.held) s.released = true;
        s.held = nowHeld;
    };
    for (const auto& [name, bindings] : m_actions) {
        bool nowHeld = m_virtualHeld.count(name) != 0;
        for (const auto& b : bindings) nowHeld |= isDown(b);
        update(name, nowHeld);
    }
    for (const std::string& name : m_virtualHeld)
        if (!m_actions.count(name)) update(name, true);
    for (auto& [name, s] : m_state)   // a virtual-only action let go
        if (!m_actions.count(name) && !m_virtualHeld.count(name) && s.held) {
            s.held = false;
            s.released = true;
        }
}

void InputMap::beginStep(std::span<const InputEvent> events) {
    m_mouseDelta = glm::vec2(0.0f);
    for (auto& [name, s] : m_state) s.pressed = s.released = false;
    settle();   // bindings may have changed since the last step: what they hold now counts

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
        case InputEvent::Type::MouseDelta:
            m_mouseDelta += glm::vec2(static_cast<float>(e.x), static_cast<float>(e.y));
            break;
        case InputEvent::Type::FocusLost:
            m_down.clear();
            m_virtualHeld.clear();
            m_axisForced.clear();
            m_haveMouse = false;   // the cursor may be anywhere on return; no jump
            settle();
            break;
        case InputEvent::Type::ActionDown:
            m_virtualHeld.insert(e.name);
            settle();
            break;
        case InputEvent::Type::ActionUp:
            m_virtualHeld.erase(e.name);
            settle();
            break;
        case InputEvent::Type::AxisSet:
            m_axisForced[e.name] = e.value;
            break;
        case InputEvent::Type::AxisRelease:
            m_axisForced.erase(e.name);
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
    if (auto f = m_axisForced.find(a); f != m_axisForced.end()) return f->second;
    auto it = m_axes.find(a);
    if (it == m_axes.end()) return 0.0f;
    return (isDown(it->second.positive) ? 1.0f : 0.0f) - (isDown(it->second.negative) ? 1.0f : 0.0f);
}

std::vector<std::string> InputMap::actionNames() const {
    std::set<std::string, std::less<>> names;
    for (const auto& [name, b] : m_actions) names.insert(name);
    for (const auto& [name, s] : m_state) names.insert(name);
    return {names.begin(), names.end()};
}

std::vector<std::string> InputMap::axisNames() const {
    std::set<std::string, std::less<>> names;
    for (const auto& [name, d] : m_axes) names.insert(name);
    for (const auto& [name, v] : m_axisForced) names.insert(name);
    return {names.begin(), names.end()};
}

} // namespace ohao
