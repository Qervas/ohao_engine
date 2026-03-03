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

#include "actor_controller.h"
#include "audio_manager.h"
#include "camera_controller.h"
#include "physics_controller.h"
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
 *   ActorController     - Actor transforms, lifecycle, physics props, materials
 *   CameraController    - FPS/Orbit camera, input handling
 *   PhysicsController   - Physics sim, raycasting, constraints, characters
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
    ActorController m_actors;
    AudioManager m_audio;
    CameraController m_camera;
    PhysicsController m_physics;
    RenderSettings m_render_settings;
    SceneSync m_scene_sync;
    SelectionController m_selection;

    // === Input Mode (EDITOR vs GAME) ===
    bool m_game_mode = false;
    bool m_picking_enabled = true;

    // === Wireframe Mode ===
    bool m_wireframe_enabled = false;

    // === Gizmo State ===
    int m_gizmo_mode = 0;
    bool m_gizmo_enabled = true;

    // === Time of Day ===
    float m_time_of_day = 12.0f;  // hours [0, 24)

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
    void set_ssgi_enabled(bool enabled);
    bool get_ssgi_enabled() const { return m_render_settings.getSSGIEnabled(); }
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

    // SSGI settings
    void set_ssgi_radius(float radius);
    float get_ssgi_radius() const { return m_render_settings.getSSGIRadius(); }
    void set_ssgi_intensity(float intensity);
    float get_ssgi_intensity() const { return m_render_settings.getSSGIIntensity(); }
    void set_ssgi_sample_count(int count);
    int get_ssgi_sample_count() const { return m_render_settings.getSSGISampleCount(); }

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

    // Sky settings
    void set_sky_enabled(bool enabled);
    bool get_sky_enabled() const { return m_render_settings.getSkyEnabled(); }
    void set_sun_direction(const Vector3& dir);
    Vector3 get_sun_direction() const;
    void set_sky_turbidity(float turbidity);
    float get_sky_turbidity() const { return m_render_settings.getSkyTurbidity(); }
    void set_sky_intensity(float intensity);
    float get_sky_intensity() const { return m_render_settings.getSkyIntensity(); }

    // Time of day (drives sun direction, sky intensity, turbidity automatically)
    void set_time_of_day(float hours);
    float get_time_of_day() const { return m_time_of_day; }

    // Rain settings
    void set_rain_enabled(bool enabled);
    bool get_rain_enabled() const { return m_render_settings.getRainEnabled(); }
    void set_rain_intensity(float v);
    float get_rain_intensity() const { return m_render_settings.getRainIntensity(); }
    void set_rain_wind_x(float v);
    float get_rain_wind_x() const { return m_render_settings.getRainWindX(); }

    // Wetness rate settings (how fast surfaces wet/dry)
    void  set_wetness_rate(float v);
    float get_wetness_rate() const { return m_render_settings.getWetnessRate(); }
    void  set_drying_rate(float v);
    float get_drying_rate() const  { return m_render_settings.getDryingRate(); }
    // Read-only — query current surface wetness level (driven by rain automatically)
    float get_surface_wetness() const;

    // Lightning settings
    void  set_lightning_enabled(bool v);
    bool  get_lightning_enabled() const { return m_render_settings.getLightningEnabled(); }
    void  set_lightning_interval(float v);
    float get_lightning_interval() const { return m_render_settings.getLightningInterval(); }
    void  set_lightning_brightness(float v);
    float get_lightning_brightness() const { return m_render_settings.getLightningBrightness(); }
    // Manual trigger — fires a strike immediately regardless of timer
    void  trigger_lightning();

    // Snow settings
    void  set_snow_enabled(bool v);
    bool  get_snow_enabled() const      { return m_render_settings.getSnowEnabled(); }
    void  set_snow_intensity(float v);
    float get_snow_intensity() const    { return m_render_settings.getSnowIntensity(); }
    void  set_snow_wind_x(float v);
    float get_snow_wind_x() const       { return m_render_settings.getSnowWindX(); }
    void  set_snow_accum_rate(float v);
    float get_snow_accum_rate() const   { return m_render_settings.getSnowAccumRate(); }
    void  set_snow_melt_rate(float v);
    float get_snow_melt_rate() const    { return m_render_settings.getSnowMeltRate(); }
    // Read-only — query current snow accumulation on ground (driven by snow automatically)
    float get_snow_accumulation() const;

    // Cloud settings
    void set_cloud_enabled(bool enabled);
    bool get_cloud_enabled() const { return m_render_settings.getCloudEnabled(); }
    void set_cloud_coverage(float v);
    float get_cloud_coverage() const { return m_render_settings.getCloudCoverage(); }
    void set_cloud_density(float v);
    float get_cloud_density() const { return m_render_settings.getCloudDensity(); }
    void set_cloud_altitude_min(float v);
    float get_cloud_altitude_min() const { return m_render_settings.getCloudAltMin(); }
    void set_cloud_altitude_max(float v);
    float get_cloud_altitude_max() const { return m_render_settings.getCloudAltMax(); }
    void set_cloud_speed(float v);
    float get_cloud_speed() const { return m_render_settings.getCloudSpeed(); }

    // Sand (sandstorm) settings
    void  set_sand_enabled(bool v);
    bool  get_sand_enabled() const    { return m_render_settings.getSandEnabled(); }
    void  set_sand_intensity(float v);
    float get_sand_intensity() const  { return m_render_settings.getSandIntensity(); }
    void  set_sand_wind_x(float v);
    float get_sand_wind_x() const     { return m_render_settings.getSandWindX(); }

    // Frost settings
    void  set_frost_accum_rate(float v);
    float get_frost_accum_rate() const { return m_render_settings.getFrostAccumRate(); }
    void  set_frost_melt_rate(float v);
    float get_frost_melt_rate() const  { return m_render_settings.getFrostMeltRate(); }
    float get_frost_cover() const;

    // God rays settings
    void  set_god_rays_enabled(bool v);
    bool  get_god_rays_enabled() const    { return m_render_settings.getGodRaysEnabled(); }
    void  set_god_rays_intensity(float v);
    float get_god_rays_intensity() const  { return m_render_settings.getGodRaysIntensity(); }

    // Aurora settings
    void  set_aurora_enabled(bool v);
    bool  get_aurora_enabled() const    { return m_render_settings.getAuroraEnabled(); }
    void  set_aurora_intensity(float v);
    float get_aurora_intensity() const  { return m_render_settings.getAuroraIntensity(); }
    void  set_aurora_hue(float v);
    float get_aurora_hue() const        { return m_render_settings.getAuroraHue(); }

    // Rainbow settings
    void  set_rainbow_enabled(bool v);
    bool  get_rainbow_enabled() const  { return m_render_settings.getRainbowEnabled(); }

    // Heat haze settings
    void  set_heat_haze_enabled(bool v);
    bool  get_heat_haze_enabled() const    { return m_render_settings.getHeatHazeEnabled(); }
    void  set_heat_haze_intensity(float v);
    float get_heat_haze_intensity() const  { return m_render_settings.getHeatHazeIntensity(); }
    void  set_heat_haze_frequency(float v);
    float get_heat_haze_frequency() const  { return m_render_settings.getHeatHazeFrequency(); }

    // === Terrain ===
    void  set_terrain_enabled(bool v);
    bool  get_terrain_enabled() const      { return m_render_settings.getTerrainEnabled(); }
    void  set_terrain_height_scale(float v);
    float get_terrain_height_scale() const { return m_render_settings.getTerrainHeightScale(); }
    void  set_terrain_size(float v);
    float get_terrain_size() const         { return m_render_settings.getTerrainSize(); }
    // Texture paths — resolved from res:// and forwarded to DeferredRenderer
    void  set_terrain_heightmap(const String& path);
    void  set_terrain_splat_map(const String& path);
    void  set_terrain_layer(int layer_index, const String& albedo_path, const String& normal_path);
    // Runtime splatmap painting: channel 0=grass,1=dirt,2=rock,3=snow
    void  paint_terrain_splat(Vector3 world_pos, int channel, float radius, float strength);
    void  clear_terrain_splat();
    // Procedural terrain generation
    void  set_terrain_type(int type);            // 0=external heightmap, 1-6=procedural
    void  set_terrain_frequency(float f);
    void  set_terrain_octaves(int n);
    void  set_terrain_offset(Vector2 off);
    void  set_terrain_gen_resolution(int r);
    void  set_terrain_macro_variation(const String& path);
    void  generate_terrain();
    // Sync terrain visual to Jolt physics heightfield (blocking GPU readback).
    // Call once after generate_terrain() or set_terrain_heightmap().
    // Returns true on success. Objects will collide with terrain after this call.
    bool  sync_terrain_physics();
    void  sync_terrain_weather_friction();
    // Multi-tile terrain streaming API
    int   add_terrain_tile(float offset_x, float offset_z);
    void  clear_terrain_tiles();
    void  set_terrain_tile_cull_radius(float r);

    // === Water ===
    void  set_water_enabled(bool v);
    bool  get_water_enabled() const        { return m_render_settings.getWaterEnabled(); }
    void  set_water_level(float v);
    float get_water_level() const          { return m_render_settings.getWaterLevel(); }
    void  set_water_size(float v);
    float get_water_size() const           { return m_render_settings.getWaterSize(); }
    void  set_water_foam_intensity(float v);
    float get_water_foam_intensity() const { return m_render_settings.getWaterFoamIntensity(); }
    void  set_water_wave_amplitude(float v);
    float get_water_wave_amplitude() const { return m_render_settings.getWaterWaveAmplitude(); }
    void  set_water_normal_maps(const String& nm1_path, const String& nm2_path);
    void  set_water_shallow_color(const Color& c);
    void  set_water_deep_color(const Color& c);
    void  set_water_sun_intensity(float v);

    // === Wave Mode (Gerstner / FFT) ===
    void  set_wave_mode(int mode);
    int   get_wave_mode() const;
    void  set_fft_wind_speed(float s);
    float get_fft_wind_speed() const;
    void  set_fft_wind_direction(const Vector2& dir);
    void  set_fft_patch_size(float s);
    float get_fft_patch_size() const;
    void  set_fft_choppiness(float c);
    float get_fft_choppiness() const;
    void  set_fft_normal_strength(float v);

    // === Caustics ===
    void  set_caustics_enabled(bool v);
    bool  get_caustics_enabled() const;
    void  set_caustics_intensity(float v);
    float get_caustics_intensity() const;
    void  set_caustics_texture(const String& path);

    // === Water Ripples ===
    void  set_water_ripples_enabled(bool v);
    bool  get_water_ripples_enabled() const;
    void  add_water_ripple(const Vector3& world_pos, float strength);
    void  clear_water_ripples();

    // === Enhanced Water ===
    void  set_water_sss_strength(float v);
    float get_water_sss_strength() const;
    void  set_water_foam_texture(const String& path);

    // === Underwater ===
    void  set_underwater_enabled(bool v);
    bool  get_underwater_enabled() const;
    void  set_underwater_fog_color(const Color& c);
    void  set_underwater_fog_density(float v);
    float get_underwater_fog_density() const;
    void  set_underwater_chrom_strength(float v);
    float get_underwater_chrom_strength() const;
    void  set_underwater_distort_frequency(float v);
    void  set_underwater_distort_speed(float v);

    // === Ripple Tuning ===
    void  set_water_ripple_damping(float v);
    float get_water_ripple_damping() const;
    void  set_water_ripple_speed(float v);
    float get_water_ripple_speed() const;

    // === Caustics Scale ===
    void  set_caustics_scale(float v);
    float get_caustics_scale() const;

    // === Water Grid LOD ===
    void  set_water_grid_resolution(int n);

    // === Decals ===
    void  set_decals_enabled(bool v);
    bool  get_decals_enabled() const       { return m_render_settings.getDecalsEnabled(); }
    // Returns decal handle (0 = failure)
    int   add_decal(const Vector3& pos, const Vector3& normal, const Vector3& size,
                    const String& albedo_path, float opacity = 0.9f,
                    const Color& tint = Color(1, 1, 1, 1));
    void  remove_decal(int handle);
    void  clear_decals();

    // === Foliage ===
    void  set_foliage_enabled(bool v);
    bool  get_foliage_enabled() const         { return m_render_settings.getFoliageEnabled(); }
    void  set_foliage_cull_distance(float v);
    float get_foliage_cull_distance() const   { return m_render_settings.getFoliageCullDistance(); }
    void  set_grass_texture(const String& path);
    void  add_foliage_cluster(const Dictionary& params);
    void  clear_foliage();

    // === Physics Controls ===
    void play_physics();
    void pause_physics();
    void step_physics();
    void stop_physics();
    void set_physics_speed(float speed);
    float get_physics_speed() const { return m_physics.getSpeed(); }
    bool is_physics_playing() const { return m_physics.isPlaying(); }
    void set_gravity(Vector3 gravity);
    Vector3 get_gravity() const;

    // === Raycasting ===
    Dictionary cast_ray(const Vector3& origin, const Vector3& direction, float max_distance, int layer_mask = 0xFFFF);
    Array cast_ray_all(const Vector3& origin, const Vector3& direction, float max_distance, int layer_mask = 0xFFFF);
    Array overlap_sphere(const Vector3& center, float radius, int layer_mask = 0xFFFF);
    Array overlap_box(const Vector3& center, const Vector3& half_extents, const Vector3& rotation_deg, int layer_mask = 0xFFFF);

    // === Collision Layers ===
    void set_layer_collision(int layer1, int layer2, bool should_collide);

    // === Constraints ===
    int create_constraint_fixed(int body_handle1, int body_handle2, const Vector3& anchor);
    int create_constraint_hinge(int body_handle1, int body_handle2, const Vector3& anchor, const Vector3& axis,
                                 float limit_min = 0.0f, float limit_max = 0.0f);
    int create_constraint_slider(int body_handle1, int body_handle2, const Vector3& axis,
                                  float limit_min = 0.0f, float limit_max = 0.0f);
    int create_constraint_point(int body_handle1, int body_handle2, const Vector3& anchor1, const Vector3& anchor2);
    int create_constraint_distance(int body_handle1, int body_handle2, const Vector3& anchor1, const Vector3& anchor2,
                                    float min_dist = -1.0f, float max_dist = -1.0f);
    int create_constraint_cone(int body_handle1, int body_handle2, const Vector3& anchor, const Vector3& twist_axis,
                                float half_cone_angle = 0.5f);
    void destroy_constraint(int constraint_handle);
    void set_constraint_enabled(int constraint_handle, bool enabled);
    void set_constraint_motor(int constraint_handle, bool enabled, float speed, float max_force);
    void set_constraint_limits(int constraint_handle, float min_val, float max_val);
    void set_constraint_breaking(int constraint_handle, float max_force, float max_torque);
    Array get_and_clear_broken_constraints();

    // === Physics Grab/Throw ===
    int grab_body(int body_handle, const Vector3& world_pos);
    void move_grab(int token, const Vector3& world_pos);
    void release_grab(int token);
    void throw_grab(int token, const Vector3& velocity);

    // === Character Controller ===
    int create_character(const Vector3& position, float capsule_radius, float capsule_height,
                          float max_slope_deg = 50.0f, float mass = 80.0f);
    void destroy_character(int char_handle);
    Dictionary get_character_state(int char_handle);
    void set_character_position(int char_handle, const Vector3& position);
    void set_character_rotation(int char_handle, const Vector3& rotation_deg);
    void set_character_velocity(int char_handle, const Vector3& velocity);
    void update_character(int char_handle, float delta, const Vector3& gravity, const Vector3& movement_input);

    // === Wireframe Mode ===
    void set_wireframe_enabled(bool enabled);
    bool get_wireframe_enabled() const { return m_wireframe_enabled; }

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

    // === Actor Transform API ===
    void set_actor_position(const String& actor_name, const Vector3& position);
    void set_actor_rotation(const String& actor_name, const Vector3& rotation_deg);
    void set_actor_scale(const String& actor_name, const Vector3& scale);

    // === Actor Lifecycle ===
    void remove_actor(const String& actor_name);
    bool has_actor(const String& actor_name) const;

    // === Actor Physics API ===
    int get_actor_body_handle(const String& actor_name);
    void set_actor_body_type(const String& actor_name, int type);
    void set_actor_mass(const String& actor_name, float mass);
    void set_actor_restitution(const String& actor_name, float restitution);
    void set_actor_friction(const String& actor_name, float friction);
    void set_actor_gravity_enabled(const String& actor_name, bool enabled);
    void set_actor_gravity_scale(const String& actor_name, float scale);
    void set_actor_linear_damping(const String& actor_name, float damping);
    void set_actor_angular_damping(const String& actor_name, float damping);
    void apply_radial_impulse(const Vector3& center, float strength, float radius, int falloff);
    void set_actor_linear_velocity(const String& actor_name, const Vector3& velocity);
    void sync_actor_physics_shape(const String& actor_name);

    // === World Force Effects ===
    void set_wind(const Vector3& direction, float strength, float turbulence);
    void clear_wind();
    void set_water(float liquid_level, float density, float drag);
    void clear_water();
    int create_force_volume_box(const Vector3& center, const Vector3& half_extents, const Vector3& force);
    int create_force_volume_sphere(const Vector3& center, float radius, const Vector3& force);
    void destroy_force_volume(int handle);
    void set_force_volume_enabled(int handle, bool enabled);

    // === Apply Forces by Actor Name ===
    void apply_force_on_actor(const String& actor_name, const Vector3& force, const Vector3& rel_pos);
    void apply_impulse_on_actor(const String& actor_name, const Vector3& impulse, const Vector3& rel_pos);
    void apply_torque_on_actor(const String& actor_name, const Vector3& torque);

    // === Velocity / Mass Getters ===
    Vector3 get_actor_linear_velocity(const String& actor_name);
    Vector3 get_actor_angular_velocity(const String& actor_name);
    float   get_actor_mass(const String& actor_name);

    // === CCD ===
    void set_actor_ccd_enabled(const String& actor_name, bool enabled);

    // === Axis Lock ===
    void set_actor_freeze_linear(const String& actor_name, bool x, bool y, bool z);
    void set_actor_freeze_rotation(const String& actor_name, bool x, bool y, bool z);

    // === Sleep / Wake ===
    void set_actor_awake(const String& actor_name, bool awake);
    bool is_actor_awake(const String& actor_name);

    // === Collision Layer Assignment ===
    void set_actor_layer(const String& actor_name, int layer);

    // === Springs ===
    int  create_spring(const String& body1, const String& body2, float stiffness, float rest_length, float damping);
    int  create_anchor_spring(const String& body_name, const Vector3& anchor, float stiffness, float rest_length, float damping);
    void destroy_spring(int handle);
    void set_spring_enabled(int handle, bool enabled);

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
