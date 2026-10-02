#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <glm/vec2.hpp>

namespace ohao::runtime {

enum class InputDevice : uint8_t { Key, MouseButton };

// Codes are GLFW's numeric key / mouse-button values. The map itself never
// includes GLFW; Window translates GLFW callbacks into InputEvents.
struct Binding {
    InputDevice device{InputDevice::Key};
    int code{0};
    auto operator<=>(const Binding&) const = default;
};

struct InputEvent {
    enum class Type : uint8_t { Down, Up, MouseMove, FocusLost };
    Type type{Type::Down};
    InputDevice device{InputDevice::Key};
    int code{0};
    double x{0.0}, y{0.0};   // MouseMove: absolute cursor position
};

// Named actions and axes over raw input. Call beginFrame once per frame with
// that frame's events; queries then describe this frame.
//   held     : down at the end of the frame
//   pressed  : went down this frame and was not held at frame start
//   released : stopped being held this frame, or tapped down-and-up within it
// Unknown names are inert (false / 0). FocusLost releases everything.
class InputMap {
public:
    void bindAction(std::string_view action, Binding b);
    void clearAction(std::string_view action);
    void bindAxis(std::string_view axis, Binding positive, Binding negative);

    void beginFrame(std::span<const InputEvent> events);

    [[nodiscard]] bool  pressed(std::string_view action) const;
    [[nodiscard]] bool  held(std::string_view action) const;
    [[nodiscard]] bool  released(std::string_view action) const;
    [[nodiscard]] float axis(std::string_view axis) const;
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }

private:
    struct ActionState { bool pressed{false}, held{false}, released{false}; };
    struct AxisDef { Binding positive, negative; };

    [[nodiscard]] bool isDown(const Binding& b) const { return m_down.count(b) != 0; }

    std::map<std::string, std::vector<Binding>, std::less<>> m_actions;
    std::map<std::string, AxisDef, std::less<>> m_axes;
    std::map<std::string, ActionState, std::less<>> m_state;
    std::set<Binding> m_down;
    glm::vec2 m_mouseDelta{0.0f};
    glm::dvec2 m_lastMouse{0.0};
    bool m_haveMouse{false};
};

} // namespace ohao::runtime
