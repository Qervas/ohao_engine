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
    class DeferredRenderer;
    class PostProcessingPipeline;
}

namespace godot {

/**
 * OhaoViewport - Control that renders using OHAO's Vulkan AAA renderer
 *
 * This node renders the scene using OHAO's custom Vulkan pipeline
 * and displays the result as a texture in Godot's UI.
 *
 * Now with full AAA renderer support:
 * - Deferred rendering with G-Buffer
 * - Cascaded Shadow Maps (CSM)
 * - SSAO, SSR, Volumetrics
 * - Bloom, TAA, Motion Blur, DoF
 * - HDR with tonemapping
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

    // === AAA Render Settings ===
    int m_render_mode = 0;  // Default to Forward until deferred output is integrated (0=Forward, 1=Deferred)

    // Post-processing toggles
    bool m_bloom_enabled = true;
    bool m_taa_enabled = true;
    bool m_ssao_enabled = true;
    bool m_ssr_enabled = false;  // More expensive, off by default
    bool m_volumetrics_enabled = false;  // Off by default
    bool m_motion_blur_enabled = false;  // Off by default
    bool m_dof_enabled = false;  // Off by default
    bool m_tonemapping_enabled = true;

    // Tonemapping settings
    int m_tonemap_operator = 0;  // ACES
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Bloom settings
    float m_bloom_threshold = 1.0f;
    float m_bloom_intensity = 0.5f;

    // SSAO settings
    float m_ssao_radius = 0.5f;
    float m_ssao_intensity = 1.0f;

    // SSR settings
    float m_ssr_max_distance = 100.0f;
    float m_ssr_thickness = 0.5f;

    // Volumetric settings
    float m_volumetric_density = 0.02f;
    float m_volumetric_scattering = 0.8f;
    Color m_fog_color = Color(0.7f, 0.8f, 1.0f);

    // Motion blur settings
    float m_motion_blur_intensity = 1.0f;
    int m_motion_blur_samples = 16;

    // DoF settings
    float m_dof_focus_distance = 5.0f;
    float m_dof_aperture = 2.8f;
    float m_dof_max_blur = 8.0f;

    // TAA settings
    float m_taa_blend_factor = 0.1f;

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

    // Apply current render settings to the deferred renderer
    void apply_render_settings();

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

    // === AAA Render Mode ===
    void set_render_mode(int mode);
    int get_render_mode() const { return m_render_mode; }

    // === Post-Processing Toggles ===
    void set_bloom_enabled(bool enabled);
    bool get_bloom_enabled() const { return m_bloom_enabled; }

    void set_taa_enabled(bool enabled);
    bool get_taa_enabled() const { return m_taa_enabled; }

    void set_ssao_enabled(bool enabled);
    bool get_ssao_enabled() const { return m_ssao_enabled; }

    void set_ssr_enabled(bool enabled);
    bool get_ssr_enabled() const { return m_ssr_enabled; }

    void set_volumetrics_enabled(bool enabled);
    bool get_volumetrics_enabled() const { return m_volumetrics_enabled; }

    void set_motion_blur_enabled(bool enabled);
    bool get_motion_blur_enabled() const { return m_motion_blur_enabled; }

    void set_dof_enabled(bool enabled);
    bool get_dof_enabled() const { return m_dof_enabled; }

    void set_tonemapping_enabled(bool enabled);
    bool get_tonemapping_enabled() const { return m_tonemapping_enabled; }

    // === Tonemapping Settings ===
    void set_tonemap_operator(int op);
    int get_tonemap_operator() const { return m_tonemap_operator; }

    void set_exposure(float exposure);
    float get_exposure() const { return m_exposure; }

    void set_gamma(float gamma);
    float get_gamma() const { return m_gamma; }

    // === Bloom Settings ===
    void set_bloom_threshold(float threshold);
    float get_bloom_threshold() const { return m_bloom_threshold; }

    void set_bloom_intensity(float intensity);
    float get_bloom_intensity() const { return m_bloom_intensity; }

    // === SSAO Settings ===
    void set_ssao_radius(float radius);
    float get_ssao_radius() const { return m_ssao_radius; }

    void set_ssao_intensity(float intensity);
    float get_ssao_intensity() const { return m_ssao_intensity; }

    // === SSR Settings ===
    void set_ssr_max_distance(float dist);
    float get_ssr_max_distance() const { return m_ssr_max_distance; }

    void set_ssr_thickness(float thickness);
    float get_ssr_thickness() const { return m_ssr_thickness; }

    // === Volumetric Settings ===
    void set_volumetric_density(float density);
    float get_volumetric_density() const { return m_volumetric_density; }

    void set_volumetric_scattering(float g);
    float get_volumetric_scattering() const { return m_volumetric_scattering; }

    void set_fog_color(const Color& color);
    Color get_fog_color() const { return m_fog_color; }

    // === Motion Blur Settings ===
    void set_motion_blur_intensity(float intensity);
    float get_motion_blur_intensity() const { return m_motion_blur_intensity; }

    void set_motion_blur_samples(int samples);
    int get_motion_blur_samples() const { return m_motion_blur_samples; }

    // === DoF Settings ===
    void set_dof_focus_distance(float distance);
    float get_dof_focus_distance() const { return m_dof_focus_distance; }

    void set_dof_aperture(float aperture);
    float get_dof_aperture() const { return m_dof_aperture; }

    void set_dof_max_blur(float blur);
    float get_dof_max_blur() const { return m_dof_max_blur; }

    // === TAA Settings ===
    void set_taa_blend_factor(float factor);
    float get_taa_blend_factor() const { return m_taa_blend_factor; }

    // === Utility ===
    // Get current renderer stats (for debug display)
    Dictionary get_render_stats() const;
};

} // namespace godot
