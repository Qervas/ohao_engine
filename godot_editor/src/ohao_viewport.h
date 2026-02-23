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

#include "audio_manager.h"
#include "camera_controller.h"
#include "render_settings.h"
#include "scene_sync.h"
#include "selection_controller.h"

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class Scene;
    class DeferredRenderer;
    class PostProcessingPipeline;
    class PickingSystem;
    class Actor;
}

namespace godot {

/**
 * OhaoViewport - Thin orchestrator for OHAO's Vulkan renderer in Godot
 *
 * Delegates to focused sub-objects:
 *   CameraController    - FPS/Orbit camera, input handling
 *   RenderSettings      - Post-processing configuration
 *   SceneSync           - Godot <-> OHAO scene bridge
 *   SelectionController - Object picking and selection
 *
 * GDScript bindings remain here for Godot's ClassDB registration,
 * but each method delegates to the appropriate sub-object.
 */
class OhaoViewport : public Control {
    GDCLASS(OhaoViewport, Control)

private:
    bool m_initialized = false;
    bool m_render_enabled = true;

    // OHAO renderer & scene (owned)
    ohao::OffscreenRenderer* m_renderer = nullptr;
    ohao::Scene* m_scene = nullptr;

    // Godot texture for display
    Ref<Image> m_image;
    Ref<ImageTexture> m_texture;

    // Viewport size
    int m_width = 800;
    int m_height = 600;

    // === Sub-objects (composition) ===
    AudioManager m_audio;
    CameraController m_camera;
    RenderSettings m_render_settings;
    SceneSync m_scene_sync;
    SelectionController m_selection;

    // === Physics State (small, stays here) ===
    bool m_physics_playing = false;
    float m_physics_speed = 1.0f;

    // === Input Mode (EDITOR vs GAME) ===
    bool m_game_mode = false;
    bool m_picking_enabled = true;

    // === Wireframe Mode ===
    bool m_wireframe_enabled = false;

    // === Grid Overlay ===
    bool m_grid_enabled = true;

    // === Gizmo State ===
    int m_gizmo_mode = 0;
    bool m_gizmo_enabled = true;

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

    // OHAO Engine control
    void initialize_renderer();
    void shutdown_renderer();
    bool is_renderer_initialized() const { return m_initialized; }
    bool has_scene_meshes() const;

    // Access OHAO scene (for physics body integration)
    ohao::Scene* get_ohao_scene() const { return m_scene; }
    ohao::OffscreenRenderer* get_ohao_renderer() const { return m_renderer; }

    // Rendering
    void set_render_enabled(bool enabled);
    bool get_render_enabled() const { return m_render_enabled; }

    // Scene management (delegates to SceneSync)
    void load_tscn(const String& path);
    void sync_scene();
    void clear_scene();
    void sync_from_godot(Node* root_node);
    int get_synced_object_count() const { return m_scene_sync.getSyncedObjectCount(); }

    // Scene building from GDScript (delegates to SceneSync)
    void add_cube(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_sphere(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_plane(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_cylinder(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color);
    void add_directional_light(const String& name, const Vector3& position, const Vector3& direction, const Color& color, float intensity);
    void add_point_light(const String& name, const Vector3& position, const Color& color, float intensity, float range);
    void finish_sync();

    // Size
    void set_viewport_size(int width, int height);
    Vector2i get_viewport_size() const { return Vector2i(m_width, m_height); }

    // Camera controls (delegates to CameraController)
    void set_mouse_sensitivity(float sensitivity) { m_camera.setMouseSensitivity(sensitivity); }
    float get_mouse_sensitivity() const { return m_camera.getMouseSensitivity(); }
    void set_move_speed(float speed) { m_camera.setMoveSpeed(speed); }
    float get_move_speed() const { return m_camera.getMoveSpeed(); }
    void set_camera_mode(int mode);
    int get_camera_mode() const { return m_camera.getMode(); }
    void focus_on_scene();

    // Camera position/rotation for game mode (GDScript drives camera)
    void set_camera_position(const Vector3& pos);
    Vector3 get_camera_position() const;
    void set_camera_rotation_deg(float pitch, float yaw);
    Vector3 get_camera_forward() const;

    // Input mode (EDITOR=0, GAME=1)
    void set_input_mode(int mode);
    int get_input_mode() const { return m_game_mode ? 1 : 0; }

    // Picking (delegates to SelectionController)
    void pick_object_at(const Vector2& screen_pos);
    String get_selected_actor_name() const { return m_selection.getSelectedActorName(); }
    void set_picking_enabled(bool enabled) { m_picking_enabled = enabled; }
    bool get_picking_enabled() const { return m_picking_enabled; }

    // === AAA Render Settings (delegates to RenderSettings) ===
    void set_render_mode(int mode);
    int get_render_mode() const { return m_render_settings.getRenderMode(); }

    // Post-processing toggles
    void set_bloom_enabled(bool enabled);
    bool get_bloom_enabled() const { return m_render_settings.getBloomEnabled(); }
    void set_taa_enabled(bool enabled);
    bool get_taa_enabled() const { return m_render_settings.getTAAEnabled(); }
    void set_ssao_enabled(bool enabled);
    bool get_ssao_enabled() const { return m_render_settings.getSSAOEnabled(); }
    void set_ssr_enabled(bool enabled);
    bool get_ssr_enabled() const { return m_render_settings.getSSREnabled(); }
    void set_volumetrics_enabled(bool enabled);
    bool get_volumetrics_enabled() const { return m_render_settings.getVolumetricsEnabled(); }
    void set_motion_blur_enabled(bool enabled);
    bool get_motion_blur_enabled() const { return m_render_settings.getMotionBlurEnabled(); }
    void set_dof_enabled(bool enabled);
    bool get_dof_enabled() const { return m_render_settings.getDoFEnabled(); }
    void set_tonemapping_enabled(bool enabled);
    bool get_tonemapping_enabled() const { return m_render_settings.getTonemappingEnabled(); }

    // Tonemapping settings
    void set_tonemap_operator(int op);
    int get_tonemap_operator() const { return m_render_settings.getTonemapOperator(); }
    void set_exposure(float exposure);
    float get_exposure() const { return m_render_settings.getExposure(); }
    void set_gamma(float gamma);
    float get_gamma() const { return m_render_settings.getGamma(); }

    // Bloom settings
    void set_bloom_threshold(float threshold);
    float get_bloom_threshold() const { return m_render_settings.getBloomThreshold(); }
    void set_bloom_intensity(float intensity);
    float get_bloom_intensity() const { return m_render_settings.getBloomIntensity(); }

    // SSAO settings
    void set_ssao_radius(float radius);
    float get_ssao_radius() const { return m_render_settings.getSSAORadius(); }
    void set_ssao_intensity(float intensity);
    float get_ssao_intensity() const { return m_render_settings.getSSAOIntensity(); }

    // SSR settings
    void set_ssr_max_distance(float dist);
    float get_ssr_max_distance() const { return m_render_settings.getSSRMaxDistance(); }
    void set_ssr_thickness(float thickness);
    float get_ssr_thickness() const { return m_render_settings.getSSRThickness(); }

    // Volumetric settings
    void set_volumetric_density(float density);
    float get_volumetric_density() const { return m_render_settings.getVolumetricDensity(); }
    void set_volumetric_scattering(float g);
    float get_volumetric_scattering() const { return m_render_settings.getVolumetricScattering(); }
    void set_fog_color(const Color& color);
    Color get_fog_color() const;

    // Motion blur settings
    void set_motion_blur_intensity(float intensity);
    float get_motion_blur_intensity() const { return m_render_settings.getMotionBlurIntensity(); }
    void set_motion_blur_samples(int samples);
    int get_motion_blur_samples() const { return m_render_settings.getMotionBlurSamples(); }

    // DoF settings
    void set_dof_focus_distance(float distance);
    float get_dof_focus_distance() const { return m_render_settings.getDoFFocusDistance(); }
    void set_dof_aperture(float aperture);
    float get_dof_aperture() const { return m_render_settings.getDoFAperture(); }
    void set_dof_max_blur(float blur);
    float get_dof_max_blur() const { return m_render_settings.getDoFMaxBlur(); }

    // TAA settings
    void set_taa_blend_factor(float factor);
    float get_taa_blend_factor() const { return m_render_settings.getTAABlendFactor(); }

    // === Physics Controls ===
    void play_physics();
    void pause_physics();
    void step_physics();
    void stop_physics();
    void set_physics_speed(float speed);
    float get_physics_speed() const { return m_physics_speed; }
    bool is_physics_playing() const { return m_physics_playing; }

    // === Wireframe Mode ===
    void set_wireframe_enabled(bool enabled);
    bool get_wireframe_enabled() const { return m_wireframe_enabled; }

    // === Grid Overlay ===
    void set_grid_enabled(bool enabled);
    bool get_grid_enabled() const { return m_grid_enabled; }

    // === Model Import ===
    void import_model(const String& path);

    // === Gizmo Controls ===
    void set_gizmo_mode(int mode);
    int get_gizmo_mode() const { return m_gizmo_mode; }
    void set_gizmo_enabled(bool enabled);
    bool get_gizmo_enabled() const { return m_gizmo_enabled; }

    // === Particles ===
    void spawn_particles(const Vector3& position, int type);
    void spawn_particles_directed(const Vector3& position, int type, const Vector3& direction);

    // === Texture / Material API ===
    void set_actor_texture(const String& actor_name, const String& texture_path);
    void set_actor_normal_map(const String& actor_name, const String& normal_path);
    void set_actor_pbr(const String& actor_name, float metallic, float roughness);
    void set_actor_material_preset(const String& actor_name, const String& preset_name);

    // === Audio ===
    int play_sound(const String& path, int category, bool loop, float volume);
    int play_sound_at(const String& path, const Vector3& position, int category, bool loop, float volume);
    void stop_sound(int handle);
    void pause_sound(int handle);
    void resume_sound(int handle);
    void set_sound_volume(int handle, float volume);
    void set_sound_position(int handle, const Vector3& position);
    void set_category_volume(int category, float volume);
    float get_category_volume(int category) const;
    void stop_category(int category);
    void pause_category(int category);
    void resume_category(int category);
    void set_master_volume(float volume);
    float get_master_volume() const;
    void stop_all_sounds();

    // === Utility ===
    Dictionary get_render_stats() const;
};

} // namespace godot
