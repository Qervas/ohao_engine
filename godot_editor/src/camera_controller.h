#pragma once

#include <glm/glm.hpp>

// Forward declare OHAO types
namespace ohao {
    class Camera;
    class Scene;
}

namespace godot {

/**
 * CameraController - FPS and Orbit camera control
 *
 * Extracted from OhaoViewport to handle all camera-related state and logic.
 * Plain C++ class (not a Godot node), owned by OhaoViewport.
 */
class CameraController {
public:
    enum Mode {
        FPS = 0,
        ORBIT = 1,
    };

    CameraController();

    // Per-frame update (movement + rotation from held keys)
    void update(double delta, ohao::Camera& camera);

    // Input handlers - return true if the event was consumed
    // Mouse motion: right-drag rotates, middle-drag pans (orbit)
    bool handleMouseMotion(float rel_x, float rel_y, ohao::Camera& camera);
    // Mouse button: right-click capture, middle-click pan, scroll zoom
    // Does NOT handle left-click (that's SelectionController).
    // Returns true if consumed. Sets m_right_click_dragged for context menu detection.
    bool handleMouseButton(int button, bool pressed, float pos_x, float pos_y, ohao::Camera& camera);
    // Key press/release for WASD + arrows
    bool handleKey(int keycode, bool pressed);

    // Focus camera on scene bounding box
    void focusOnScene(ohao::Scene* scene, ohao::Camera& camera);

    // Mode
    void setMode(int mode, ohao::Camera& camera);
    int getMode() const { return static_cast<int>(m_mode); }

    // Properties
    void setMouseSensitivity(float sens) { m_mouse_sensitivity = sens; }
    float getMouseSensitivity() const { return m_mouse_sensitivity; }
    void setMoveSpeed(float speed) { m_move_speed = speed; }
    float getMoveSpeed() const { return m_move_speed; }
    void setAlwaysLook(bool v) { m_always_look = v; }
    bool getAlwaysLook() const { return m_always_look; }
    void clearMovementState();

    // State queries (used by OhaoViewport for signal emission)
    bool isMouseCaptured() const { return m_mouse_captured; }
    bool wasRightClickDrag() const { return m_right_click_dragged; }

private:
    void updateOrbitCamera(ohao::Camera& camera);

    // Camera mode
    Mode m_mode = FPS;

    // Mouse state
    bool m_always_look = false;  // FPS game mode: always rotate on mouse move
    bool m_mouse_captured = false;
    float m_mouse_sensitivity = 0.3f;
    float m_move_speed = 5.0f;
    float m_fast_move_multiplier = 2.5f;

    // Movement keys
    bool m_move_forward = false;
    bool m_move_backward = false;
    bool m_move_left = false;
    bool m_move_right = false;
    bool m_move_up = false;
    bool m_move_down = false;
    bool m_move_fast = false;

    // Rotation keys (arrow keys)
    bool m_rotate_up = false;
    bool m_rotate_down = false;
    bool m_rotate_left = false;
    bool m_rotate_right = false;
    float m_rotation_speed = 90.0f;

    // Orbit camera state
    float m_orbit_distance = 10.0f;
    float m_orbit_yaw = 0.0f;
    float m_orbit_pitch = 30.0f;
    float m_orbit_target_x = 0.0f;
    float m_orbit_target_y = 0.0f;
    float m_orbit_target_z = 0.0f;
    bool m_middle_mouse_captured = false;

    // Right-click context menu detection
    float m_right_click_start_x = 0.0f;
    float m_right_click_start_y = 0.0f;
    bool m_right_click_dragged = false;
};

} // namespace godot
