#pragma once
// Named actions and axes over raw input (sub-project 4 spec, "Input"). CPU-only and
// GLFW-free, so the ECS's systems read it and the API injects into it; the window host
// translates GLFW callbacks into InputEvents.
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <glm/vec2.hpp>

namespace ohao {

enum class InputDevice : uint8_t { Key, MouseButton };

// Codes are GLFW's numeric key / mouse-button values.
struct Binding {
    InputDevice device{InputDevice::Key};
    int code{0};
    auto operator<=>(const Binding&) const = default;
};

// The key names files, commands and recordings use. A Key's value is its GLFW key code, or
// kMouseKeyBase + the button for mouse buttons; None binds nothing.
enum class Key : int32_t {
    None = 0,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Digit0 = 48, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265, PageUp = 266, PageDown = 267, Home = 268, End = 269,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, RightShift = 344, RightControl = 345, RightAlt = 346,
    MouseLeft = 1000, MouseRight = 1001, MouseMiddle = 1002,
};
inline constexpr int32_t kMouseKeyBase = 1000;

struct KeyName {
    std::string_view name;
    Key key;
};
// Every Key with its name, in enum order (the one table: the catalogue registers it).
[[nodiscard]] std::span<const KeyName> keyNames();
[[nodiscard]] std::optional<Key> keyFromName(std::string_view name);
[[nodiscard]] std::string_view keyName(Key key);   // "" for a value not in the table
// The device binding a key stands for; nullopt for None.
[[nodiscard]] std::optional<Binding> keyBinding(Key key);

struct InputEvent {
    enum class Type : uint8_t {
        Down, Up,        // a device key or button (device, code)
        MouseMove,       // absolute cursor position (x, y)
        FocusLost,       // releases everything held, real and injected
        ActionDown,      // the virtual device: an action held / let go (name)
        ActionUp,
        AxisSet,         // an axis forced to `value` until changed or released (name)
        AxisRelease,     // the axis follows its keys again (name)
        MouseDelta,      // a relative mouse movement (x, y)
    };
    Type type{Type::Down};
    InputDevice device{InputDevice::Key};
    int code{0};
    double x{0.0}, y{0.0};
    std::string name;
    float value{0.0f};
};

// Named actions and axes. Call beginStep once per simulation step with that step's events;
// queries then describe this step.
//   held     : down at the end of the step
//   pressed  : went down this step and was not held at its start
//   released : stopped being held this step, or tapped down-and-up within it
// An action is held when any of its bindings is down or the virtual device holds it; an
// axis is its forced value when one is set, else its keys. Unknown names are inert
// (false / 0). FocusLost releases everything.
class InputMap {
public:
    void bindAction(std::string_view action, Binding b);
    void clearAction(std::string_view action);
    void bindAxis(std::string_view axis, Binding positive, Binding negative);
    // Drops every binding (the state of keys and the virtual device is kept).
    void clearBindings();

    void beginStep(std::span<const InputEvent> events);

    [[nodiscard]] bool  pressed(std::string_view action) const;
    [[nodiscard]] bool  held(std::string_view action) const;
    [[nodiscard]] bool  released(std::string_view action) const;
    [[nodiscard]] float axis(std::string_view axis) const;
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }
    // The actions and axes this map knows: bound, or set through the virtual device.
    [[nodiscard]] std::vector<std::string> actionNames() const;
    [[nodiscard]] std::vector<std::string> axisNames() const;

private:
    struct ActionState { bool pressed{false}, held{false}, released{false}; };
    struct AxisDef { Binding positive, negative; };

    [[nodiscard]] bool isDown(const Binding& b) const { return m_down.count(b) != 0; }
    void settle();

    std::map<std::string, std::vector<Binding>, std::less<>> m_actions;
    std::map<std::string, AxisDef, std::less<>> m_axes;
    std::map<std::string, ActionState, std::less<>> m_state;
    std::set<Binding> m_down;
    std::set<std::string, std::less<>> m_virtualHeld;              // injected actions held
    std::map<std::string, float, std::less<>> m_axisForced;         // injected axis values
    glm::vec2 m_mouseDelta{0.0f};
    glm::dvec2 m_lastMouse{0.0};
    bool m_haveMouse{false};
};

} // namespace ohao
