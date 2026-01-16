#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class Scene;
}

namespace godot {

/**
 * OhaoViewport - Control that renders using OHAO's Vulkan renderer
 *
 * This node renders the scene using OHAO's custom Vulkan pipeline
 * and displays the result as a texture in Godot's UI.
 *
 * FPS-style camera controls:
 * - Right-click + drag to look around
 * - WASD to move
 * - Shift to move faster
 */
class OhaoViewport : public Control {
    GDCLASS(OhaoViewport, Control)

private:
    bool m_initialized = false;
    bool m_render_enabled = true;

    // OHAO offscreen renderer
    ohao::OffscreenRenderer* m_renderer = nullptr;
    ohao::Scene* m_scene = nullptr;

    // Godot texture for display
    Ref<Image> m_image;
    Ref<ImageTexture> m_texture;

    // Viewport size
    int m_width = 800;
    int m_height = 600;

    // Camera controls state
    bool m_mouse_captured = false;
    float m_mouse_sensitivity = 0.3f;
    float m_move_speed = 5.0f;
    float m_fast_move_multiplier = 2.5f;

    // Movement state
    bool m_move_forward = false;
    bool m_move_backward = false;
    bool m_move_left = false;
    bool m_move_right = false;
    bool m_move_up = false;
    bool m_move_down = false;
    bool m_move_fast = false;

    // Rotation state (arrow keys)
    bool m_rotate_up = false;
    bool m_rotate_down = false;
    bool m_rotate_left = false;
    bool m_rotate_right = false;
    float m_rotation_speed = 90.0f;  // degrees per second

protected:
    static void _bind_methods();

public:
    OhaoViewport();
    ~OhaoViewport();

    // Lifecycle
    void _ready() override;
    void _process(double delta) override;
    void _draw() override;
    void _notification(int p_what);
    void _gui_input(const Ref<InputEvent>& p_event) override;

    // Camera control helpers
    void handle_mouse_motion(const Ref<InputEventMouseMotion>& event);
    void handle_mouse_button(const Ref<InputEventMouseButton>& event);
    void handle_key(const Ref<InputEventKey>& event);
    void update_camera_movement(double delta);

    // OHAO Engine control
    void initialize_renderer();
    void shutdown_renderer();
    bool is_renderer_initialized() const { return m_initialized; }
    bool has_scene_meshes() const;

    // Rendering
    void set_render_enabled(bool enabled);
    bool get_render_enabled() const { return m_render_enabled; }

    // Scene management
    void load_tscn(const String& path);
    void sync_scene();
    void clear_scene();

    // Sync from Godot scene tree
    void sync_from_godot(Node* root_node);
    int get_synced_object_count() const { return m_synced_object_count; }

private:
    void traverse_and_sync(Node* node);
    void count_syncable_objects(Node* node);
    int m_synced_object_count = 0;

public:

    // Scene building from GDScript
    void add_cube(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_sphere(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_plane(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_cylinder(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_directional_light(const String& name, const Vector3& position, const Vector3& direction, const Color& color, float intensity);
    void add_point_light(const String& name, const Vector3& position, const Color& color, float intensity, float range);

    // Call after adding objects to rebuild buffers
    void finish_sync();

    // Size
    void set_viewport_size(int width, int height);
    Vector2i get_viewport_size() const { return Vector2i(m_width, m_height); }

    // Camera controls
    void set_mouse_sensitivity(float sensitivity) { m_mouse_sensitivity = sensitivity; }
    float get_mouse_sensitivity() const { return m_mouse_sensitivity; }
    void set_move_speed(float speed) { m_move_speed = speed; }
    float get_move_speed() const { return m_move_speed; }
};

} // namespace godot
