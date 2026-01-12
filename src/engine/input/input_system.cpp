#include "input_system.hpp"
#include <GLFW/glfw3.h>
#include <iostream>

namespace ohao {

// Static callback wrappers for GLFW
static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    InputSystem::get().onKeyCallback(key, scancode, action, mods);
}

static void glfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    InputSystem::get().onMouseButtonCallback(button, action, mods);
}

static void glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    InputSystem::get().onCursorPosCallback(xpos, ypos);
}

static void glfwScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    InputSystem::get().onScrollCallback(xoffset, yoffset);
}

InputSystem& InputSystem::get() {
    static InputSystem instance;
    return instance;
}

void InputSystem::initialize(GLFWwindow* win) {
    if (initialized) {
        std::cerr << "[InputSystem] Already initialized" << std::endl;
        return;
    }

    window = win;
    if (!window) {
        std::cerr << "[InputSystem] Invalid window pointer" << std::endl;
        return;
    }

    // Set GLFW callbacks
    glfwSetKeyCallback(window, glfwKeyCallback);
    glfwSetMouseButtonCallback(window, glfwMouseButtonCallback);
    glfwSetCursorPosCallback(window, glfwCursorPosCallback);
    glfwSetScrollCallback(window, glfwScrollCallback);

    // Initialize mouse position
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    currentState.mousePosition = glm::vec2(static_cast<float>(xpos), static_cast<float>(ypos));
    previousState.mousePosition = currentState.mousePosition;

    initialized = true;
    std::cout << "[InputSystem] Initialized" << std::endl;
}

void InputSystem::shutdown() {
    if (!initialized) return;

    // Clear callbacks (set to nullptr)
    if (window) {
        glfwSetKeyCallback(window, nullptr);
        glfwSetMouseButtonCallback(window, nullptr);
        glfwSetCursorPosCallback(window, nullptr);
        glfwSetScrollCallback(window, nullptr);
    }

    window = nullptr;
    initialized = false;
    currentState.reset();
    previousState.reset();

    std::cout << "[InputSystem] Shutdown" << std::endl;
}

void InputSystem::update() {
    if (!initialized) return;

    // Save current state as previous
    previousState = currentState;

    // Update timestamp
    currentState.timestamp = glfwGetTime();

    // Calculate mouse delta
    currentState.mouseDelta = currentState.mousePosition - previousState.mousePosition;

    // Apply accumulated scroll and reset
    currentState.scrollDelta = accumulatedScroll;
    accumulatedScroll = 0.0f;

    // Reset first mouse flag after first frame
    if (firstMouseMove && glm::length(currentState.mouseDelta) > 0.0f) {
        currentState.mouseDelta = glm::vec2(0.0f);  // Ignore first delta
        firstMouseMove = false;
    }
}

// Mouse queries
glm::vec2 InputSystem::getMousePosition() const {
    return currentState.mousePosition;
}

glm::vec2 InputSystem::getMouseDelta() const {
    return currentState.mouseDelta;
}

float InputSystem::getScrollDelta() const {
    return currentState.scrollDelta;
}

bool InputSystem::isMouseButtonDown(MouseButton button) const {
    return currentState.isMouseButtonDown(button);
}

bool InputSystem::isMouseButtonPressed(MouseButton button) const {
    return currentState.isMouseButtonDown(button) && !previousState.isMouseButtonDown(button);
}

bool InputSystem::isMouseButtonReleased(MouseButton button) const {
    return !currentState.isMouseButtonDown(button) && previousState.isMouseButtonDown(button);
}

// Keyboard queries
bool InputSystem::isKeyDown(KeyCode key) const {
    return currentState.isKeyDown(key);
}

bool InputSystem::isKeyPressed(KeyCode key) const {
    return currentState.isKeyDown(key) && !previousState.isKeyDown(key);
}

bool InputSystem::isKeyReleased(KeyCode key) const {
    return !currentState.isKeyDown(key) && previousState.isKeyDown(key);
}

// Modifiers
ModifierFlags InputSystem::getModifiers() const {
    return currentState.modifiers;
}

bool InputSystem::isShiftDown() const {
    return hasModifier(currentState.modifiers, ModifierFlags::Shift);
}

bool InputSystem::isControlDown() const {
    return hasModifier(currentState.modifiers, ModifierFlags::Control);
}

bool InputSystem::isAltDown() const {
    return hasModifier(currentState.modifiers, ModifierFlags::Alt);
}

// Cursor control
void InputSystem::setCursorEnabled(bool enabled) {
    if (!window) return;

    cursorEnabled = enabled;
    glfwSetInputMode(window, GLFW_CURSOR, enabled ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
}

// GLFW callback handlers
void InputSystem::onKeyCallback(int key, int scancode, int action, int mods) {
    if (key < 0 || key >= static_cast<int>(KeyCode::MaxKeyCode)) return;

    if (action == GLFW_PRESS) {
        currentState.setKey(static_cast<KeyCode>(key), true);
    } else if (action == GLFW_RELEASE) {
        currentState.setKey(static_cast<KeyCode>(key), false);
    }
    // GLFW_REPEAT is ignored - we handle repeat via isKeyDown()

    updateModifiers(mods);
}

void InputSystem::onMouseButtonCallback(int button, int action, int mods) {
    if (button < 0 || button >= static_cast<int>(MouseButton::Count)) return;

    if (action == GLFW_PRESS) {
        currentState.setMouseButton(static_cast<MouseButton>(button), true);
    } else if (action == GLFW_RELEASE) {
        currentState.setMouseButton(static_cast<MouseButton>(button), false);
    }

    updateModifiers(mods);
}

void InputSystem::onCursorPosCallback(double xpos, double ypos) {
    currentState.mousePosition = glm::vec2(static_cast<float>(xpos), static_cast<float>(ypos));
}

void InputSystem::onScrollCallback(double xoffset, double yoffset) {
    accumulatedScroll += static_cast<float>(yoffset);
}

void InputSystem::updateModifiers(int mods) {
    currentState.modifiers = ModifierFlags::None;

    if (mods & GLFW_MOD_SHIFT) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::Shift;
    }
    if (mods & GLFW_MOD_CONTROL) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::Control;
    }
    if (mods & GLFW_MOD_ALT) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::Alt;
    }
    if (mods & GLFW_MOD_SUPER) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::Super;
    }
    if (mods & GLFW_MOD_CAPS_LOCK) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::CapsLock;
    }
    if (mods & GLFW_MOD_NUM_LOCK) {
        currentState.modifiers = currentState.modifiers | ModifierFlags::NumLock;
    }
}

} // namespace ohao
