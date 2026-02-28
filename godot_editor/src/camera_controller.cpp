#include "camera_controller.h"

#include "renderer/camera/camera.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cfloat>
#include <algorithm>
#include <cmath>

// Godot key/mouse constants (match godot_cpp enums)
// We use raw int values to avoid pulling in heavy Godot headers.
static constexpr int MOUSE_BUTTON_RIGHT_VAL = 2;
static constexpr int MOUSE_BUTTON_MIDDLE_VAL = 3;
static constexpr int MOUSE_BUTTON_WHEEL_UP_VAL = 4;
static constexpr int MOUSE_BUTTON_WHEEL_DOWN_VAL = 5;

static constexpr int KEY_W_VAL = 87;
static constexpr int KEY_S_VAL = 83;
static constexpr int KEY_A_VAL = 65;
static constexpr int KEY_D_VAL = 68;
static constexpr int KEY_E_VAL = 69;
static constexpr int KEY_Q_VAL = 81;
static constexpr int KEY_SPACE_VAL = 32;
static constexpr int KEY_CTRL_VAL = 4194306;
static constexpr int KEY_SHIFT_VAL = 4194304;
static constexpr int KEY_UP_VAL = 4194320;
static constexpr int KEY_DOWN_VAL = 4194322;
static constexpr int KEY_LEFT_VAL = 4194319;
static constexpr int KEY_RIGHT_VAL = 4194321;

namespace godot {

CameraController::CameraController() {}

void CameraController::update(double delta, ohao::Camera& camera) {
    float dt = static_cast<float>(delta);
    updateArrowKeyRotation(dt, camera);
    updateFPSMovement(dt, camera);
}

void CameraController::updateArrowKeyRotation(float delta, ohao::Camera& camera) {
    if (!m_rotate_up && !m_rotate_down && !m_rotate_left && !m_rotate_right) return;

    float rotSpeed = m_rotation_speed * delta;
    if (m_move_fast) rotSpeed *= m_fast_move_multiplier;

    float deltaYaw = 0.0f;
    float deltaPitch = 0.0f;

    if (m_rotate_left)  deltaYaw -= rotSpeed;
    if (m_rotate_right) deltaYaw += rotSpeed;
    if (m_rotate_up)    deltaPitch += rotSpeed;
    if (m_rotate_down)  deltaPitch -= rotSpeed;

    camera.rotate(deltaPitch, deltaYaw);
}

void CameraController::updateFPSMovement(float delta, ohao::Camera& camera) {
    if (!m_move_forward && !m_move_backward && !m_move_left && !m_move_right &&
        !m_move_up && !m_move_down) return;

    float speed = m_move_speed * delta;
    if (m_move_fast) speed *= m_fast_move_multiplier;

    glm::vec3 front = camera.getFront();
    glm::vec3 right = camera.getRight();
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

    glm::vec3 movement(0.0f);
    if (m_move_forward)  movement += front * speed;
    if (m_move_backward) movement -= front * speed;
    if (m_move_right)    movement += right * speed;
    if (m_move_left)     movement -= right * speed;
    if (m_move_up)       movement += up * speed;
    if (m_move_down)     movement -= up * speed;

    camera.move(movement);
}

bool CameraController::handleMouseMotion(float rel_x, float rel_y, ohao::Camera& camera) {
    // Middle mouse: pan in orbit mode
    if (m_middle_mouse_captured && m_mode == ORBIT) {
        float pan_speed = m_orbit_distance * 0.002f;
        glm::vec3 right = camera.getRight();
        glm::vec3 up = camera.getUp();
        m_orbit_target_x -= right.x * rel_x * pan_speed + up.x * -rel_y * pan_speed;
        m_orbit_target_y -= right.y * rel_x * pan_speed + up.y * -rel_y * pan_speed;
        m_orbit_target_z -= right.z * rel_x * pan_speed + up.z * -rel_y * pan_speed;
        updateOrbitCamera(camera);
        return true;
    }

    if (!m_mouse_captured && !m_always_look) {
        return false;
    }

    // Mark as dragged if moved more than a few pixels
    float len = std::sqrt(rel_x * rel_x + rel_y * rel_y);
    if (len > 2.0f && m_mouse_captured) {
        m_right_click_dragged = true;
    }

    float delta_yaw = -rel_x * m_mouse_sensitivity;
    float delta_pitch = -rel_y * m_mouse_sensitivity;

    if (m_mode == ORBIT) {
        m_orbit_yaw += delta_yaw;
        m_orbit_pitch = std::clamp(m_orbit_pitch + delta_pitch, -89.0f, 89.0f);
        updateOrbitCamera(camera);
    } else {
        camera.rotate(delta_pitch, delta_yaw);
    }

    return true;
}

bool CameraController::handleMouseButton(int button, bool pressed, float pos_x, float pos_y, ohao::Camera& camera) {
    if (button == MOUSE_BUTTON_RIGHT_VAL) {
        if (pressed) {
            m_mouse_captured = true;
            m_right_click_start_x = pos_x;
            m_right_click_start_y = pos_y;
            m_right_click_dragged = false;
        } else {
            m_mouse_captured = false;
            // Caller checks wasRightClickDrag() for context menu
        }
        return true;
    }

    if (button == MOUSE_BUTTON_MIDDLE_VAL) {
        m_middle_mouse_captured = pressed;
        return true;
    }

    if (button == MOUSE_BUTTON_WHEEL_UP_VAL && pressed) {
        if (m_mode == ORBIT) {
            m_orbit_distance = std::max(0.5f, m_orbit_distance - m_orbit_distance * 0.1f);
            updateOrbitCamera(camera);
        } else {
            glm::vec3 forward = camera.getFront();
            camera.move(forward * 0.5f);
        }
        return true;
    }

    if (button == MOUSE_BUTTON_WHEEL_DOWN_VAL && pressed) {
        if (m_mode == ORBIT) {
            m_orbit_distance = std::min(500.0f, m_orbit_distance + m_orbit_distance * 0.1f);
            updateOrbitCamera(camera);
        } else {
            glm::vec3 forward = camera.getFront();
            camera.move(-forward * 0.5f);
        }
        return true;
    }

    return false;
}

bool CameraController::handleKey(int keycode, bool pressed) {
    bool consumed = true;

    switch (keycode) {
        case KEY_W_VAL:     m_move_forward = pressed; break;
        case KEY_S_VAL:     m_move_backward = pressed; break;
        case KEY_A_VAL:     m_move_left = pressed; break;
        case KEY_D_VAL:     m_move_right = pressed; break;
        case KEY_E_VAL:
        case KEY_SPACE_VAL: m_move_up = pressed; break;
        case KEY_Q_VAL:
        case KEY_CTRL_VAL:  m_move_down = pressed; break;
        case KEY_SHIFT_VAL: m_move_fast = pressed; break;
        case KEY_UP_VAL:    m_rotate_up = pressed; break;
        case KEY_DOWN_VAL:  m_rotate_down = pressed; break;
        case KEY_LEFT_VAL:  m_rotate_left = pressed; break;
        case KEY_RIGHT_VAL: m_rotate_right = pressed; break;
        default: consumed = false; break;
    }

    return consumed;
}

void CameraController::setMode(int mode, ohao::Camera& camera) {
    Mode new_mode = static_cast<Mode>(mode);
    if (new_mode == m_mode) return;

    m_mode = new_mode;

    if (m_mode == ORBIT) {
        glm::vec3 pos = camera.getPosition();
        glm::vec3 front = camera.getFront();

        m_orbit_distance = 10.0f;
        m_orbit_target_x = pos.x + front.x * m_orbit_distance;
        m_orbit_target_y = pos.y + front.y * m_orbit_distance;
        m_orbit_target_z = pos.z + front.z * m_orbit_distance;

        glm::vec3 dir = glm::normalize(glm::vec3(m_orbit_target_x, m_orbit_target_y, m_orbit_target_z) - pos);
        m_orbit_yaw = glm::degrees(atan2(dir.x, dir.z));
        m_orbit_pitch = glm::degrees(asin(glm::clamp(-dir.y, -1.0f, 1.0f)));

        updateOrbitCamera(camera);
    }
}

void CameraController::updateOrbitCamera(ohao::Camera& camera) {
    float pitch_rad = glm::radians(m_orbit_pitch);
    float yaw_rad = glm::radians(m_orbit_yaw);

    float cam_x = m_orbit_target_x + m_orbit_distance * cos(pitch_rad) * sin(yaw_rad);
    float cam_y = m_orbit_target_y + m_orbit_distance * sin(pitch_rad);
    float cam_z = m_orbit_target_z + m_orbit_distance * cos(pitch_rad) * cos(yaw_rad);

    glm::vec3 cam_pos(cam_x, cam_y, cam_z);
    glm::vec3 target(m_orbit_target_x, m_orbit_target_y, m_orbit_target_z);

    camera.setPosition(cam_pos);

    glm::vec3 dir = glm::normalize(target - cam_pos);
    float cam_pitch = glm::degrees(asin(dir.y));
    float cam_yaw = glm::degrees(atan2(dir.z, dir.x));
    camera.setRotation(cam_pitch, cam_yaw);
}

void CameraController::clearMovementState() {
    m_move_forward = m_move_backward = m_move_left = m_move_right = false;
    m_move_up = m_move_down = m_move_fast = false;
    m_rotate_up = m_rotate_down = m_rotate_left = m_rotate_right = false;
    m_mouse_captured = false;
    m_middle_mouse_captured = false;
    m_always_look = false;
}

void CameraController::focusOnScene(ohao::Scene* scene, ohao::Camera& camera) {
    if (!scene) return;

    glm::vec3 scene_min(FLT_MAX);
    glm::vec3 scene_max(-FLT_MAX);
    bool has_actors = false;

    for (const auto& [id, actor] : scene->getAllActors()) {
        auto transform = actor->getTransform();
        if (!transform) continue;

        glm::vec3 pos = transform->getPosition();
        glm::vec3 scale = transform->getScale();

        scene_min = glm::min(scene_min, pos - glm::abs(scale));
        scene_max = glm::max(scene_max, pos + glm::abs(scale));
        has_actors = true;
    }

    if (!has_actors) return;

    glm::vec3 center = (scene_min + scene_max) * 0.5f;
    glm::vec3 extents = scene_max - scene_min;
    float max_extent = glm::max(extents.x, glm::max(extents.y, extents.z));
    float distance = max_extent * 1.5f;
    if (distance < 2.0f) distance = 2.0f;

    if (m_mode == ORBIT) {
        m_orbit_target_x = center.x;
        m_orbit_target_y = center.y;
        m_orbit_target_z = center.z;
        m_orbit_distance = distance;
        m_orbit_pitch = 30.0f;
        m_orbit_yaw = 45.0f;
        updateOrbitCamera(camera);
    } else {
        glm::vec3 cam_pos = center + glm::vec3(distance * 0.5f, distance * 0.4f, distance * 0.5f);
        camera.setPosition(cam_pos);
        glm::vec3 dir = glm::normalize(center - cam_pos);
        camera.setRotation(glm::degrees(asin(dir.y)), glm::degrees(atan2(dir.z, dir.x)));
    }
}

} // namespace godot
