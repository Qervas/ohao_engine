#include "ohao_viewport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/input.hpp>

// Include OHAO headers
#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/loader/tscn_loader.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/component_factory.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/light_component.hpp"
#include "renderer/components/material_component.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace godot {

void OhaoViewport::_bind_methods() {
    // Methods
    ClassDB::bind_method(D_METHOD("initialize_renderer"), &OhaoViewport::initialize_renderer);
    ClassDB::bind_method(D_METHOD("shutdown_renderer"), &OhaoViewport::shutdown_renderer);
    ClassDB::bind_method(D_METHOD("is_renderer_initialized"), &OhaoViewport::is_renderer_initialized);
    ClassDB::bind_method(D_METHOD("load_tscn", "path"), &OhaoViewport::load_tscn);
    ClassDB::bind_method(D_METHOD("sync_scene"), &OhaoViewport::sync_scene);
    ClassDB::bind_method(D_METHOD("clear_scene"), &OhaoViewport::clear_scene);
    ClassDB::bind_method(D_METHOD("add_cube", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_cube);
    ClassDB::bind_method(D_METHOD("add_sphere", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_sphere);
    ClassDB::bind_method(D_METHOD("add_plane", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_plane);
    ClassDB::bind_method(D_METHOD("add_cylinder", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_cylinder);
    ClassDB::bind_method(D_METHOD("add_directional_light", "name", "position", "direction", "color", "intensity"), &OhaoViewport::add_directional_light);
    ClassDB::bind_method(D_METHOD("add_point_light", "name", "position", "color", "intensity", "range"), &OhaoViewport::add_point_light);
    ClassDB::bind_method(D_METHOD("finish_sync"), &OhaoViewport::finish_sync);
    ClassDB::bind_method(D_METHOD("set_viewport_size", "width", "height"), &OhaoViewport::set_viewport_size);
    ClassDB::bind_method(D_METHOD("get_viewport_size"), &OhaoViewport::get_viewport_size);

    // Properties
    ClassDB::bind_method(D_METHOD("set_render_enabled", "enabled"), &OhaoViewport::set_render_enabled);
    ClassDB::bind_method(D_METHOD("get_render_enabled"), &OhaoViewport::get_render_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "render_enabled"), "set_render_enabled", "get_render_enabled");

    // Camera control properties
    ClassDB::bind_method(D_METHOD("set_mouse_sensitivity", "sensitivity"), &OhaoViewport::set_mouse_sensitivity);
    ClassDB::bind_method(D_METHOD("get_mouse_sensitivity"), &OhaoViewport::get_mouse_sensitivity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mouse_sensitivity", PROPERTY_HINT_RANGE, "0.01,2.0,0.01"), "set_mouse_sensitivity", "get_mouse_sensitivity");

    ClassDB::bind_method(D_METHOD("set_move_speed", "speed"), &OhaoViewport::set_move_speed);
    ClassDB::bind_method(D_METHOD("get_move_speed"), &OhaoViewport::get_move_speed);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "move_speed", PROPERTY_HINT_RANGE, "0.1,50.0,0.1"), "set_move_speed", "get_move_speed");
}

OhaoViewport::OhaoViewport() {
    UtilityFunctions::print("[OHAO] Viewport created");
}

OhaoViewport::~OhaoViewport() {
    shutdown_renderer();
}

void OhaoViewport::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_RESIZED:
            // Update renderer size when control is resized
            if (m_initialized) {
                Vector2 size = get_size();
                if (size.x > 0 && size.y > 0) {
                    set_viewport_size(static_cast<int>(size.x), static_cast<int>(size.y));
                }
            }
            break;
    }
}

void OhaoViewport::_ready() {
    UtilityFunctions::print("[OHAO] Viewport ready - initializing renderer");

    // Get initial size from control
    Vector2 size = get_size();
    if (size.x > 0 && size.y > 0) {
        m_width = static_cast<int>(size.x);
        m_height = static_cast<int>(size.y);
    }

    // Enable focus for keyboard input
    set_focus_mode(FOCUS_ALL);

    initialize_renderer();
}

void OhaoViewport::_process(double delta) {
    if (!m_initialized || !m_render_enabled) {
        return;
    }

    // Update camera movement based on key state
    update_camera_movement(delta);

    // Update physics
    if (m_renderer) {
        m_renderer->updatePhysics(static_cast<float>(delta));
    }

    // Render frame
    if (m_renderer) {
        m_renderer->render();

        // Update Godot texture with rendered pixels
        const uint8_t* pixels = m_renderer->getPixels();
        if (pixels && m_image.is_valid()) {
            // Create PackedByteArray from pixels
            PackedByteArray data;
            data.resize(m_width * m_height * 4);
            memcpy(data.ptrw(), pixels, m_width * m_height * 4);

            // Update image
            m_image->set_data(m_width, m_height, false, Image::FORMAT_RGBA8, data);

            // Update texture
            if (m_texture.is_valid()) {
                m_texture->update(m_image);
            }
        }

        // Request redraw
        queue_redraw();
    }
}

void OhaoViewport::_draw() {
    if (!m_initialized || !m_texture.is_valid()) {
        // Draw placeholder
        draw_rect(Rect2(0, 0, get_size().x, get_size().y), Color(0.1, 0.1, 0.12, 1.0));
        return;
    }

    // Draw the rendered texture
    draw_texture(m_texture, Vector2(0, 0));
}

void OhaoViewport::initialize_renderer() {
    if (m_initialized) {
        return;
    }

    UtilityFunctions::print("[OHAO] Initializing Vulkan renderer...");

    // Create offscreen renderer
    m_renderer = new ohao::OffscreenRenderer(m_width, m_height);
    if (!m_renderer->initialize()) {
        UtilityFunctions::printerr("[OHAO] Failed to initialize renderer!");
        delete m_renderer;
        m_renderer = nullptr;
        return;
    }

    // Create scene
    m_scene = new ohao::Scene("GodotScene");
    m_renderer->setScene(m_scene);

    // Create Godot image and texture
    m_image = Image::create(m_width, m_height, false, Image::FORMAT_RGBA8);
    m_texture = ImageTexture::create_from_image(m_image);

    m_initialized = true;
    UtilityFunctions::print("[OHAO] Renderer initialized: ", m_width, "x", m_height);
}

void OhaoViewport::shutdown_renderer() {
    if (!m_initialized) {
        return;
    }

    UtilityFunctions::print("[OHAO] Shutting down renderer...");

    if (m_renderer) {
        m_renderer->shutdown();
        delete m_renderer;
        m_renderer = nullptr;
    }

    if (m_scene) {
        delete m_scene;
        m_scene = nullptr;
    }

    m_image.unref();
    m_texture.unref();

    m_initialized = false;
}

void OhaoViewport::set_render_enabled(bool enabled) {
    m_render_enabled = enabled;
    if (enabled) {
        queue_redraw();
    }
}

void OhaoViewport::set_viewport_size(int width, int height) {
    if (width == m_width && height == m_height) {
        return;
    }

    m_width = width;
    m_height = height;

    if (m_renderer) {
        m_renderer->resize(width, height);
    }

    // Recreate image and texture
    if (m_initialized) {
        m_image = Image::create(m_width, m_height, false, Image::FORMAT_RGBA8);
        m_texture = ImageTexture::create_from_image(m_image);
    }

    UtilityFunctions::print("[OHAO] Viewport resized: ", width, "x", height);
}

void OhaoViewport::load_tscn(const String& path) {
    if (!m_scene) {
        UtilityFunctions::printerr("[OHAO] Scene not initialized!");
        return;
    }

    // Convert Godot String to std::string
    std::string filepath = path.utf8().get_data();

    UtilityFunctions::print("[OHAO] Loading scene: ", path);

    ohao::loader::TscnLoader loader;
    if (loader.load(filepath)) {
        // Clear existing scene
        m_scene->removeAllActors();

        // Create scene from .tscn
        if (loader.createScene(m_scene)) {
            UtilityFunctions::print("[OHAO] Scene loaded successfully");

            // Apply camera if found
            const auto& parsed = loader.getParsedScene();
            if (parsed.camera.valid) {
                auto& camera = m_renderer->getCamera();
                camera.setPosition(parsed.camera.position);
                glm::vec3 euler = glm::eulerAngles(parsed.camera.rotation);
                camera.setRotation(glm::degrees(euler.x), glm::degrees(euler.y));
            }
        } else {
            UtilityFunctions::printerr("[OHAO] Failed to create scene: ", loader.getError().c_str());
        }
    } else {
        UtilityFunctions::printerr("[OHAO] Failed to load .tscn: ", loader.getError().c_str());
    }
}

void OhaoViewport::sync_scene() {
    // This is now handled by GDScript calling clear_scene(), add_* methods, then finish_sync()
    UtilityFunctions::print("[OHAO] sync_scene() called - use clear_scene(), add_* methods, and finish_sync() instead");
}

void OhaoViewport::clear_scene() {
    if (!m_scene) {
        UtilityFunctions::printerr("[OHAO] Scene not initialized!");
        return;
    }

    m_scene->removeAllActors();
    UtilityFunctions::print("[OHAO] Scene cleared");
}

// Helper to convert Godot vectors to GLM
static glm::vec3 to_glm(const Vector3& v) {
    return glm::vec3(v.x, v.y, v.z);
}

static glm::vec3 to_glm_color(const Color& c) {
    return glm::vec3(c.r, c.g, c.b);
}

void OhaoViewport::add_cube(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::Cube);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
            transform->setRotation(glm::quat(glm::radians(to_glm(rotation))));
            transform->setScale(to_glm(scale));
        }
        auto material = actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = to_glm_color(color);
        }
    }
}

void OhaoViewport::add_sphere(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::Sphere);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
            transform->setRotation(glm::quat(glm::radians(to_glm(rotation))));
            transform->setScale(to_glm(scale));
        }
        auto material = actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = to_glm_color(color);
        }
    }
}

void OhaoViewport::add_plane(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::Platform);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
            transform->setRotation(glm::quat(glm::radians(to_glm(rotation))));
            transform->setScale(to_glm(scale));
        }
        auto material = actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = to_glm_color(color);
        }
    }
}

void OhaoViewport::add_cylinder(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::Cylinder);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
            transform->setRotation(glm::quat(glm::radians(to_glm(rotation))));
            transform->setScale(to_glm(scale));
        }
        auto material = actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = to_glm_color(color);
        }
    }
}

void OhaoViewport::add_directional_light(const String& name, const Vector3& position, const Vector3& direction, const Color& color, float intensity) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::DirectionalLight);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
        }
        auto light = actor->getComponent<ohao::LightComponent>();
        if (light) {
            light->setDirection(glm::normalize(to_glm(direction)));
            light->setColor(to_glm_color(color));
            light->setIntensity(intensity);
        }
    }
}

void OhaoViewport::add_point_light(const String& name, const Vector3& position, const Color& color, float intensity, float range) {
    if (!m_scene) return;

    std::string actorName = name.utf8().get_data();
    auto actor = m_scene->createActorWithComponents(actorName, ohao::PrimitiveType::PointLight);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
        }
        auto light = actor->getComponent<ohao::LightComponent>();
        if (light) {
            light->setColor(to_glm_color(color));
            light->setIntensity(intensity);
            light->setRange(range);
        }
    }
}

void OhaoViewport::finish_sync() {
    if (!m_renderer) {
        UtilityFunctions::printerr("[OHAO] Renderer not initialized!");
        return;
    }

    // Rebuild vertex/index buffers from scene
    if (m_renderer->updateSceneBuffers()) {
        UtilityFunctions::print("[OHAO] Scene buffers updated successfully");
    } else {
        UtilityFunctions::print("[OHAO] No meshes to render in scene");
    }
}

// ===== FPS Camera Controls =====

void OhaoViewport::_gui_input(const Ref<InputEvent>& p_event) {
    if (!m_initialized || !m_renderer) {
        return;
    }

    // Handle mouse motion
    Ref<InputEventMouseMotion> motion_event = p_event;
    if (motion_event.is_valid()) {
        handle_mouse_motion(motion_event);
        return;
    }

    // Handle mouse button
    Ref<InputEventMouseButton> button_event = p_event;
    if (button_event.is_valid()) {
        handle_mouse_button(button_event);
        return;
    }

    // Handle keyboard
    Ref<InputEventKey> key_event = p_event;
    if (key_event.is_valid()) {
        handle_key(key_event);
        return;
    }
}

void OhaoViewport::handle_mouse_motion(const Ref<InputEventMouseMotion>& event) {
    // Only rotate camera when right mouse button is held
    if (!m_mouse_captured) {
        return;
    }

    Vector2 relative = event->get_relative();

    // Apply mouse sensitivity and rotate camera
    // Yaw (horizontal) and Pitch (vertical)
    float delta_yaw = -relative.x * m_mouse_sensitivity;
    float delta_pitch = -relative.y * m_mouse_sensitivity;

    ohao::Camera& camera = m_renderer->getCamera();
    camera.rotate(delta_pitch, delta_yaw);
}

void OhaoViewport::handle_mouse_button(const Ref<InputEventMouseButton>& event) {
    MouseButton button = event->get_button_index();

    if (button == MOUSE_BUTTON_RIGHT) {
        if (event->is_pressed()) {
            // Start capturing mouse for camera look
            m_mouse_captured = true;
            grab_focus();  // Ensure we get keyboard events
        } else {
            // Stop capturing
            m_mouse_captured = false;
        }
    }

    // Handle scroll wheel for zoom/move forward-back
    if (button == MOUSE_BUTTON_WHEEL_UP && event->is_pressed()) {
        ohao::Camera& camera = m_renderer->getCamera();
        glm::vec3 forward = camera.getFront();
        camera.move(forward * 0.5f);
    }
    if (button == MOUSE_BUTTON_WHEEL_DOWN && event->is_pressed()) {
        ohao::Camera& camera = m_renderer->getCamera();
        glm::vec3 forward = camera.getFront();
        camera.move(-forward * 0.5f);
    }
}

void OhaoViewport::handle_key(const Ref<InputEventKey>& event) {
    Key keycode = event->get_keycode();
    bool pressed = event->is_pressed();

    switch (keycode) {
        case KEY_W:
            m_move_forward = pressed;
            break;
        case KEY_S:
            m_move_backward = pressed;
            break;
        case KEY_A:
            m_move_left = pressed;
            break;
        case KEY_D:
            m_move_right = pressed;
            break;
        case KEY_E:
        case KEY_SPACE:
            m_move_up = pressed;
            break;
        case KEY_Q:
        case KEY_CTRL:
            m_move_down = pressed;
            break;
        case KEY_SHIFT:
            m_move_fast = pressed;
            break;
        // Arrow keys for camera rotation
        case KEY_UP:
            m_rotate_up = pressed;
            break;
        case KEY_DOWN:
            m_rotate_down = pressed;
            break;
        case KEY_LEFT:
            m_rotate_left = pressed;
            break;
        case KEY_RIGHT:
            m_rotate_right = pressed;
            break;
        default:
            break;
    }

    // Accept the input event to prevent propagation
    if (m_move_forward || m_move_backward || m_move_left || m_move_right ||
        m_move_up || m_move_down || m_rotate_up || m_rotate_down ||
        m_rotate_left || m_rotate_right) {
        accept_event();
    }
}

void OhaoViewport::update_camera_movement(double delta) {
    if (!m_renderer) {
        return;
    }

    ohao::Camera& camera = m_renderer->getCamera();

    // Handle arrow key rotation
    if (m_rotate_up || m_rotate_down || m_rotate_left || m_rotate_right) {
        float rotSpeed = m_rotation_speed * static_cast<float>(delta);
        if (m_move_fast) {
            rotSpeed *= m_fast_move_multiplier;
        }

        float deltaYaw = 0.0f;
        float deltaPitch = 0.0f;

        if (m_rotate_left) {
            deltaYaw -= rotSpeed;
        }
        if (m_rotate_right) {
            deltaYaw += rotSpeed;
        }
        if (m_rotate_up) {
            deltaPitch += rotSpeed;
        }
        if (m_rotate_down) {
            deltaPitch -= rotSpeed;
        }

        camera.rotate(deltaPitch, deltaYaw);
    }

    // Check if any movement keys are pressed
    if (!m_move_forward && !m_move_backward && !m_move_left && !m_move_right &&
        !m_move_up && !m_move_down) {
        return;
    }

    // Calculate speed
    float speed = m_move_speed * static_cast<float>(delta);
    if (m_move_fast) {
        speed *= m_fast_move_multiplier;
    }

    // Get camera directions
    glm::vec3 front = camera.getFront();
    glm::vec3 right = camera.getRight();
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);  // World up for vertical movement

    // Calculate movement
    glm::vec3 movement(0.0f);

    if (m_move_forward) {
        movement += front * speed;
    }
    if (m_move_backward) {
        movement -= front * speed;
    }
    if (m_move_right) {
        movement += right * speed;
    }
    if (m_move_left) {
        movement -= right * speed;
    }
    if (m_move_up) {
        movement += up * speed;
    }
    if (m_move_down) {
        movement -= up * speed;
    }

    // Apply movement
    camera.move(movement);
}

} // namespace godot
