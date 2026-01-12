#pragma once
#include <glm/glm.hpp>
#include <array>
#include <bitset>
#include "input_types.hpp"

namespace ohao {

// Snapshot of input state at a single frame
struct InputState {
    // Mouse state
    glm::vec2 mousePosition{0.0f};
    glm::vec2 mouseDelta{0.0f};
    float scrollDelta{0.0f};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> mouseButtons{};

    // Keyboard state
    std::bitset<static_cast<size_t>(KeyCode::MaxKeyCode)> keys{};

    // Modifier keys
    ModifierFlags modifiers{ModifierFlags::None};

    // Timestamp
    double timestamp{0.0};

    // Helper methods
    bool isMouseButtonDown(MouseButton button) const {
        return mouseButtons[static_cast<size_t>(button)];
    }

    bool isKeyDown(KeyCode key) const {
        int keyIndex = static_cast<int>(key);
        if (keyIndex < 0 || keyIndex >= static_cast<int>(KeyCode::MaxKeyCode)) {
            return false;
        }
        return keys[keyIndex];
    }

    void setMouseButton(MouseButton button, bool pressed) {
        mouseButtons[static_cast<size_t>(button)] = pressed;
    }

    void setKey(KeyCode key, bool pressed) {
        int keyIndex = static_cast<int>(key);
        if (keyIndex >= 0 && keyIndex < static_cast<int>(KeyCode::MaxKeyCode)) {
            keys[keyIndex] = pressed;
        }
    }

    void reset() {
        mousePosition = glm::vec2(0.0f);
        mouseDelta = glm::vec2(0.0f);
        scrollDelta = 0.0f;
        mouseButtons.fill(false);
        keys.reset();
        modifiers = ModifierFlags::None;
        timestamp = 0.0;
    }
};

} // namespace ohao
