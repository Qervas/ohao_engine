#pragma once
#include <glm/glm.hpp>
#include "input_types.hpp"
#include "input_state.hpp"

struct GLFWwindow;

namespace ohao {

// Input System - Singleton that manages all input processing
// Provides abstraction over GLFW/ImGui input for keyboard, mouse, and future controller support
class InputSystem {
public:
    // Singleton access
    static InputSystem& get();

    // Lifecycle
    void initialize(GLFWwindow* window);
    void shutdown();

    // Must be called once per frame, before processing input
    void update();

    // Mouse queries
    glm::vec2 getMousePosition() const;
    glm::vec2 getMouseDelta() const;
    float getScrollDelta() const;

    // Mouse button state
    bool isMouseButtonDown(MouseButton button) const;
    bool isMouseButtonPressed(MouseButton button) const;   // Just pressed this frame
    bool isMouseButtonReleased(MouseButton button) const;  // Just released this frame

    // Keyboard state
    bool isKeyDown(KeyCode key) const;
    bool isKeyPressed(KeyCode key) const;   // Just pressed this frame
    bool isKeyReleased(KeyCode key) const;  // Just released this frame

    // Modifier keys
    ModifierFlags getModifiers() const;
    bool isShiftDown() const;
    bool isControlDown() const;
    bool isAltDown() const;

    // State snapshots
    const InputState& getCurrentState() const { return currentState; }
    const InputState& getPreviousState() const { return previousState; }

    // Cursor control
    void setCursorEnabled(bool enabled);
    bool isCursorEnabled() const { return cursorEnabled; }

    // GLFW callbacks (internal use)
    void onKeyCallback(int key, int scancode, int action, int mods);
    void onMouseButtonCallback(int button, int action, int mods);
    void onCursorPosCallback(double xpos, double ypos);
    void onScrollCallback(double xoffset, double yoffset);

private:
    InputSystem() = default;
    ~InputSystem() = default;
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    void updateModifiers(int mods);

    GLFWwindow* window{nullptr};
    bool initialized{false};
    bool cursorEnabled{true};

    InputState currentState;
    InputState previousState;

    // Accumulated scroll for this frame (reset each update)
    float accumulatedScroll{0.0f};

    // First mouse movement flag to prevent large delta on first frame
    bool firstMouseMove{true};
};

} // namespace ohao
