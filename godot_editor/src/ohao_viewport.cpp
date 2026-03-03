#include "ohao_viewport.h"
#include "ohao_physics_body.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/font.hpp>

// Include OHAO headers
#include "physics/world/physics_world.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "engine/scene/scene.hpp"
#include "renderer/gizmo/gizmo_meshes.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <iostream>
#include <vector>

namespace godot {

// ===== GDScript Bindings =====

void OhaoViewport::_bind_methods() {
    // === Collision Signals ===
    ADD_SIGNAL(MethodInfo("body_entered",
        PropertyInfo(Variant::STRING, "body1_name"),
        PropertyInfo(Variant::STRING, "body2_name"),
        PropertyInfo(Variant::VECTOR3, "contact_point"),
        PropertyInfo(Variant::FLOAT,   "impulse")));
    ADD_SIGNAL(MethodInfo("body_exited",
        PropertyInfo(Variant::STRING, "body1_name"),
        PropertyInfo(Variant::STRING, "body2_name")));
    ADD_SIGNAL(MethodInfo("lightning_struck"));

    // === Core Methods ===
    ClassDB::bind_method(D_METHOD("initialize_renderer"), &OhaoViewport::initialize_renderer);
    ClassDB::bind_method(D_METHOD("shutdown_renderer"), &OhaoViewport::shutdown_renderer);
    ClassDB::bind_method(D_METHOD("is_renderer_initialized"), &OhaoViewport::is_renderer_initialized);
    ClassDB::bind_method(D_METHOD("load_tscn", "path"), &OhaoViewport::load_tscn);
    ClassDB::bind_method(D_METHOD("sync_scene"), &OhaoViewport::sync_scene);
    ClassDB::bind_method(D_METHOD("clear_scene"), &OhaoViewport::clear_scene);
    ClassDB::bind_method(D_METHOD("sync_from_godot", "root_node"), &OhaoViewport::sync_from_godot);
    ClassDB::bind_method(D_METHOD("get_synced_object_count"), &OhaoViewport::get_synced_object_count);
    ClassDB::bind_method(D_METHOD("add_cube", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_cube);
    ClassDB::bind_method(D_METHOD("add_sphere", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_sphere);
    ClassDB::bind_method(D_METHOD("add_plane", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_plane);
    ClassDB::bind_method(D_METHOD("add_cylinder", "name", "position", "rotation", "scale", "color"), &OhaoViewport::add_cylinder);
    ClassDB::bind_method(D_METHOD("add_directional_light", "name", "position", "direction", "color", "intensity"), &OhaoViewport::add_directional_light);
    ClassDB::bind_method(D_METHOD("add_point_light", "name", "position", "color", "intensity", "range"), &OhaoViewport::add_point_light);
    ClassDB::bind_method(D_METHOD("finish_sync"), &OhaoViewport::finish_sync);
    ClassDB::bind_method(D_METHOD("set_viewport_size", "width", "height"), &OhaoViewport::set_viewport_size);
    ClassDB::bind_method(D_METHOD("get_viewport_size"), &OhaoViewport::get_viewport_size);
    ClassDB::bind_method(D_METHOD("get_render_stats"), &OhaoViewport::get_render_stats);

    // === Base Properties ===
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

    // === AAA Render Mode ===
    ClassDB::bind_method(D_METHOD("set_render_mode", "mode"), &OhaoViewport::set_render_mode);
    ClassDB::bind_method(D_METHOD("get_render_mode"), &OhaoViewport::get_render_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "render_mode", PROPERTY_HINT_ENUM, "Forward,Deferred"), "set_render_mode", "get_render_mode");

    // === Post-Processing Toggles ===
    ADD_GROUP("Post Processing", "");

    ClassDB::bind_method(D_METHOD("set_bloom_enabled", "enabled"), &OhaoViewport::set_bloom_enabled);
    ClassDB::bind_method(D_METHOD("get_bloom_enabled"), &OhaoViewport::get_bloom_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bloom_enabled"), "set_bloom_enabled", "get_bloom_enabled");

    ClassDB::bind_method(D_METHOD("set_taa_enabled", "enabled"), &OhaoViewport::set_taa_enabled);
    ClassDB::bind_method(D_METHOD("get_taa_enabled"), &OhaoViewport::get_taa_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "taa_enabled"), "set_taa_enabled", "get_taa_enabled");

    ClassDB::bind_method(D_METHOD("set_ssao_enabled", "enabled"), &OhaoViewport::set_ssao_enabled);
    ClassDB::bind_method(D_METHOD("get_ssao_enabled"), &OhaoViewport::get_ssao_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssao_enabled"), "set_ssao_enabled", "get_ssao_enabled");

    ClassDB::bind_method(D_METHOD("set_ssgi_enabled", "enabled"), &OhaoViewport::set_ssgi_enabled);
    ClassDB::bind_method(D_METHOD("get_ssgi_enabled"), &OhaoViewport::get_ssgi_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssgi_enabled"), "set_ssgi_enabled", "get_ssgi_enabled");

    ClassDB::bind_method(D_METHOD("set_ssr_enabled", "enabled"), &OhaoViewport::set_ssr_enabled);
    ClassDB::bind_method(D_METHOD("get_ssr_enabled"), &OhaoViewport::get_ssr_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssr_enabled"), "set_ssr_enabled", "get_ssr_enabled");

    ClassDB::bind_method(D_METHOD("set_volumetrics_enabled", "enabled"), &OhaoViewport::set_volumetrics_enabled);
    ClassDB::bind_method(D_METHOD("get_volumetrics_enabled"), &OhaoViewport::get_volumetrics_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "volumetrics_enabled"), "set_volumetrics_enabled", "get_volumetrics_enabled");

    ClassDB::bind_method(D_METHOD("set_motion_blur_enabled", "enabled"), &OhaoViewport::set_motion_blur_enabled);
    ClassDB::bind_method(D_METHOD("get_motion_blur_enabled"), &OhaoViewport::get_motion_blur_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "motion_blur_enabled"), "set_motion_blur_enabled", "get_motion_blur_enabled");

    ClassDB::bind_method(D_METHOD("set_dof_enabled", "enabled"), &OhaoViewport::set_dof_enabled);
    ClassDB::bind_method(D_METHOD("get_dof_enabled"), &OhaoViewport::get_dof_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dof_enabled"), "set_dof_enabled", "get_dof_enabled");

    ClassDB::bind_method(D_METHOD("set_tonemapping_enabled", "enabled"), &OhaoViewport::set_tonemapping_enabled);
    ClassDB::bind_method(D_METHOD("get_tonemapping_enabled"), &OhaoViewport::get_tonemapping_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tonemapping_enabled"), "set_tonemapping_enabled", "get_tonemapping_enabled");

    // === Tonemapping Settings ===
    ADD_GROUP("Tonemapping", "tonemap_");

    ClassDB::bind_method(D_METHOD("set_tonemap_operator", "op"), &OhaoViewport::set_tonemap_operator);
    ClassDB::bind_method(D_METHOD("get_tonemap_operator"), &OhaoViewport::get_tonemap_operator);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "tonemap_operator", PROPERTY_HINT_ENUM, "ACES,Reinhard,Uncharted2,Neutral"), "set_tonemap_operator", "get_tonemap_operator");

    ClassDB::bind_method(D_METHOD("set_exposure", "exposure"), &OhaoViewport::set_exposure);
    ClassDB::bind_method(D_METHOD("get_exposure"), &OhaoViewport::get_exposure);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonemap_exposure", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_exposure", "get_exposure");

    ClassDB::bind_method(D_METHOD("set_gamma", "gamma"), &OhaoViewport::set_gamma);
    ClassDB::bind_method(D_METHOD("get_gamma"), &OhaoViewport::get_gamma);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonemap_gamma", PROPERTY_HINT_RANGE, "1.0,3.0,0.1"), "set_gamma", "get_gamma");

    // === Bloom Settings ===
    ADD_GROUP("Bloom", "bloom_");

    ClassDB::bind_method(D_METHOD("set_bloom_threshold", "threshold"), &OhaoViewport::set_bloom_threshold);
    ClassDB::bind_method(D_METHOD("get_bloom_threshold"), &OhaoViewport::get_bloom_threshold);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bloom_threshold", PROPERTY_HINT_RANGE, "0.0,5.0,0.1"), "set_bloom_threshold", "get_bloom_threshold");

    ClassDB::bind_method(D_METHOD("set_bloom_intensity", "intensity"), &OhaoViewport::set_bloom_intensity);
    ClassDB::bind_method(D_METHOD("get_bloom_intensity"), &OhaoViewport::get_bloom_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bloom_intensity", PROPERTY_HINT_RANGE, "0.0,2.0,0.1"), "set_bloom_intensity", "get_bloom_intensity");

    // === SSAO Settings ===
    ADD_GROUP("SSAO", "ssao_");

    ClassDB::bind_method(D_METHOD("set_ssao_radius", "radius"), &OhaoViewport::set_ssao_radius);
    ClassDB::bind_method(D_METHOD("get_ssao_radius"), &OhaoViewport::get_ssao_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssao_radius", PROPERTY_HINT_RANGE, "0.1,2.0,0.05"), "set_ssao_radius", "get_ssao_radius");

    ClassDB::bind_method(D_METHOD("set_ssao_intensity", "intensity"), &OhaoViewport::set_ssao_intensity);
    ClassDB::bind_method(D_METHOD("get_ssao_intensity"), &OhaoViewport::get_ssao_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssao_intensity", PROPERTY_HINT_RANGE, "0.0,3.0,0.1"), "set_ssao_intensity", "get_ssao_intensity");

    // === SSGI Settings ===
    ADD_GROUP("SSGI", "ssgi_");

    ClassDB::bind_method(D_METHOD("set_ssgi_radius", "radius"), &OhaoViewport::set_ssgi_radius);
    ClassDB::bind_method(D_METHOD("get_ssgi_radius"), &OhaoViewport::get_ssgi_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssgi_radius", PROPERTY_HINT_RANGE, "0.5,10.0,0.5"), "set_ssgi_radius", "get_ssgi_radius");

    ClassDB::bind_method(D_METHOD("set_ssgi_intensity", "intensity"), &OhaoViewport::set_ssgi_intensity);
    ClassDB::bind_method(D_METHOD("get_ssgi_intensity"), &OhaoViewport::get_ssgi_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssgi_intensity", PROPERTY_HINT_RANGE, "0.0,3.0,0.1"), "set_ssgi_intensity", "get_ssgi_intensity");

    ClassDB::bind_method(D_METHOD("set_ssgi_sample_count", "count"), &OhaoViewport::set_ssgi_sample_count);
    ClassDB::bind_method(D_METHOD("get_ssgi_sample_count"), &OhaoViewport::get_ssgi_sample_count);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "ssgi_sample_count", PROPERTY_HINT_RANGE, "1,16,1"), "set_ssgi_sample_count", "get_ssgi_sample_count");

    // === SSR Settings ===
    ADD_GROUP("SSR", "ssr_");

    ClassDB::bind_method(D_METHOD("set_ssr_max_distance", "distance"), &OhaoViewport::set_ssr_max_distance);
    ClassDB::bind_method(D_METHOD("get_ssr_max_distance"), &OhaoViewport::get_ssr_max_distance);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssr_max_distance", PROPERTY_HINT_RANGE, "10.0,500.0,10.0"), "set_ssr_max_distance", "get_ssr_max_distance");

    ClassDB::bind_method(D_METHOD("set_ssr_thickness", "thickness"), &OhaoViewport::set_ssr_thickness);
    ClassDB::bind_method(D_METHOD("get_ssr_thickness"), &OhaoViewport::get_ssr_thickness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssr_thickness", PROPERTY_HINT_RANGE, "0.1,2.0,0.1"), "set_ssr_thickness", "get_ssr_thickness");

    // === Volumetric Settings ===
    ADD_GROUP("Volumetrics", "volumetric_");

    ClassDB::bind_method(D_METHOD("set_volumetric_density", "density"), &OhaoViewport::set_volumetric_density);
    ClassDB::bind_method(D_METHOD("get_volumetric_density"), &OhaoViewport::get_volumetric_density);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volumetric_density", PROPERTY_HINT_RANGE, "0.0,0.2,0.005"), "set_volumetric_density", "get_volumetric_density");

    ClassDB::bind_method(D_METHOD("set_volumetric_scattering", "g"), &OhaoViewport::set_volumetric_scattering);
    ClassDB::bind_method(D_METHOD("get_volumetric_scattering"), &OhaoViewport::get_volumetric_scattering);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volumetric_scattering", PROPERTY_HINT_RANGE, "0.0,1.0,0.05"), "set_volumetric_scattering", "get_volumetric_scattering");

    ClassDB::bind_method(D_METHOD("set_fog_color", "color"), &OhaoViewport::set_fog_color);
    ClassDB::bind_method(D_METHOD("get_fog_color"), &OhaoViewport::get_fog_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "volumetric_fog_color"), "set_fog_color", "get_fog_color");

    // === Motion Blur Settings ===
    ADD_GROUP("Motion Blur", "motion_blur_");

    ClassDB::bind_method(D_METHOD("set_motion_blur_intensity", "intensity"), &OhaoViewport::set_motion_blur_intensity);
    ClassDB::bind_method(D_METHOD("get_motion_blur_intensity"), &OhaoViewport::get_motion_blur_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "motion_blur_intensity", PROPERTY_HINT_RANGE, "0.0,2.0,0.1"), "set_motion_blur_intensity", "get_motion_blur_intensity");

    ClassDB::bind_method(D_METHOD("set_motion_blur_samples", "samples"), &OhaoViewport::set_motion_blur_samples);
    ClassDB::bind_method(D_METHOD("get_motion_blur_samples"), &OhaoViewport::get_motion_blur_samples);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "motion_blur_samples", PROPERTY_HINT_RANGE, "4,32,1"), "set_motion_blur_samples", "get_motion_blur_samples");

    // === DoF Settings ===
    ADD_GROUP("Depth of Field", "dof_");

    ClassDB::bind_method(D_METHOD("set_dof_focus_distance", "distance"), &OhaoViewport::set_dof_focus_distance);
    ClassDB::bind_method(D_METHOD("get_dof_focus_distance"), &OhaoViewport::get_dof_focus_distance);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dof_focus_distance", PROPERTY_HINT_RANGE, "0.1,100.0,0.5"), "set_dof_focus_distance", "get_dof_focus_distance");

    ClassDB::bind_method(D_METHOD("set_dof_aperture", "aperture"), &OhaoViewport::set_dof_aperture);
    ClassDB::bind_method(D_METHOD("get_dof_aperture"), &OhaoViewport::get_dof_aperture);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dof_aperture", PROPERTY_HINT_RANGE, "1.0,22.0,0.5"), "set_dof_aperture", "get_dof_aperture");

    ClassDB::bind_method(D_METHOD("set_dof_max_blur", "blur"), &OhaoViewport::set_dof_max_blur);
    ClassDB::bind_method(D_METHOD("get_dof_max_blur"), &OhaoViewport::get_dof_max_blur);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dof_max_blur", PROPERTY_HINT_RANGE, "1.0,20.0,1.0"), "set_dof_max_blur", "get_dof_max_blur");

    // === TAA Settings ===
    ADD_GROUP("TAA", "taa_");

    ClassDB::bind_method(D_METHOD("set_taa_blend_factor", "factor"), &OhaoViewport::set_taa_blend_factor);
    ClassDB::bind_method(D_METHOD("get_taa_blend_factor"), &OhaoViewport::get_taa_blend_factor);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "taa_blend_factor", PROPERTY_HINT_RANGE, "0.01,0.5,0.01"), "set_taa_blend_factor", "get_taa_blend_factor");

    // === Sky Settings ===
    ADD_GROUP("Sky", "sky_");

    ClassDB::bind_method(D_METHOD("set_sky_enabled", "enabled"), &OhaoViewport::set_sky_enabled);
    ClassDB::bind_method(D_METHOD("get_sky_enabled"), &OhaoViewport::get_sky_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sky_enabled"), "set_sky_enabled", "get_sky_enabled");

    ClassDB::bind_method(D_METHOD("set_sun_direction", "dir"), &OhaoViewport::set_sun_direction);
    ClassDB::bind_method(D_METHOD("get_sun_direction"), &OhaoViewport::get_sun_direction);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sun_direction"), "set_sun_direction", "get_sun_direction");

    ClassDB::bind_method(D_METHOD("set_sky_turbidity", "turbidity"), &OhaoViewport::set_sky_turbidity);
    ClassDB::bind_method(D_METHOD("get_sky_turbidity"), &OhaoViewport::get_sky_turbidity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_turbidity", PROPERTY_HINT_RANGE, "1.0,10.0,0.1"), "set_sky_turbidity", "get_sky_turbidity");

    ClassDB::bind_method(D_METHOD("set_sky_intensity", "intensity"), &OhaoViewport::set_sky_intensity);
    ClassDB::bind_method(D_METHOD("get_sky_intensity"), &OhaoViewport::get_sky_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_intensity", PROPERTY_HINT_RANGE, "0.0,5.0,0.1"), "set_sky_intensity", "get_sky_intensity");

    // === Time of Day ===
    ClassDB::bind_method(D_METHOD("set_time_of_day", "hours"), &OhaoViewport::set_time_of_day);
    ClassDB::bind_method(D_METHOD("get_time_of_day"), &OhaoViewport::get_time_of_day);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_of_day", PROPERTY_HINT_RANGE, "0.0,24.0,0.01"), "set_time_of_day", "get_time_of_day");

    // === Rain Settings ===
    ADD_GROUP("Rain", "rain_");

    ClassDB::bind_method(D_METHOD("set_rain_enabled", "enabled"), &OhaoViewport::set_rain_enabled);
    ClassDB::bind_method(D_METHOD("get_rain_enabled"), &OhaoViewport::get_rain_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rain_enabled"), "set_rain_enabled", "get_rain_enabled");

    ClassDB::bind_method(D_METHOD("set_rain_intensity", "intensity"), &OhaoViewport::set_rain_intensity);
    ClassDB::bind_method(D_METHOD("get_rain_intensity"), &OhaoViewport::get_rain_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_intensity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_rain_intensity", "get_rain_intensity");

    ClassDB::bind_method(D_METHOD("set_rain_wind_x", "wind_x"), &OhaoViewport::set_rain_wind_x);
    ClassDB::bind_method(D_METHOD("get_rain_wind_x"), &OhaoViewport::get_rain_wind_x);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_wind_x", PROPERTY_HINT_RANGE, "-1.0,1.0,0.01"), "set_rain_wind_x", "get_rain_wind_x");

    ClassDB::bind_method(D_METHOD("set_wetness_rate", "rate"), &OhaoViewport::set_wetness_rate);
    ClassDB::bind_method(D_METHOD("get_wetness_rate"), &OhaoViewport::get_wetness_rate);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_rate", PROPERTY_HINT_RANGE, "0.0,10.0,0.001"), "set_wetness_rate", "get_wetness_rate");

    ClassDB::bind_method(D_METHOD("set_drying_rate", "rate"), &OhaoViewport::set_drying_rate);
    ClassDB::bind_method(D_METHOD("get_drying_rate"), &OhaoViewport::get_drying_rate);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drying_rate", PROPERTY_HINT_RANGE, "0.0,10.0,0.001"), "set_drying_rate", "get_drying_rate");

    ClassDB::bind_method(D_METHOD("get_surface_wetness"), &OhaoViewport::get_surface_wetness);

    // === Lightning Settings ===
    ADD_GROUP("Lightning", "lightning_");

    ClassDB::bind_method(D_METHOD("set_lightning_enabled", "enabled"), &OhaoViewport::set_lightning_enabled);
    ClassDB::bind_method(D_METHOD("get_lightning_enabled"), &OhaoViewport::get_lightning_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lightning_enabled"), "set_lightning_enabled", "get_lightning_enabled");

    ClassDB::bind_method(D_METHOD("set_lightning_interval", "seconds"), &OhaoViewport::set_lightning_interval);
    ClassDB::bind_method(D_METHOD("get_lightning_interval"), &OhaoViewport::get_lightning_interval);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lightning_interval", PROPERTY_HINT_RANGE, "0.5,60.0,0.5"), "set_lightning_interval", "get_lightning_interval");

    ClassDB::bind_method(D_METHOD("set_lightning_brightness", "brightness"), &OhaoViewport::set_lightning_brightness);
    ClassDB::bind_method(D_METHOD("get_lightning_brightness"), &OhaoViewport::get_lightning_brightness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lightning_brightness", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_lightning_brightness", "get_lightning_brightness");

    ClassDB::bind_method(D_METHOD("trigger_lightning"), &OhaoViewport::trigger_lightning);

    // === Snow Settings ===
    ADD_GROUP("Snow", "snow_");

    ClassDB::bind_method(D_METHOD("set_snow_enabled", "enabled"), &OhaoViewport::set_snow_enabled);
    ClassDB::bind_method(D_METHOD("get_snow_enabled"), &OhaoViewport::get_snow_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snow_enabled"), "set_snow_enabled", "get_snow_enabled");

    ClassDB::bind_method(D_METHOD("set_snow_intensity", "intensity"), &OhaoViewport::set_snow_intensity);
    ClassDB::bind_method(D_METHOD("get_snow_intensity"), &OhaoViewport::get_snow_intensity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_intensity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_snow_intensity", "get_snow_intensity");

    ClassDB::bind_method(D_METHOD("set_snow_wind_x", "wind_x"), &OhaoViewport::set_snow_wind_x);
    ClassDB::bind_method(D_METHOD("get_snow_wind_x"), &OhaoViewport::get_snow_wind_x);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_wind_x", PROPERTY_HINT_RANGE, "-1.0,1.0,0.01"), "set_snow_wind_x", "get_snow_wind_x");

    ClassDB::bind_method(D_METHOD("set_snow_accum_rate", "rate"), &OhaoViewport::set_snow_accum_rate);
    ClassDB::bind_method(D_METHOD("get_snow_accum_rate"), &OhaoViewport::get_snow_accum_rate);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_accum_rate", PROPERTY_HINT_RANGE, "0.0,10.0,0.001"), "set_snow_accum_rate", "get_snow_accum_rate");

    ClassDB::bind_method(D_METHOD("set_snow_melt_rate", "rate"), &OhaoViewport::set_snow_melt_rate);
    ClassDB::bind_method(D_METHOD("get_snow_melt_rate"), &OhaoViewport::get_snow_melt_rate);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_melt_rate", PROPERTY_HINT_RANGE, "0.0,10.0,0.001"), "set_snow_melt_rate", "get_snow_melt_rate");

    ClassDB::bind_method(D_METHOD("get_snow_accumulation"), &OhaoViewport::get_snow_accumulation);

    // === Cloud Settings ===
    ADD_GROUP("Clouds", "cloud_");

    ClassDB::bind_method(D_METHOD("set_cloud_enabled", "enabled"), &OhaoViewport::set_cloud_enabled);
    ClassDB::bind_method(D_METHOD("get_cloud_enabled"), &OhaoViewport::get_cloud_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cloud_enabled"), "set_cloud_enabled", "get_cloud_enabled");

    ClassDB::bind_method(D_METHOD("set_cloud_coverage", "coverage"), &OhaoViewport::set_cloud_coverage);
    ClassDB::bind_method(D_METHOD("get_cloud_coverage"), &OhaoViewport::get_cloud_coverage);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_coverage", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_cloud_coverage", "get_cloud_coverage");

    ClassDB::bind_method(D_METHOD("set_cloud_density", "density"), &OhaoViewport::set_cloud_density);
    ClassDB::bind_method(D_METHOD("get_cloud_density"), &OhaoViewport::get_cloud_density);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_density", PROPERTY_HINT_RANGE, "0.0,5.0,0.05"), "set_cloud_density", "get_cloud_density");

    ClassDB::bind_method(D_METHOD("set_cloud_altitude_min", "alt"), &OhaoViewport::set_cloud_altitude_min);
    ClassDB::bind_method(D_METHOD("get_cloud_altitude_min"), &OhaoViewport::get_cloud_altitude_min);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_altitude_min", PROPERTY_HINT_RANGE, "100.0,5000.0,10.0"), "set_cloud_altitude_min", "get_cloud_altitude_min");

    ClassDB::bind_method(D_METHOD("set_cloud_altitude_max", "alt"), &OhaoViewport::set_cloud_altitude_max);
    ClassDB::bind_method(D_METHOD("get_cloud_altitude_max"), &OhaoViewport::get_cloud_altitude_max);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_altitude_max", PROPERTY_HINT_RANGE, "1000.0,15000.0,10.0"), "set_cloud_altitude_max", "get_cloud_altitude_max");

    ClassDB::bind_method(D_METHOD("set_cloud_speed", "speed"), &OhaoViewport::set_cloud_speed);
    ClassDB::bind_method(D_METHOD("get_cloud_speed"), &OhaoViewport::get_cloud_speed);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_speed", PROPERTY_HINT_RANGE, "0.0,10.0,0.1"), "set_cloud_speed", "get_cloud_speed");

    // === Sand (Sandstorm) ===
    ClassDB::bind_method(D_METHOD("set_sand_enabled", "enabled"), &OhaoViewport::set_sand_enabled);
    ClassDB::bind_method(D_METHOD("get_sand_enabled"), &OhaoViewport::get_sand_enabled);
    ClassDB::bind_method(D_METHOD("set_sand_intensity", "intensity"), &OhaoViewport::set_sand_intensity);
    ClassDB::bind_method(D_METHOD("get_sand_intensity"), &OhaoViewport::get_sand_intensity);
    ClassDB::bind_method(D_METHOD("set_sand_wind_x", "wind_x"), &OhaoViewport::set_sand_wind_x);
    ClassDB::bind_method(D_METHOD("get_sand_wind_x"), &OhaoViewport::get_sand_wind_x);

    // === Frost / Ice ===
    ClassDB::bind_method(D_METHOD("set_frost_accum_rate", "rate"), &OhaoViewport::set_frost_accum_rate);
    ClassDB::bind_method(D_METHOD("get_frost_accum_rate"), &OhaoViewport::get_frost_accum_rate);
    ClassDB::bind_method(D_METHOD("set_frost_melt_rate", "rate"), &OhaoViewport::set_frost_melt_rate);
    ClassDB::bind_method(D_METHOD("get_frost_melt_rate"), &OhaoViewport::get_frost_melt_rate);
    ClassDB::bind_method(D_METHOD("get_frost_cover"), &OhaoViewport::get_frost_cover);

    // === God Rays ===
    ClassDB::bind_method(D_METHOD("set_god_rays_enabled", "enabled"), &OhaoViewport::set_god_rays_enabled);
    ClassDB::bind_method(D_METHOD("get_god_rays_enabled"), &OhaoViewport::get_god_rays_enabled);
    ClassDB::bind_method(D_METHOD("set_god_rays_intensity", "intensity"), &OhaoViewport::set_god_rays_intensity);
    ClassDB::bind_method(D_METHOD("get_god_rays_intensity"), &OhaoViewport::get_god_rays_intensity);

    // === Aurora Borealis ===
    ClassDB::bind_method(D_METHOD("set_aurora_enabled", "enabled"), &OhaoViewport::set_aurora_enabled);
    ClassDB::bind_method(D_METHOD("get_aurora_enabled"), &OhaoViewport::get_aurora_enabled);
    ClassDB::bind_method(D_METHOD("set_aurora_intensity", "intensity"), &OhaoViewport::set_aurora_intensity);
    ClassDB::bind_method(D_METHOD("get_aurora_intensity"), &OhaoViewport::get_aurora_intensity);
    ClassDB::bind_method(D_METHOD("set_aurora_hue", "hue"), &OhaoViewport::set_aurora_hue);
    ClassDB::bind_method(D_METHOD("get_aurora_hue"), &OhaoViewport::get_aurora_hue);

    // === Rainbow ===
    ClassDB::bind_method(D_METHOD("set_rainbow_enabled", "enabled"), &OhaoViewport::set_rainbow_enabled);
    ClassDB::bind_method(D_METHOD("get_rainbow_enabled"), &OhaoViewport::get_rainbow_enabled);

    // === Heat Haze ===
    ClassDB::bind_method(D_METHOD("set_heat_haze_enabled", "enabled"), &OhaoViewport::set_heat_haze_enabled);
    ClassDB::bind_method(D_METHOD("get_heat_haze_enabled"), &OhaoViewport::get_heat_haze_enabled);
    ClassDB::bind_method(D_METHOD("set_heat_haze_intensity", "intensity"), &OhaoViewport::set_heat_haze_intensity);
    ClassDB::bind_method(D_METHOD("get_heat_haze_intensity"), &OhaoViewport::get_heat_haze_intensity);
    ClassDB::bind_method(D_METHOD("set_heat_haze_frequency", "frequency"), &OhaoViewport::set_heat_haze_frequency);
    ClassDB::bind_method(D_METHOD("get_heat_haze_frequency"), &OhaoViewport::get_heat_haze_frequency);

    // === Terrain ===
    ClassDB::bind_method(D_METHOD("set_terrain_enabled", "enabled"), &OhaoViewport::set_terrain_enabled);
    ClassDB::bind_method(D_METHOD("get_terrain_enabled"), &OhaoViewport::get_terrain_enabled);
    ClassDB::bind_method(D_METHOD("set_terrain_height_scale", "scale"), &OhaoViewport::set_terrain_height_scale);
    ClassDB::bind_method(D_METHOD("get_terrain_height_scale"), &OhaoViewport::get_terrain_height_scale);
    ClassDB::bind_method(D_METHOD("set_terrain_size", "size"), &OhaoViewport::set_terrain_size);
    ClassDB::bind_method(D_METHOD("get_terrain_size"), &OhaoViewport::get_terrain_size);
    ClassDB::bind_method(D_METHOD("set_terrain_heightmap", "path"), &OhaoViewport::set_terrain_heightmap);
    ClassDB::bind_method(D_METHOD("set_terrain_splat_map", "path"), &OhaoViewport::set_terrain_splat_map);
    ClassDB::bind_method(D_METHOD("set_terrain_layer", "layer_index", "albedo_path", "normal_path"), &OhaoViewport::set_terrain_layer);
    // Procedural generation
    ClassDB::bind_method(D_METHOD("set_terrain_type", "type"), &OhaoViewport::set_terrain_type);
    ClassDB::bind_method(D_METHOD("set_terrain_frequency", "freq"), &OhaoViewport::set_terrain_frequency);
    ClassDB::bind_method(D_METHOD("set_terrain_octaves", "octaves"), &OhaoViewport::set_terrain_octaves);
    ClassDB::bind_method(D_METHOD("set_terrain_offset", "offset"), &OhaoViewport::set_terrain_offset);
    ClassDB::bind_method(D_METHOD("set_terrain_gen_resolution", "resolution"), &OhaoViewport::set_terrain_gen_resolution);
    ClassDB::bind_method(D_METHOD("set_terrain_macro_variation", "path"), &OhaoViewport::set_terrain_macro_variation);
    ClassDB::bind_method(D_METHOD("generate_terrain"), &OhaoViewport::generate_terrain);
    ClassDB::bind_method(D_METHOD("sync_terrain_physics"), &OhaoViewport::sync_terrain_physics);
    // Runtime splatmap painting
    ClassDB::bind_method(D_METHOD("paint_terrain_splat", "world_pos", "channel", "radius", "strength"),
                         &OhaoViewport::paint_terrain_splat);
    ClassDB::bind_method(D_METHOD("clear_terrain_splat"), &OhaoViewport::clear_terrain_splat);
    // Multi-tile terrain streaming
    ClassDB::bind_method(D_METHOD("add_terrain_tile", "offset_x", "offset_z"),
                         &OhaoViewport::add_terrain_tile);
    ClassDB::bind_method(D_METHOD("clear_terrain_tiles"), &OhaoViewport::clear_terrain_tiles);
    ClassDB::bind_method(D_METHOD("set_terrain_tile_cull_radius", "r"),
                         &OhaoViewport::set_terrain_tile_cull_radius);

    // === Water ===
    ClassDB::bind_method(D_METHOD("set_water_enabled", "enabled"), &OhaoViewport::set_water_enabled);
    ClassDB::bind_method(D_METHOD("get_water_enabled"), &OhaoViewport::get_water_enabled);
    ClassDB::bind_method(D_METHOD("set_water_level", "level"), &OhaoViewport::set_water_level);
    ClassDB::bind_method(D_METHOD("get_water_level"), &OhaoViewport::get_water_level);
    ClassDB::bind_method(D_METHOD("set_water_size", "size"), &OhaoViewport::set_water_size);
    ClassDB::bind_method(D_METHOD("get_water_size"), &OhaoViewport::get_water_size);
    ClassDB::bind_method(D_METHOD("set_water_foam_intensity", "intensity"), &OhaoViewport::set_water_foam_intensity);
    ClassDB::bind_method(D_METHOD("get_water_foam_intensity"), &OhaoViewport::get_water_foam_intensity);
    ClassDB::bind_method(D_METHOD("set_water_wave_amplitude", "amplitude"), &OhaoViewport::set_water_wave_amplitude);
    ClassDB::bind_method(D_METHOD("get_water_wave_amplitude"), &OhaoViewport::get_water_wave_amplitude);
    ClassDB::bind_method(D_METHOD("set_water_normal_maps", "nm1_path", "nm2_path"), &OhaoViewport::set_water_normal_maps);
    ClassDB::bind_method(D_METHOD("set_water_shallow_color", "color"), &OhaoViewport::set_water_shallow_color);
    ClassDB::bind_method(D_METHOD("set_water_deep_color", "color"), &OhaoViewport::set_water_deep_color);
    ClassDB::bind_method(D_METHOD("set_water_sun_intensity", "intensity"), &OhaoViewport::set_water_sun_intensity);

    // Wave mode
    ClassDB::bind_method(D_METHOD("set_wave_mode", "mode"), &OhaoViewport::set_wave_mode);
    ClassDB::bind_method(D_METHOD("get_wave_mode"), &OhaoViewport::get_wave_mode);
    ClassDB::bind_method(D_METHOD("set_fft_wind_speed", "speed"), &OhaoViewport::set_fft_wind_speed);
    ClassDB::bind_method(D_METHOD("get_fft_wind_speed"), &OhaoViewport::get_fft_wind_speed);
    ClassDB::bind_method(D_METHOD("set_fft_wind_direction", "dir"), &OhaoViewport::set_fft_wind_direction);
    ClassDB::bind_method(D_METHOD("set_fft_patch_size", "size"), &OhaoViewport::set_fft_patch_size);
    ClassDB::bind_method(D_METHOD("get_fft_patch_size"), &OhaoViewport::get_fft_patch_size);
    ClassDB::bind_method(D_METHOD("set_fft_choppiness", "choppiness"), &OhaoViewport::set_fft_choppiness);
    ClassDB::bind_method(D_METHOD("get_fft_choppiness"), &OhaoViewport::get_fft_choppiness);
    ClassDB::bind_method(D_METHOD("set_fft_normal_strength", "strength"), &OhaoViewport::set_fft_normal_strength);


    // === Caustics ===
    ClassDB::bind_method(D_METHOD("set_caustics_enabled", "enabled"), &OhaoViewport::set_caustics_enabled);
    ClassDB::bind_method(D_METHOD("get_caustics_enabled"), &OhaoViewport::get_caustics_enabled);
    ClassDB::bind_method(D_METHOD("set_caustics_intensity", "intensity"), &OhaoViewport::set_caustics_intensity);
    ClassDB::bind_method(D_METHOD("get_caustics_intensity"), &OhaoViewport::get_caustics_intensity);
    ClassDB::bind_method(D_METHOD("set_caustics_texture", "path"), &OhaoViewport::set_caustics_texture);

    // === Water Ripples ===
    ClassDB::bind_method(D_METHOD("set_water_ripples_enabled", "enabled"), &OhaoViewport::set_water_ripples_enabled);
    ClassDB::bind_method(D_METHOD("get_water_ripples_enabled"), &OhaoViewport::get_water_ripples_enabled);
    ClassDB::bind_method(D_METHOD("add_water_ripple", "world_pos", "strength"), &OhaoViewport::add_water_ripple);
    ClassDB::bind_method(D_METHOD("clear_water_ripples"), &OhaoViewport::clear_water_ripples);

    // === Enhanced Water ===
    ClassDB::bind_method(D_METHOD("set_water_sss_strength", "strength"), &OhaoViewport::set_water_sss_strength);
    ClassDB::bind_method(D_METHOD("get_water_sss_strength"), &OhaoViewport::get_water_sss_strength);
    ClassDB::bind_method(D_METHOD("set_water_foam_texture", "path"), &OhaoViewport::set_water_foam_texture);

    // === Underwater ===
    ClassDB::bind_method(D_METHOD("set_underwater_enabled", "enabled"), &OhaoViewport::set_underwater_enabled);
    ClassDB::bind_method(D_METHOD("get_underwater_enabled"), &OhaoViewport::get_underwater_enabled);
    ClassDB::bind_method(D_METHOD("set_underwater_fog_color", "color"), &OhaoViewport::set_underwater_fog_color);
    ClassDB::bind_method(D_METHOD("set_underwater_fog_density", "density"), &OhaoViewport::set_underwater_fog_density);
    ClassDB::bind_method(D_METHOD("get_underwater_fog_density"), &OhaoViewport::get_underwater_fog_density);
    ClassDB::bind_method(D_METHOD("set_underwater_chrom_strength", "strength"), &OhaoViewport::set_underwater_chrom_strength);
    ClassDB::bind_method(D_METHOD("get_underwater_chrom_strength"), &OhaoViewport::get_underwater_chrom_strength);
    ClassDB::bind_method(D_METHOD("set_underwater_distort_frequency", "v"), &OhaoViewport::set_underwater_distort_frequency);
    ClassDB::bind_method(D_METHOD("set_underwater_distort_speed", "v"), &OhaoViewport::set_underwater_distort_speed);

    // === Ripple Tuning ===
    ClassDB::bind_method(D_METHOD("set_water_ripple_damping", "v"), &OhaoViewport::set_water_ripple_damping);
    ClassDB::bind_method(D_METHOD("get_water_ripple_damping"), &OhaoViewport::get_water_ripple_damping);
    ClassDB::bind_method(D_METHOD("set_water_ripple_speed", "v"), &OhaoViewport::set_water_ripple_speed);
    ClassDB::bind_method(D_METHOD("get_water_ripple_speed"), &OhaoViewport::get_water_ripple_speed);

    // === Caustics Scale ===
    ClassDB::bind_method(D_METHOD("set_caustics_scale", "v"), &OhaoViewport::set_caustics_scale);
    ClassDB::bind_method(D_METHOD("get_caustics_scale"), &OhaoViewport::get_caustics_scale);

    // === Water Grid LOD ===
    ClassDB::bind_method(D_METHOD("set_water_grid_resolution", "n"), &OhaoViewport::set_water_grid_resolution);

    // === Decals ===
    ClassDB::bind_method(D_METHOD("set_decals_enabled", "enabled"), &OhaoViewport::set_decals_enabled);
    ClassDB::bind_method(D_METHOD("get_decals_enabled"), &OhaoViewport::get_decals_enabled);
    ClassDB::bind_method(D_METHOD("add_decal", "pos", "normal", "size", "albedo_path", "opacity", "tint"), &OhaoViewport::add_decal);
    ClassDB::bind_method(D_METHOD("remove_decal", "handle"), &OhaoViewport::remove_decal);
    ClassDB::bind_method(D_METHOD("clear_decals"), &OhaoViewport::clear_decals);

    // === Foliage ===
    ClassDB::bind_method(D_METHOD("set_foliage_enabled", "enabled"), &OhaoViewport::set_foliage_enabled);
    ClassDB::bind_method(D_METHOD("get_foliage_enabled"), &OhaoViewport::get_foliage_enabled);
    ClassDB::bind_method(D_METHOD("set_foliage_cull_distance", "distance"), &OhaoViewport::set_foliage_cull_distance);
    ClassDB::bind_method(D_METHOD("get_foliage_cull_distance"), &OhaoViewport::get_foliage_cull_distance);
    ClassDB::bind_method(D_METHOD("set_grass_texture", "path"), &OhaoViewport::set_grass_texture);
    ClassDB::bind_method(D_METHOD("add_foliage_cluster", "params"), &OhaoViewport::add_foliage_cluster);
    ClassDB::bind_method(D_METHOD("clear_foliage"), &OhaoViewport::clear_foliage);

    // === Camera Mode ===
    ADD_GROUP("Camera", "camera_");

    ClassDB::bind_method(D_METHOD("set_camera_mode", "mode"), &OhaoViewport::set_camera_mode);
    ClassDB::bind_method(D_METHOD("get_camera_mode"), &OhaoViewport::get_camera_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "camera_mode", PROPERTY_HINT_ENUM, "FPS,Orbit"), "set_camera_mode", "get_camera_mode");

    ClassDB::bind_method(D_METHOD("focus_on_scene"), &OhaoViewport::focus_on_scene);

    // Camera position/rotation (game mode - GDScript drives camera)
    ClassDB::bind_method(D_METHOD("set_camera_position", "position"), &OhaoViewport::set_camera_position);
    ClassDB::bind_method(D_METHOD("get_camera_position"), &OhaoViewport::get_camera_position);
    ClassDB::bind_method(D_METHOD("set_camera_rotation_deg", "pitch", "yaw"), &OhaoViewport::set_camera_rotation_deg);
    ClassDB::bind_method(D_METHOD("get_camera_forward"), &OhaoViewport::get_camera_forward);

    // === Input Mode ===
    ClassDB::bind_method(D_METHOD("set_input_mode", "mode"), &OhaoViewport::set_input_mode);
    ClassDB::bind_method(D_METHOD("get_input_mode"), &OhaoViewport::get_input_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "input_mode", PROPERTY_HINT_ENUM, "Editor,Game"),
        "set_input_mode", "get_input_mode");

    ClassDB::bind_method(D_METHOD("pick_object_at", "screen_pos"), &OhaoViewport::pick_object_at);
    ClassDB::bind_method(D_METHOD("get_selected_actor_name"), &OhaoViewport::get_selected_actor_name);
    ClassDB::bind_method(D_METHOD("set_picking_enabled", "enabled"), &OhaoViewport::set_picking_enabled);
    ClassDB::bind_method(D_METHOD("get_picking_enabled"), &OhaoViewport::get_picking_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "picking_enabled"), "set_picking_enabled", "get_picking_enabled");

    // === Physics Controls ===
    ADD_GROUP("Physics", "physics_");

    ClassDB::bind_method(D_METHOD("play_physics"), &OhaoViewport::play_physics);
    ClassDB::bind_method(D_METHOD("pause_physics"), &OhaoViewport::pause_physics);
    ClassDB::bind_method(D_METHOD("step_physics"), &OhaoViewport::step_physics);
    ClassDB::bind_method(D_METHOD("stop_physics"), &OhaoViewport::stop_physics);
    ClassDB::bind_method(D_METHOD("set_physics_speed", "speed"), &OhaoViewport::set_physics_speed);
    ClassDB::bind_method(D_METHOD("get_physics_speed"), &OhaoViewport::get_physics_speed);
    ClassDB::bind_method(D_METHOD("is_physics_playing"), &OhaoViewport::is_physics_playing);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physics_speed", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_physics_speed", "get_physics_speed");
    ClassDB::bind_method(D_METHOD("set_gravity", "gravity"), &OhaoViewport::set_gravity);
    ClassDB::bind_method(D_METHOD("get_gravity"), &OhaoViewport::get_gravity);

    // === Raycasting ===
    ClassDB::bind_method(D_METHOD("cast_ray", "origin", "direction", "max_distance", "layer_mask"), &OhaoViewport::cast_ray, DEFVAL(0xFFFF));
    ClassDB::bind_method(D_METHOD("cast_ray_all", "origin", "direction", "max_distance", "layer_mask"), &OhaoViewport::cast_ray_all, DEFVAL(0xFFFF));
    ClassDB::bind_method(D_METHOD("overlap_sphere", "center", "radius", "layer_mask"), &OhaoViewport::overlap_sphere, DEFVAL(0xFFFF));
    ClassDB::bind_method(D_METHOD("overlap_box", "center", "half_extents", "rotation_deg", "layer_mask"), &OhaoViewport::overlap_box, DEFVAL(0xFFFF));

    // === Collision Layers ===
    ClassDB::bind_method(D_METHOD("set_layer_collision", "layer1", "layer2", "should_collide"), &OhaoViewport::set_layer_collision);

    // === Constraints ===
    ClassDB::bind_method(D_METHOD("create_constraint_fixed", "body1", "body2", "anchor"), &OhaoViewport::create_constraint_fixed);
    ClassDB::bind_method(D_METHOD("create_constraint_hinge", "body1", "body2", "anchor", "axis", "limit_min", "limit_max"), &OhaoViewport::create_constraint_hinge, DEFVAL(0.0f), DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("create_constraint_slider", "body1", "body2", "axis", "limit_min", "limit_max"), &OhaoViewport::create_constraint_slider, DEFVAL(0.0f), DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("create_constraint_point", "body1", "body2", "anchor1", "anchor2"), &OhaoViewport::create_constraint_point);
    ClassDB::bind_method(D_METHOD("create_constraint_distance", "body1", "body2", "anchor1", "anchor2", "min_dist", "max_dist"), &OhaoViewport::create_constraint_distance, DEFVAL(-1.0f), DEFVAL(-1.0f));
    ClassDB::bind_method(D_METHOD("create_constraint_cone", "body1", "body2", "anchor", "twist_axis", "half_cone_angle"), &OhaoViewport::create_constraint_cone, DEFVAL(0.5f));
    ClassDB::bind_method(D_METHOD("destroy_constraint", "handle"), &OhaoViewport::destroy_constraint);
    ClassDB::bind_method(D_METHOD("set_constraint_enabled", "handle", "enabled"), &OhaoViewport::set_constraint_enabled);
    ClassDB::bind_method(D_METHOD("set_constraint_motor", "handle", "enabled", "speed", "max_force"), &OhaoViewport::set_constraint_motor);
    ClassDB::bind_method(D_METHOD("set_constraint_limits", "handle", "min", "max"), &OhaoViewport::set_constraint_limits);
    ClassDB::bind_method(D_METHOD("set_constraint_breaking", "handle", "max_force", "max_torque"), &OhaoViewport::set_constraint_breaking);
    ClassDB::bind_method(D_METHOD("get_and_clear_broken_constraints"), &OhaoViewport::get_and_clear_broken_constraints);

    // === Physics Grab/Throw ===
    ClassDB::bind_method(D_METHOD("grab_body", "body_handle", "world_pos"), &OhaoViewport::grab_body);
    ClassDB::bind_method(D_METHOD("move_grab", "token", "world_pos"), &OhaoViewport::move_grab);
    ClassDB::bind_method(D_METHOD("release_grab", "token"), &OhaoViewport::release_grab);
    ClassDB::bind_method(D_METHOD("throw_grab", "token", "velocity"), &OhaoViewport::throw_grab);

    // === Character Controller ===
    ClassDB::bind_method(D_METHOD("create_character", "position", "capsule_radius", "capsule_height", "max_slope_deg", "mass"), &OhaoViewport::create_character, DEFVAL(50.0f), DEFVAL(80.0f));
    ClassDB::bind_method(D_METHOD("destroy_character", "handle"), &OhaoViewport::destroy_character);
    ClassDB::bind_method(D_METHOD("get_character_state", "handle"), &OhaoViewport::get_character_state);
    ClassDB::bind_method(D_METHOD("set_character_position", "handle", "position"), &OhaoViewport::set_character_position);
    ClassDB::bind_method(D_METHOD("set_character_rotation", "handle", "rotation_deg"), &OhaoViewport::set_character_rotation);
    ClassDB::bind_method(D_METHOD("set_character_velocity", "handle", "velocity"), &OhaoViewport::set_character_velocity);
    ClassDB::bind_method(D_METHOD("update_character", "handle", "delta", "gravity", "movement_input"), &OhaoViewport::update_character);

    // === Wireframe ===
    ADD_GROUP("Debug", "");

    ClassDB::bind_method(D_METHOD("set_wireframe_enabled", "enabled"), &OhaoViewport::set_wireframe_enabled);
    ClassDB::bind_method(D_METHOD("get_wireframe_enabled"), &OhaoViewport::get_wireframe_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wireframe_enabled"), "set_wireframe_enabled", "get_wireframe_enabled");

    // === Grid ===
    ClassDB::bind_method(D_METHOD("set_grid_enabled", "enabled"), &OhaoViewport::set_grid_enabled);
    ClassDB::bind_method(D_METHOD("get_grid_enabled"), &OhaoViewport::get_grid_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "grid_enabled"), "set_grid_enabled", "get_grid_enabled");

    // === Model Import ===
    ClassDB::bind_method(D_METHOD("import_model", "path"), &OhaoViewport::import_model);

    // === Actor Transform API ===
    ClassDB::bind_method(D_METHOD("set_actor_position", "actor_name", "position"), &OhaoViewport::set_actor_position);
    ClassDB::bind_method(D_METHOD("set_actor_rotation", "actor_name", "rotation_deg"), &OhaoViewport::set_actor_rotation);
    ClassDB::bind_method(D_METHOD("set_actor_scale", "actor_name", "scale"), &OhaoViewport::set_actor_scale);
    ClassDB::bind_method(D_METHOD("remove_actor", "actor_name"), &OhaoViewport::remove_actor);
    ClassDB::bind_method(D_METHOD("has_actor", "actor_name"), &OhaoViewport::has_actor);

    // === Actor Physics API ===
    ClassDB::bind_method(D_METHOD("get_actor_body_handle", "actor_name"), &OhaoViewport::get_actor_body_handle);
    ClassDB::bind_method(D_METHOD("set_actor_body_type", "actor_name", "type"), &OhaoViewport::set_actor_body_type);
    ClassDB::bind_method(D_METHOD("set_actor_mass", "actor_name", "mass"), &OhaoViewport::set_actor_mass);
    ClassDB::bind_method(D_METHOD("set_actor_restitution", "actor_name", "restitution"), &OhaoViewport::set_actor_restitution);
    ClassDB::bind_method(D_METHOD("set_actor_friction", "actor_name", "friction"), &OhaoViewport::set_actor_friction);
    ClassDB::bind_method(D_METHOD("set_actor_gravity_enabled", "actor_name", "enabled"), &OhaoViewport::set_actor_gravity_enabled);
    ClassDB::bind_method(D_METHOD("set_actor_gravity_scale", "actor_name", "scale"), &OhaoViewport::set_actor_gravity_scale);
    ClassDB::bind_method(D_METHOD("set_actor_linear_damping", "actor_name", "damping"), &OhaoViewport::set_actor_linear_damping);
    ClassDB::bind_method(D_METHOD("set_actor_angular_damping", "actor_name", "damping"), &OhaoViewport::set_actor_angular_damping);
    ClassDB::bind_method(D_METHOD("apply_radial_impulse", "center", "strength", "radius", "falloff"), &OhaoViewport::apply_radial_impulse);
    ClassDB::bind_method(D_METHOD("set_actor_linear_velocity", "actor_name", "velocity"), &OhaoViewport::set_actor_linear_velocity);
    ClassDB::bind_method(D_METHOD("sync_actor_physics_shape", "actor_name"), &OhaoViewport::sync_actor_physics_shape);

    // === World Force Effects ===
    ClassDB::bind_method(D_METHOD("set_wind", "direction", "strength", "turbulence"), &OhaoViewport::set_wind);
    ClassDB::bind_method(D_METHOD("clear_wind"), &OhaoViewport::clear_wind);
    ClassDB::bind_method(D_METHOD("set_water", "liquid_level", "density", "drag"), &OhaoViewport::set_water);
    ClassDB::bind_method(D_METHOD("clear_water"), &OhaoViewport::clear_water);
    ClassDB::bind_method(D_METHOD("create_force_volume_box", "center", "half_extents", "force"), &OhaoViewport::create_force_volume_box);
    ClassDB::bind_method(D_METHOD("create_force_volume_sphere", "center", "radius", "force"), &OhaoViewport::create_force_volume_sphere);
    ClassDB::bind_method(D_METHOD("destroy_force_volume", "handle"), &OhaoViewport::destroy_force_volume);
    ClassDB::bind_method(D_METHOD("set_force_volume_enabled", "handle", "enabled"), &OhaoViewport::set_force_volume_enabled);

    // === Apply Forces by Actor Name ===
    ClassDB::bind_method(D_METHOD("apply_force_on_actor", "actor_name", "force", "rel_pos"), &OhaoViewport::apply_force_on_actor);
    ClassDB::bind_method(D_METHOD("apply_impulse_on_actor", "actor_name", "impulse", "rel_pos"), &OhaoViewport::apply_impulse_on_actor);
    ClassDB::bind_method(D_METHOD("apply_torque_on_actor", "actor_name", "torque"), &OhaoViewport::apply_torque_on_actor);

    // === Velocity / Mass Getters ===
    ClassDB::bind_method(D_METHOD("get_actor_linear_velocity", "actor_name"), &OhaoViewport::get_actor_linear_velocity);
    ClassDB::bind_method(D_METHOD("get_actor_angular_velocity", "actor_name"), &OhaoViewport::get_actor_angular_velocity);
    ClassDB::bind_method(D_METHOD("get_actor_mass", "actor_name"), &OhaoViewport::get_actor_mass);

    // === CCD ===
    ClassDB::bind_method(D_METHOD("set_actor_ccd_enabled", "actor_name", "enabled"), &OhaoViewport::set_actor_ccd_enabled);

    // === Axis Lock ===
    ClassDB::bind_method(D_METHOD("set_actor_freeze_linear", "actor_name", "x", "y", "z"), &OhaoViewport::set_actor_freeze_linear);
    ClassDB::bind_method(D_METHOD("set_actor_freeze_rotation", "actor_name", "x", "y", "z"), &OhaoViewport::set_actor_freeze_rotation);

    // === Sleep / Wake ===
    ClassDB::bind_method(D_METHOD("set_actor_awake", "actor_name", "awake"), &OhaoViewport::set_actor_awake);
    ClassDB::bind_method(D_METHOD("is_actor_awake", "actor_name"), &OhaoViewport::is_actor_awake);

    // === Collision Layer Assignment ===
    ClassDB::bind_method(D_METHOD("set_actor_layer", "actor_name", "layer"), &OhaoViewport::set_actor_layer);

    // === Springs ===
    ClassDB::bind_method(D_METHOD("create_spring", "body1_name", "body2_name", "stiffness", "rest_length", "damping"), &OhaoViewport::create_spring);
    ClassDB::bind_method(D_METHOD("create_anchor_spring", "body_name", "anchor", "stiffness", "rest_length", "damping"), &OhaoViewport::create_anchor_spring);
    ClassDB::bind_method(D_METHOD("destroy_spring", "handle"), &OhaoViewport::destroy_spring);
    ClassDB::bind_method(D_METHOD("set_spring_enabled", "handle", "enabled"), &OhaoViewport::set_spring_enabled);

    // === Texture / Material API ===
    ADD_GROUP("Materials", "");
    ClassDB::bind_method(D_METHOD("set_actor_texture", "actor_name", "texture_path"), &OhaoViewport::set_actor_texture);
    ClassDB::bind_method(D_METHOD("set_actor_normal_map", "actor_name", "normal_path"), &OhaoViewport::set_actor_normal_map);
    ClassDB::bind_method(D_METHOD("set_actor_pbr", "actor_name", "metallic", "roughness"), &OhaoViewport::set_actor_pbr);
    ClassDB::bind_method(D_METHOD("set_actor_material_preset", "actor_name", "preset_name"), &OhaoViewport::set_actor_material_preset);

    // === Particles ===
    ClassDB::bind_method(D_METHOD("spawn_particles", "position", "type"), &OhaoViewport::spawn_particles);
    ClassDB::bind_method(D_METHOD("spawn_particles_directed", "position", "type", "direction"), &OhaoViewport::spawn_particles_directed);

    // === Gizmo Controls ===
    ADD_GROUP("Gizmo", "gizmo_");

    ClassDB::bind_method(D_METHOD("set_gizmo_mode", "mode"), &OhaoViewport::set_gizmo_mode);
    ClassDB::bind_method(D_METHOD("get_gizmo_mode"), &OhaoViewport::get_gizmo_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "gizmo_mode", PROPERTY_HINT_ENUM, "Translate,Rotate,Scale"), "set_gizmo_mode", "get_gizmo_mode");

    ClassDB::bind_method(D_METHOD("set_gizmo_enabled", "enabled"), &OhaoViewport::set_gizmo_enabled);
    ClassDB::bind_method(D_METHOD("get_gizmo_enabled"), &OhaoViewport::get_gizmo_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gizmo_enabled"), "set_gizmo_enabled", "get_gizmo_enabled");

    // === Audio ===
    ADD_GROUP("Audio", "audio_");

    ClassDB::bind_method(D_METHOD("play_sound", "path", "category", "loop", "volume"), &OhaoViewport::play_sound, DEFVAL(0), DEFVAL(false), DEFVAL(1.0f));
    ClassDB::bind_method(D_METHOD("play_sound_at", "path", "position", "category", "loop", "volume"), &OhaoViewport::play_sound_at, DEFVAL(0), DEFVAL(false), DEFVAL(1.0f));
    ClassDB::bind_method(D_METHOD("stop_sound", "handle"), &OhaoViewport::stop_sound);
    ClassDB::bind_method(D_METHOD("pause_sound", "handle"), &OhaoViewport::pause_sound);
    ClassDB::bind_method(D_METHOD("resume_sound", "handle"), &OhaoViewport::resume_sound);
    ClassDB::bind_method(D_METHOD("set_sound_volume", "handle", "volume"), &OhaoViewport::set_sound_volume);
    ClassDB::bind_method(D_METHOD("set_sound_position", "handle", "position"), &OhaoViewport::set_sound_position);
    ClassDB::bind_method(D_METHOD("set_category_volume", "category", "volume"), &OhaoViewport::set_category_volume);
    ClassDB::bind_method(D_METHOD("get_category_volume", "category"), &OhaoViewport::get_category_volume);
    ClassDB::bind_method(D_METHOD("stop_category", "category"), &OhaoViewport::stop_category);
    ClassDB::bind_method(D_METHOD("pause_category", "category"), &OhaoViewport::pause_category);
    ClassDB::bind_method(D_METHOD("resume_category", "category"), &OhaoViewport::resume_category);
    ClassDB::bind_method(D_METHOD("set_master_volume", "volume"), &OhaoViewport::set_master_volume);
    ClassDB::bind_method(D_METHOD("get_master_volume"), &OhaoViewport::get_master_volume);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "audio_master_volume", PROPERTY_HINT_RANGE, "0.0,1.0,0.05"), "set_master_volume", "get_master_volume");
    ClassDB::bind_method(D_METHOD("stop_all_sounds"), &OhaoViewport::stop_all_sounds);

    // Signals
    ADD_SIGNAL(MethodInfo("actor_selected", PropertyInfo(Variant::STRING, "name")));
    ADD_SIGNAL(MethodInfo("right_click_menu", PropertyInfo(Variant::VECTOR2, "position")));
}

// ===== Lifecycle =====

OhaoViewport::OhaoViewport() {
    UtilityFunctions::print("[OHAO] AAA Viewport created");
}

OhaoViewport::~OhaoViewport() {
    shutdown_renderer();
}

void OhaoViewport::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_RESIZED:
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
    UtilityFunctions::print("[OHAO] AAA Viewport ready - initializing deferred renderer");

    Vector2 size = get_size();
    if (size.x > 0 && size.y > 0) {
        m_width = static_cast<int>(size.x);
        m_height = static_cast<int>(size.y);
    }

    set_focus_mode(FOCUS_ALL);
    initialize_renderer();
}

void OhaoViewport::_process(double delta) {
    if (!m_initialized || !m_render_enabled) {
        return;
    }

    // Delegate camera movement (skip in GAME mode — GDScript drives camera)
    if (!m_game_mode) {
        m_camera.update(delta, m_renderer->getCamera());
    }

    // Sync audio listener to camera (always, even in GAME mode)
    m_audio.updateListener(m_renderer->getCamera());

    // Physics
    if (m_physics.isPlaying() && m_renderer) {
        m_renderer->updatePhysics(static_cast<float>(delta) * m_physics.getSpeed());
    }

    // Drain and dispatch contact events as Godot signals
    if (m_scene) {
        auto* physWorld = m_scene->getPhysicsWorld();
        if (physWorld && physWorld->hasBackend()) {
            auto events = physWorld->getContactEvents();
            for (auto& ev : events) {
                std::string n1 = physWorld->resolveHandleName(ev.body1);
                std::string n2 = physWorld->resolveHandleName(ev.body2);
                if (n1.empty() || n2.empty()) continue;
                if (ev.type == ohao::physics::backend::ContactEvent::BEGIN) {
                    emit_signal("body_entered",
                        String(n1.c_str()), String(n2.c_str()),
                        Vector3(ev.contactPoint.x, ev.contactPoint.y, ev.contactPoint.z),
                        static_cast<double>(glm::length(ev.relativeVelocity)));
                } else if (ev.type == ohao::physics::backend::ContactEvent::END) {
                    emit_signal("body_exited", String(n1.c_str()), String(n2.c_str()));
                }
            }
        }
    }

    // Gizmo transform for selected object (skip in GAME mode)
    m_selection.updateGizmo(m_renderer, !m_game_mode && m_gizmo_enabled);

    // Delta time + drain lightning signal
    if (m_renderer) {
        ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
        if (deferred) {
            deferred->setDeltaTime(static_cast<float>(delta));
            if (deferred->consumeLightningPending()) {
                emit_signal("lightning_struck");
            }
        }
    }

    // Render frame
    if (m_renderer) {
        m_renderer->render();
        sync_terrain_weather_friction();

        const uint8_t* pixels = m_renderer->getPixels();
        if (pixels && m_image.is_valid()) {
            PackedByteArray data;
            data.resize(m_width * m_height * 4);
            memcpy(data.ptrw(), pixels, m_width * m_height * 4);
            m_image->set_data(m_width, m_height, false, Image::FORMAT_RGBA8, data);
            if (m_texture.is_valid()) {
                m_texture->update(m_image);
            }
        }
        queue_redraw();
    }
}

void OhaoViewport::_draw() {
    if (!m_initialized || !m_texture.is_valid()) {
        draw_rect(Rect2(0, 0, get_size().x, get_size().y), Color(0.1, 0.1, 0.12, 1.0));
        return;
    }

    draw_texture(m_texture, Vector2(0, 0));

    // Game mode: no editor overlays
    if (m_game_mode) return;

    // "OHAO Engine" text overlay when scene is empty
    if (!has_scene_meshes()) {
        Ref<Font> font = get_theme_default_font();
        if (font.is_valid()) {
            String text = "OHAO AAA Engine";
            int font_size = 32;
            Vector2 text_size = font->get_string_size(text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size);
            Vector2 center = get_size() / 2.0;
            Vector2 text_pos = center - text_size / 2.0;
            text_pos.y += text_size.y / 2.0;
            draw_string(font, text_pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.6, 0.6, 0.7, 0.8));

            int rm = m_render_settings.getRenderMode();
            String subtitle = rm == 1 ? "Deferred Rendering Mode" : "Forward Rendering Mode";
            int sub_size = 14;
            Vector2 sub_text_size = font->get_string_size(subtitle, HORIZONTAL_ALIGNMENT_CENTER, -1, sub_size);
            Vector2 sub_pos = center - sub_text_size / 2.0;
            sub_pos.y += text_size.y / 2.0 + 30;
            draw_string(font, sub_pos, subtitle, HORIZONTAL_ALIGNMENT_LEFT, -1, sub_size, Color(0.5, 0.5, 0.55, 0.6));

            String hint = "Scenes auto-sync from Godot's 3D editor";
            Vector2 hint_size = font->get_string_size(hint, HORIZONTAL_ALIGNMENT_CENTER, -1, sub_size);
            Vector2 hint_pos = center - hint_size / 2.0;
            hint_pos.y += text_size.y / 2.0 + 50;
            draw_string(font, hint_pos, hint, HORIZONTAL_ALIGNMENT_LEFT, -1, sub_size, Color(0.4, 0.4, 0.45, 0.5));
        }
    }

    // Selection highlight overlay (math in SelectionController, draw calls here)
    if (m_selection.hasSelection() && m_renderer) {
        ohao::Camera& camera = m_renderer->getCamera();
        Vector2 ctrl_size = get_size();
        SelectionOverlay sel = m_selection.computeSelectionBounds(
            camera.getViewProjectionMatrix(), ctrl_size.x, ctrl_size.y);

        if (sel.visible) {
            Vector2 tl(sel.minX, sel.minY);
            Vector2 br(sel.maxX, sel.maxY);
            float cx = sel.cornerX, cy = sel.cornerY;

            Color glow_color(1.0f, 0.6f, 0.0f, 0.25f);
            Color sel_color(1.0f, 0.7f, 0.1f, 1.0f);
            Color fill_color(1.0f, 0.7f, 0.1f, 0.06f);

            draw_rect(Rect2(tl, br - tl), fill_color, true);
            draw_rect(Rect2(tl, br - tl), glow_color, false, 5.0f);

            float line_w = 2.5f;
            draw_line(tl, Vector2(tl.x + cx, tl.y), sel_color, line_w);
            draw_line(tl, Vector2(tl.x, tl.y + cy), sel_color, line_w);
            draw_line(Vector2(br.x, tl.y), Vector2(br.x - cx, tl.y), sel_color, line_w);
            draw_line(Vector2(br.x, tl.y), Vector2(br.x, tl.y + cy), sel_color, line_w);
            draw_line(Vector2(tl.x, br.y), Vector2(tl.x + cx, br.y), sel_color, line_w);
            draw_line(Vector2(tl.x, br.y), Vector2(tl.x, br.y - cy), sel_color, line_w);
            draw_line(br, Vector2(br.x - cx, br.y), sel_color, line_w);
            draw_line(br, Vector2(br.x, br.y - cy), sel_color, line_w);

            Ref<Font> font = get_theme_default_font();
            if (font.is_valid()) {
                int font_size = 14;
                String label = m_selection.getSelectedActorName();
                float text_w = font->get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
                float pill_h = font_size + 8.0f;
                float pill_w = text_w + 16.0f;
                float pill_x = (tl.x + br.x) * 0.5f - pill_w * 0.5f;
                float pill_y = tl.y - pill_h - 4.0f;

                draw_rect(Rect2(pill_x, pill_y, pill_w, pill_h), Color(0.0f, 0.0f, 0.0f, 0.75f), true);
                draw_rect(Rect2(pill_x, pill_y, pill_w, pill_h), sel_color, false, 1.5f);
                draw_string(font, Vector2(pill_x + 8.0f, pill_y + font_size + 2.0f), label,
                           HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, sel_color);
            }
        }
    }
}

bool OhaoViewport::has_scene_meshes() const {
    if (!m_renderer) return false;
    return m_renderer->hasSceneMeshes();
}

// ===== Input =====

void OhaoViewport::_gui_input(const Ref<InputEvent>& p_event) {
    if (!m_initialized || !m_renderer) {
        return;
    }

    // GAME mode: GDScript handles all input
    if (m_game_mode) return;

    // Auto-enable always-look in FPS mode when Godot mouse is captured
    bool godot_captured = Input::get_singleton()->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
    m_camera.setAlwaysLook(m_camera.getMode() == CameraController::FPS && godot_captured);

    // Mouse motion -> camera
    Ref<InputEventMouseMotion> motion_event = p_event;
    if (motion_event.is_valid()) {
        Vector2 relative = motion_event->get_relative();
        m_camera.handleMouseMotion(relative.x, relative.y, m_renderer->getCamera());
        return;
    }

    // Mouse button
    Ref<InputEventMouseButton> button_event = p_event;
    if (button_event.is_valid()) {
        MouseButton button = button_event->get_button_index();

        // Left-click: picking (only when picking is enabled)
        if (button == MOUSE_BUTTON_LEFT && button_event->is_pressed() && m_picking_enabled) {
            pick_object_at(button_event->get_position());
            return;
        }

        // Everything else: camera
        bool consumed = m_camera.handleMouseButton(
            static_cast<int>(button), button_event->is_pressed(),
            button_event->get_position().x, button_event->get_position().y,
            m_renderer->getCamera());

        if (consumed) {
            // Right-click or middle-click needs focus
            if (button == MOUSE_BUTTON_RIGHT || button == MOUSE_BUTTON_MIDDLE) {
                if (button_event->is_pressed()) {
                    grab_focus();
                }
            }
            // Right-click release: check for context menu
            if (button == MOUSE_BUTTON_RIGHT && !button_event->is_pressed()) {
                if (!m_camera.wasRightClickDrag()) {
                    emit_signal("right_click_menu", button_event->get_global_position());
                }
            }
        }
        return;
    }

    // Keyboard -> camera
    Ref<InputEventKey> key_event = p_event;
    if (key_event.is_valid()) {
        if (m_camera.handleKey(static_cast<int>(key_event->get_keycode()), key_event->is_pressed())) {
            accept_event();
        }
        return;
    }
}

// ===== Renderer Management =====

void OhaoViewport::initialize_renderer() {
    if (m_initialized) return;

    UtilityFunctions::print("[OHAO] Initializing Vulkan AAA renderer...");

    m_renderer = new ohao::OffscreenRenderer(m_width, m_height);
    if (!m_renderer->initialize()) {
        UtilityFunctions::printerr("[OHAO] Failed to initialize renderer!");
        delete m_renderer;
        m_renderer = nullptr;
        return;
    }

    int rm = m_render_settings.getRenderMode();
    m_renderer->setRenderMode(rm == 1 ? ohao::RenderMode::Deferred : ohao::RenderMode::Forward);

    m_scene = new ohao::Scene("GodotScene");
    m_renderer->setScene(m_scene);

    m_image = Image::create(m_width, m_height, false, Image::FORMAT_RGBA8);
    m_texture = ImageTexture::create_from_image(m_image);

    m_initialized = true;

    m_renderer->getCamera().setAspectRatio(static_cast<float>(m_width) / static_cast<float>(m_height));

    // Apply initial render settings
    m_render_settings.apply(m_renderer);

    // Initialize audio system
    std::string soundsPath = SceneSync::resolveResPath(String("res://sounds"));
    m_audio.initialize(soundsPath);

    UtilityFunctions::print("[OHAO] AAA Renderer initialized: ", m_width, "x", m_height,
                            " Mode: ", rm == 1 ? "Deferred" : "Forward");
}

void OhaoViewport::shutdown_renderer() {
    if (!m_initialized) return;

    UtilityFunctions::print("[OHAO] Shutting down renderer...");

    // Shutdown audio before renderer
    m_audio.shutdown();

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
    if (enabled) queue_redraw();
}

void OhaoViewport::set_viewport_size(int width, int height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    if (m_renderer) {
        m_renderer->resize(width, height);
        m_renderer->getCamera().setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
    }

    if (m_initialized) {
        m_image = Image::create(m_width, m_height, false, Image::FORMAT_RGBA8);
        m_texture = ImageTexture::create_from_image(m_image);
    }

    UtilityFunctions::print("[OHAO] Viewport resized: ", width, "x", height);
}

// ===== Render Settings (delegates to RenderSettings) =====

void OhaoViewport::set_render_mode(int mode) {
    m_render_settings.setRenderMode(mode);
    if (m_renderer) {
        m_renderer->setRenderMode(mode == 1 ? ohao::RenderMode::Deferred : ohao::RenderMode::Forward);
        UtilityFunctions::print("[OHAO] Render mode set to: ", mode == 1 ? "Deferred" : "Forward");
    }
}

void OhaoViewport::set_bloom_enabled(bool enabled)        { m_render_settings.setBloomEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_taa_enabled(bool enabled)          { m_render_settings.setTAAEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssao_enabled(bool enabled)         { m_render_settings.setSSAOEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssgi_enabled(bool enabled)         { m_render_settings.setSSGIEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssr_enabled(bool enabled)          { m_render_settings.setSSREnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_volumetrics_enabled(bool enabled)  { m_render_settings.setVolumetricsEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_motion_blur_enabled(bool enabled)  { m_render_settings.setMotionBlurEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_dof_enabled(bool enabled)          { m_render_settings.setDoFEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_tonemapping_enabled(bool enabled)  { m_render_settings.setTonemappingEnabled(enabled); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_tonemap_operator(int op)           { m_render_settings.setTonemapOperator(op); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_exposure(float exposure)            { m_render_settings.setExposure(exposure); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_gamma(float gamma)                  { m_render_settings.setGamma(gamma); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_bloom_threshold(float threshold)    { m_render_settings.setBloomThreshold(threshold); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_bloom_intensity(float intensity)    { m_render_settings.setBloomIntensity(intensity); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_ssao_radius(float radius)           { m_render_settings.setSSAORadius(radius); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssao_intensity(float intensity)     { m_render_settings.setSSAOIntensity(intensity); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_ssgi_radius(float radius)           { m_render_settings.setSSGIRadius(radius); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssgi_intensity(float intensity)     { m_render_settings.setSSGIIntensity(intensity); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssgi_sample_count(int count)        { m_render_settings.setSSGISampleCount(count); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_ssr_max_distance(float dist)        { m_render_settings.setSSRMaxDistance(dist); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_ssr_thickness(float thickness)      { m_render_settings.setSSRThickness(thickness); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_volumetric_density(float density)   { m_render_settings.setVolumetricDensity(density); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_volumetric_scattering(float g)      { m_render_settings.setVolumetricScattering(g); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_fog_color(const Color& color) {
    m_render_settings.setFogColor(glm::vec3(color.r, color.g, color.b));
    m_render_settings.apply(m_renderer);
}
Color OhaoViewport::get_fog_color() const {
    glm::vec3 c = m_render_settings.getFogColor();
    return Color(c.r, c.g, c.b);
}

void OhaoViewport::set_motion_blur_intensity(float intensity) { m_render_settings.setMotionBlurIntensity(intensity); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_motion_blur_samples(int samples)       { m_render_settings.setMotionBlurSamples(samples); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_dof_focus_distance(float distance)  { m_render_settings.setDoFFocusDistance(distance); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_dof_aperture(float aperture)        { m_render_settings.setDoFAperture(aperture); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_dof_max_blur(float blur)            { m_render_settings.setDoFMaxBlur(blur); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_taa_blend_factor(float factor)      { m_render_settings.setTAABlendFactor(factor); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_sky_enabled(bool enabled)   { m_render_settings.setSkyEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_sky_turbidity(float t)      { m_render_settings.setSkyTurbidity(t); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_sky_intensity(float i)      { m_render_settings.setSkyIntensity(i); m_render_settings.apply(m_renderer); }

void OhaoViewport::set_sun_direction(const Vector3& dir) {
    m_render_settings.setSunDirection(glm::vec3(dir.x, dir.y, dir.z));
    m_render_settings.apply(m_renderer);
}

Vector3 OhaoViewport::get_sun_direction() const {
    glm::vec3 d = m_render_settings.getSunDirection();
    return Vector3(d.x, d.y, d.z);
}

void OhaoViewport::set_time_of_day(float hours) {
    // Clamp to [0, 24)
    hours = std::fmod(hours, 24.0f);
    if (hours < 0.0f) hours += 24.0f;
    m_time_of_day = hours;

    // Sun arc: dawn at 6am, zenith at noon, dusk at 18pm
    // t = 0 at dawn (6am), t = 0.5 at noon, t = 1 at dusk (18pm)
    float t = glm::clamp((hours - 6.0f) / 12.0f, 0.0f, 1.0f);
    float elevAngle = t * glm::pi<float>();  // 0 → PI
    float sinElev = std::sin(elevAngle);     // 0 → 1 → 0 (vertical height)
    float cosElev = std::cos(elevAngle);     // 1 → 0 → -1 (east → west)

    // Sun direction vector (toward sun from world origin)
    // East at 6am (-cosElev=−1,+X), overhead at noon (0,+Y), West at 6pm (+X)
    // Small +Z tilt for natural south-sky angle
    glm::vec3 sunDir = glm::normalize(glm::vec3(-cosElev, sinElev + 0.01f, 0.3f));

    // Sky intensity: smooth ramp up after dawn, peak at noon, down before dusk; dark at night
    float dayWindow = glm::clamp((hours - 5.0f), 0.0f, 1.0f)   // fade in from 5am
                    * glm::clamp((19.5f - hours), 0.0f, 1.0f);  // fade out to 7:30pm
    float dayIntensity = glm::mix(0.15f, 1.0f, sinElev);        // horizon dim, noon bright
    float skyIntensity = glm::mix(0.04f, dayIntensity, dayWindow);

    // Turbidity: clear at noon, atmospheric at dawn/dusk (more scattering)
    float turbidity = glm::mix(3.5f, 2.0f, sinElev);            // dusk=3.5, noon=2.0
    turbidity = glm::clamp(turbidity, 1.8f, 5.0f);

    m_render_settings.setSunDirection(sunDir);
    m_render_settings.setSkyIntensity(skyIntensity);
    m_render_settings.setSkyTurbidity(turbidity);
    m_render_settings.apply(m_renderer);
}

void OhaoViewport::set_rain_enabled(bool enabled) { m_render_settings.setRainEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_rain_intensity(float v)    { m_render_settings.setRainIntensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_rain_wind_x(float v)       { m_render_settings.setRainWindX(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_wetness_rate(float v)      { m_render_settings.setWetnessRate(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_drying_rate(float v)       { m_render_settings.setDryingRate(v); m_render_settings.apply(m_renderer); }
float OhaoViewport::get_surface_wetness() const {
    if (!m_renderer) return 0.0f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getSurfaceWetness() : 0.0f;
}
void OhaoViewport::set_lightning_enabled(bool v)    { m_render_settings.setLightningEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_lightning_interval(float v)  { m_render_settings.setLightningInterval(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_lightning_brightness(float v){ m_render_settings.setLightningBrightness(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::trigger_lightning() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->triggerLightning();
}

void OhaoViewport::set_snow_enabled(bool v)       { m_render_settings.setSnowEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_snow_intensity(float v)    { m_render_settings.setSnowIntensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_snow_wind_x(float v)       { m_render_settings.setSnowWindX(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_snow_accum_rate(float v)   { m_render_settings.setSnowAccumRate(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_snow_melt_rate(float v)    { m_render_settings.setSnowMeltRate(v); m_render_settings.apply(m_renderer); }
float OhaoViewport::get_snow_accumulation() const {
    if (!m_renderer) return 0.0f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getSnowAccumulation() : 0.0f;
}

void OhaoViewport::set_cloud_enabled(bool enabled) { m_render_settings.setCloudEnabled(enabled); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_cloud_coverage(float v)     { m_render_settings.setCloudCoverage(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_cloud_density(float v)      { m_render_settings.setCloudDensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_cloud_altitude_min(float v) { m_render_settings.setCloudAltMin(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_cloud_altitude_max(float v) { m_render_settings.setCloudAltMax(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_cloud_speed(float v)        { m_render_settings.setCloudSpeed(v); m_render_settings.apply(m_renderer); }

// Sand (sandstorm)
void OhaoViewport::set_sand_enabled(bool v)    { m_render_settings.setSandEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_sand_intensity(float v) { m_render_settings.setSandIntensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_sand_wind_x(float v)    { m_render_settings.setSandWindX(v); m_render_settings.apply(m_renderer); }

// Frost
void OhaoViewport::set_frost_accum_rate(float v) { m_render_settings.setFrostAccumRate(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_frost_melt_rate(float v)  { m_render_settings.setFrostMeltRate(v); m_render_settings.apply(m_renderer); }
float OhaoViewport::get_frost_cover() const {
    if (!m_renderer) return 0.0f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getFrostCover() : 0.0f;
}

// God rays
void OhaoViewport::set_god_rays_enabled(bool v)    { m_render_settings.setGodRaysEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_god_rays_intensity(float v)  { m_render_settings.setGodRaysIntensity(v); m_render_settings.apply(m_renderer); }

// Aurora
void OhaoViewport::set_aurora_enabled(bool v)    { m_render_settings.setAuroraEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_aurora_intensity(float v) { m_render_settings.setAuroraIntensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_aurora_hue(float v)       { m_render_settings.setAuroraHue(v); m_render_settings.apply(m_renderer); }

// Rainbow
void OhaoViewport::set_rainbow_enabled(bool v) { m_render_settings.setRainbowEnabled(v); m_render_settings.apply(m_renderer); }

// Heat haze
void OhaoViewport::set_heat_haze_enabled(bool v)    { m_render_settings.setHeatHazeEnabled(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_heat_haze_intensity(float v) { m_render_settings.setHeatHazeIntensity(v); m_render_settings.apply(m_renderer); }
void OhaoViewport::set_heat_haze_frequency(float v) { m_render_settings.setHeatHazeFrequency(v); m_render_settings.apply(m_renderer); }

// ===== Terrain =====

void OhaoViewport::set_terrain_enabled(bool v) {
    m_render_settings.setTerrainEnabled(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_terrain_height_scale(float v) {
    m_render_settings.setTerrainHeightScale(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_terrain_size(float v) {
    m_render_settings.setTerrainSize(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_terrain_heightmap(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->setTerrainHeightmapPath(SceneSync::resolveResPath(path));
}
void OhaoViewport::set_terrain_splat_map(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->setTerrainSplatMapPath(SceneSync::resolveResPath(path));
}
void OhaoViewport::set_terrain_layer(int layer_index, const String& albedo_path, const String& normal_path) {
    if (!m_renderer || layer_index < 0 || layer_index > 3) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->setTerrainLayerAlbedo(static_cast<uint32_t>(layer_index), SceneSync::resolveResPath(albedo_path));
    deferred->setTerrainLayerNormal(static_cast<uint32_t>(layer_index), SceneSync::resolveResPath(normal_path));
}
void OhaoViewport::set_terrain_type(int type) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainType(type);
}
void OhaoViewport::set_terrain_frequency(float f) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainGenFrequency(f);
}
void OhaoViewport::set_terrain_octaves(int n) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainGenOctaves(n);
}
void OhaoViewport::set_terrain_offset(Vector2 off) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainGenOffset(glm::vec2(off.x, off.y));
}
void OhaoViewport::set_terrain_gen_resolution(int r) {
    if (!m_renderer || r < 64) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainGenResolution(static_cast<uint32_t>(r));
}
void OhaoViewport::set_terrain_macro_variation(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainMacroVariationPath(SceneSync::resolveResPath(path));
}
void OhaoViewport::generate_terrain() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->generateTerrain();
}

bool OhaoViewport::sync_terrain_physics() {
    if (!m_renderer) return false;

    // Read heightmap from GPU to CPU (blocking, one-time operation)
    std::vector<float> heights;
    uint32_t res = 0;
    if (!m_renderer->readTerrainHeights(heights, res) || heights.empty()) {
        std::cerr << "sync_terrain_physics: no procedural terrain to sync\n";
        return false;
    }

    // Get terrain params needed for scaling
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return false;
    float worldSize   = deferred->getTerrainSize();
    float heightScale = deferred->getTerrainHeightScale();

    // Get physics world directly from m_scene
    if (!m_scene) return false;
    auto* physWorld = m_scene->getPhysicsWorld();
    if (!physWorld) return false;

    // Create Jolt static heightfield body
    auto handle = physWorld->addTerrainHeightfield(
        heights.data(), res, worldSize, heightScale);

    if (handle == ohao::physics::backend::INVALID_BODY) {
        std::cerr << "sync_terrain_physics: Jolt heightfield creation failed\n";
        return false;
    }

    std::cout << "sync_terrain_physics: terrain body created (handle=" << handle
              << ", res=" << res << ", size=" << worldSize << "m)\n";
    return true;
}

void OhaoViewport::sync_terrain_weather_friction() {
    if (!m_renderer || !m_scene) return;
    auto* physWorld = m_scene->getPhysicsWorld();
    if (!physWorld) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    physWorld->updateTerrainFriction(
        deferred->getSurfaceWetness(),
        deferred->getSnowAccumulation(),
        deferred->getFrostCover()
    );
}

// ===== Terrain — runtime splatmap painting =====

void OhaoViewport::paint_terrain_splat(Vector3 world_pos, int channel, float radius, float strength) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    auto* tp = deferred->getTerrainPass();
    if (!tp) return;
    tp->paintSplat(glm::vec2(world_pos.x, world_pos.z), channel, radius, strength,
                   deferred->getTerrainSize());
}

void OhaoViewport::clear_terrain_splat() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    auto* tp = deferred->getTerrainPass();
    if (tp) tp->clearSplatPaint();
}

// ===== Terrain — multi-tile streaming =====

int OhaoViewport::add_terrain_tile(float offset_x, float offset_z) {
    if (!m_renderer) return -1;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->addTerrainTile(offset_x, offset_z) : -1;
}

void OhaoViewport::clear_terrain_tiles() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->clearTerrainTiles();
}

void OhaoViewport::set_terrain_tile_cull_radius(float r) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setTerrainTileCullRadius(r);
}

// ===== Water =====

void OhaoViewport::set_water_enabled(bool v) {
    m_render_settings.setWaterEnabled(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_water_level(float v) {
    m_render_settings.setWaterLevel(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_water_size(float v) {
    m_render_settings.setWaterSize(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_water_foam_intensity(float v) {
    m_render_settings.setWaterFoamIntensity(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_water_wave_amplitude(float v) {
    m_render_settings.setWaterWaveAmplitude(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_water_normal_maps(const String& nm1_path, const String& nm2_path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->setWaterNormalMap1(SceneSync::resolveResPath(nm1_path));
    deferred->setWaterNormalMap2(SceneSync::resolveResPath(nm2_path));
}
void OhaoViewport::set_water_shallow_color(const Color& c) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterColors(glm::vec3(c.r, c.g, c.b), glm::vec3(0.02f, 0.12f, 0.22f));
}
void OhaoViewport::set_water_deep_color(const Color& c) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterColors(glm::vec3(0.10f, 0.40f, 0.50f), glm::vec3(c.r, c.g, c.b));
}
void OhaoViewport::set_water_sun_intensity(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterSunDirection(glm::vec3(-0.3f, 0.8f, -0.5f), v);
}

// ===== Wave Mode (Gerstner / FFT) =====

void OhaoViewport::set_wave_mode(int mode) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setWaveMode(mode);
}
int OhaoViewport::get_wave_mode() const {
    if (!m_renderer) return 0;
    auto* d = m_renderer->getDeferredRenderer();
    return d ? d->getWaveMode() : 0;
}
void OhaoViewport::set_fft_wind_speed(float s) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setFFTWindSpeed(s);
}
float OhaoViewport::get_fft_wind_speed() const {
    if (!m_renderer) return 8.0f;
    auto* d = m_renderer->getDeferredRenderer();
    return d ? d->getFFTWindSpeed() : 8.0f;
}
void OhaoViewport::set_fft_wind_direction(const Vector2& dir) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setFFTWindDirection(dir.x, dir.y);
}
void OhaoViewport::set_fft_patch_size(float s) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setFFTPatchSize(s);
}
float OhaoViewport::get_fft_patch_size() const {
    if (!m_renderer) return 500.0f;
    auto* d = m_renderer->getDeferredRenderer();
    return d ? d->getFFTPatchSize() : 500.0f;
}
void OhaoViewport::set_fft_choppiness(float c) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setFFTChoppiness(c);
}
float OhaoViewport::get_fft_choppiness() const {
    if (!m_renderer) return 1.4f;
    auto* d = m_renderer->getDeferredRenderer();
    return d ? d->getFFTChoppiness() : 1.4f;
}
void OhaoViewport::set_fft_normal_strength(float v) {
    if (!m_renderer) return;
    auto* d = m_renderer->getDeferredRenderer();
    if (d) d->setFFTNormalStrength(v);
}

// ===== Caustics =====

void OhaoViewport::set_caustics_enabled(bool v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setCausticsEnabled(v);
}
bool OhaoViewport::get_caustics_enabled() const {
    if (!m_renderer) return false;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getCausticsEnabled() : false;
}
void OhaoViewport::set_caustics_intensity(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setCausticsIntensity(v);
}
float OhaoViewport::get_caustics_intensity() const {
    if (!m_renderer) return 0.5f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getCausticsIntensity() : 0.5f;
}
void OhaoViewport::set_caustics_texture(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setCausticsTexturePath(SceneSync::resolveResPath(path));
}

// ===== Water Ripples =====

void OhaoViewport::set_water_ripples_enabled(bool v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterRipplesEnabled(v);
}
bool OhaoViewport::get_water_ripples_enabled() const {
    if (!m_renderer) return false;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getWaterRipplesEnabled() : false;
}
void OhaoViewport::add_water_ripple(const Vector3& world_pos, float strength) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->addWaterRipple(world_pos.x, world_pos.z, strength);
}
void OhaoViewport::clear_water_ripples() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->clearWaterRipples();
}

// ===== Enhanced Water =====

void OhaoViewport::set_water_sss_strength(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterSSSStrength(v);
}
float OhaoViewport::get_water_sss_strength() const {
    if (!m_renderer) return 0.35f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getWaterSSSStrength() : 0.35f;
}
void OhaoViewport::set_water_foam_texture(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterFoamTexturePath(SceneSync::resolveResPath(path));
}

// ===== Underwater =====

void OhaoViewport::set_underwater_enabled(bool v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterEnabled(v);
}
bool OhaoViewport::get_underwater_enabled() const {
    if (!m_renderer) return false;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getUnderwaterEnabled() : false;
}
void OhaoViewport::set_underwater_fog_color(const Color& c) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterFogColor(glm::vec3(c.r, c.g, c.b));
}
void OhaoViewport::set_underwater_fog_density(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterFogDensity(v);
}
float OhaoViewport::get_underwater_fog_density() const {
    if (!m_renderer) return 0.12f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getUnderwaterFogDensity() : 0.12f;
}
void OhaoViewport::set_underwater_chrom_strength(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterChromStrength(v);
}
float OhaoViewport::get_underwater_chrom_strength() const {
    if (!m_renderer) return 0.006f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getUnderwaterChromStrength() : 0.006f;
}
void OhaoViewport::set_underwater_distort_frequency(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterDistortFrequency(v);
}
void OhaoViewport::set_underwater_distort_speed(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setUnderwaterDistortSpeed(v);
}

// ===== Ripple Tuning =====

void OhaoViewport::set_water_ripple_damping(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterRippleDamping(v);
}
float OhaoViewport::get_water_ripple_damping() const {
    if (!m_renderer) return 0.005f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getWaterRippleDamping() : 0.005f;
}
void OhaoViewport::set_water_ripple_speed(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterRippleSpeed(v);
}
float OhaoViewport::get_water_ripple_speed() const {
    if (!m_renderer) return 8.0f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getWaterRippleSpeed() : 8.0f;
}

// ===== Caustics Scale =====

void OhaoViewport::set_caustics_scale(float v) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setCausticsScale(v);
}
float OhaoViewport::get_caustics_scale() const {
    if (!m_renderer) return 0.08f;
    auto* deferred = m_renderer->getDeferredRenderer();
    return deferred ? deferred->getCausticsScale() : 0.08f;
}

// ===== Water Grid LOD =====

void OhaoViewport::set_water_grid_resolution(int n) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->setWaterGridResolution(n);
}

// ===== Decals =====

void OhaoViewport::set_decals_enabled(bool v) {
    m_render_settings.setDecalsEnabled(v);
    m_render_settings.apply(m_renderer);
}
int OhaoViewport::add_decal(const Vector3& pos, const Vector3& normal, const Vector3& size,
                             const String& albedo_path, float opacity, const Color& tint) {
    if (!m_renderer) return 0;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return 0;
    uint32_t handle = deferred->addDecal(
        glm::vec3(pos.x, pos.y, pos.z),
        glm::vec3(normal.x, normal.y, normal.z),
        glm::vec3(size.x, size.y, size.z),
        SceneSync::resolveResPath(albedo_path),
        opacity,
        glm::vec4(tint.r, tint.g, tint.b, tint.a));
    return static_cast<int>(handle);
}
void OhaoViewport::remove_decal(int handle) {
    if (!m_renderer || handle <= 0) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->removeDecal(static_cast<uint32_t>(handle));
}
void OhaoViewport::clear_decals() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->clearDecals();
}

// ===== Foliage =====

void OhaoViewport::set_foliage_enabled(bool v) {
    m_render_settings.setFoliageEnabled(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_foliage_cull_distance(float v) {
    m_render_settings.setFoliageCullDistance(v);
    m_render_settings.apply(m_renderer);
}
void OhaoViewport::set_grass_texture(const String& path) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    deferred->setGrassTexturePath(SceneSync::resolveResPath(path));
}
void OhaoViewport::add_foliage_cluster(const Dictionary& params) {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;
    Vector3 center = params.has("center") ? params["center"].operator Vector3() : Vector3(0, 0, 0);
    float radius   = params.has("radius")  ? static_cast<float>(params["radius"].operator double())  : 50.0f;
    float density  = params.has("density") ? static_cast<float>(params["density"].operator double()) : 30.0f;
    deferred->addFoliageCluster(
        glm::vec3(center.x, center.y, center.z),
        radius,
        density);
}
void OhaoViewport::clear_foliage() {
    if (!m_renderer) return;
    auto* deferred = m_renderer->getDeferredRenderer();
    if (deferred) deferred->clearFoliage();
}

// ===== Scene Management (delegates to SceneSync) =====

void OhaoViewport::load_tscn(const String& path) { m_scene_sync.loadTscn(path, m_scene, m_renderer); }

void OhaoViewport::sync_scene() {
    UtilityFunctions::print("[OHAO] sync_scene() called - use clear_scene(), add_* methods, and finish_sync() instead");
}

void OhaoViewport::clear_scene() {
    m_selection.clearSelection();
    m_scene_sync.clearScene(m_scene);
}

void OhaoViewport::sync_from_godot(Node* root_node) {
    m_selection.clearSelection();
    m_scene_sync.syncFromGodot(root_node, m_scene, m_renderer);
}

void OhaoViewport::add_cube(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    m_scene_sync.addCube(m_scene, name, position, rotation, scale, color);
}
void OhaoViewport::add_sphere(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    m_scene_sync.addSphere(m_scene, name, position, rotation, scale, color);
}
void OhaoViewport::add_plane(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    m_scene_sync.addPlane(m_scene, name, position, rotation, scale, color);
}
void OhaoViewport::add_cylinder(const String& name, const Vector3& position, const Vector3& rotation, const Vector3& scale, const Color& color) {
    m_scene_sync.addCylinder(m_scene, name, position, rotation, scale, color);
}
void OhaoViewport::add_directional_light(const String& name, const Vector3& position, const Vector3& direction, const Color& color, float intensity) {
    m_scene_sync.addDirectionalLight(m_scene, name, position, direction, color, intensity);
}
void OhaoViewport::add_point_light(const String& name, const Vector3& position, const Color& color, float intensity, float range) {
    m_scene_sync.addPointLight(m_scene, name, position, color, intensity, range);
}
void OhaoViewport::finish_sync() {
    m_scene_sync.finishSync(m_renderer);
    // Eagerly create backend physics bodies so get_actor_body_handle() returns
    // valid handles immediately after finish_sync() — required for constraints.
    if (m_scene) {
        auto* physWorld = m_scene->getPhysicsWorld();
        if (physWorld && physWorld->hasBackend()) {
            physWorld->flushPendingBodies();
            UtilityFunctions::print("[OHAO] finish_sync: physics bodies synced to backend");
        }
    }
}

// ===== Camera (delegates to CameraController) =====

void OhaoViewport::set_camera_mode(int mode) {
    if (m_renderer) {
        m_camera.setMode(mode, m_renderer->getCamera());
        UtilityFunctions::print("[OHAO] Camera mode: ", mode == 0 ? "FPS" : "Orbit");
    }
}

void OhaoViewport::focus_on_scene() {
    if (m_renderer) {
        m_camera.focusOnScene(m_scene, m_renderer->getCamera());
    }
}

// ===== Camera Position/Rotation (for game mode) =====

void OhaoViewport::set_camera_position(const Vector3& pos) {
    if (!m_renderer) return;
    m_renderer->getCamera().setPosition(glm::vec3(pos.x, pos.y, pos.z));
}

Vector3 OhaoViewport::get_camera_position() const {
    if (!m_renderer) return Vector3();
    glm::vec3 p = m_renderer->getCamera().getPosition();
    return Vector3(p.x, p.y, p.z);
}

void OhaoViewport::set_camera_rotation_deg(float pitch, float yaw) {
    if (!m_renderer) return;
    m_renderer->getCamera().setRotation(pitch, yaw);
}

Vector3 OhaoViewport::get_camera_forward() const {
    if (!m_renderer) return Vector3(0, 0, -1);
    glm::vec3 f = m_renderer->getCamera().getFront();
    return Vector3(f.x, f.y, f.z);
}

// ===== Input Mode =====

void OhaoViewport::set_input_mode(int mode) {
    bool entering_game = (mode == 1);
    if (entering_game == m_game_mode) return;
    m_game_mode = entering_game;

    if (m_game_mode) {
        // Entering GAME mode
        m_selection.clearSelection();
        m_camera.clearMovementState();
        m_picking_enabled = false;
        set_mouse_filter(MOUSE_FILTER_IGNORE);  // Transparent to mouse — events flow to _unhandled_input
        if (m_renderer) {
            ohao::DeferredRenderer* def = m_renderer->getDeferredRenderer();
            if (def) {
                def->setGridEnabled(false);
                def->setGizmoEnabled(false);
            }
        }
    } else {
        // Entering EDITOR mode
        m_picking_enabled = true;
        m_camera.clearMovementState();
        set_mouse_filter(MOUSE_FILTER_STOP);  // Capture mouse events for editor controls
        if (m_renderer) {
            ohao::DeferredRenderer* def = m_renderer->getDeferredRenderer();
            if (def) {
                def->setGridEnabled(m_grid_enabled);
            }
        }
    }
    queue_redraw();
}

// ===== Picking (delegates to SelectionController) =====

void OhaoViewport::pick_object_at(const Vector2& screen_pos) {
    m_selection.pickObjectAt(screen_pos, get_size(), m_width, m_height, m_renderer, m_scene);

    if (m_selection.hasSelection()) {
        emit_signal("actor_selected", m_selection.getSelectedActorName());
        UtilityFunctions::print("[OHAO] Selected: ", m_selection.getSelectedActorName());
    }

    queue_redraw();
}

// ===== Physics Controls (delegates to PhysicsController) =====

void OhaoViewport::play_physics()              { m_physics.play(m_scene); }
void OhaoViewport::pause_physics()             { m_physics.pause(m_scene); }
void OhaoViewport::step_physics()              { m_physics.step(m_scene); }
void OhaoViewport::stop_physics()              { m_physics.stop(m_scene); }
void OhaoViewport::set_physics_speed(float s)  { m_physics.setSpeed(s); }

void OhaoViewport::set_gravity(Vector3 gravity) {
    if (m_scene) {
        auto* world = m_scene->getPhysicsWorld();
        if (world) world->setGravity(glm::vec3(gravity.x, gravity.y, gravity.z));
    }
}

Vector3 OhaoViewport::get_gravity() const {
    if (m_scene) {
        auto* world = m_scene->getPhysicsWorld();
        if (world) {
            auto g = world->getGravity();
            return Vector3(g.x, g.y, g.z);
        }
    }
    return Vector3(0.0f, -9.81f, 0.0f);
}

// ===== Raycasting (delegates to PhysicsController) =====

Dictionary OhaoViewport::cast_ray(const Vector3& origin, const Vector3& direction, float max_distance, int layer_mask) {
    auto hit = m_physics.castRay(m_scene, glm::vec3(origin.x, origin.y, origin.z),
                                  glm::vec3(direction.x, direction.y, direction.z),
                                  max_distance, static_cast<uint16_t>(layer_mask));
    Dictionary result;
    if (hit.hit) {
        result["hit"] = true;
        result["position"] = Vector3(hit.position.x, hit.position.y, hit.position.z);
        result["normal"] = Vector3(hit.normal.x, hit.normal.y, hit.normal.z);
        result["fraction"] = hit.fraction;
        result["body_handle"] = static_cast<int>(hit.bodyHandle);
        result["layer"] = static_cast<int>(hit.layer);
    } else {
        result["hit"] = false;
    }
    return result;
}

Array OhaoViewport::cast_ray_all(const Vector3& origin, const Vector3& direction, float max_distance, int layer_mask) {
    auto hits = m_physics.castRayAll(m_scene, glm::vec3(origin.x, origin.y, origin.z),
                                      glm::vec3(direction.x, direction.y, direction.z),
                                      max_distance, static_cast<uint16_t>(layer_mask));
    Array results;
    for (const auto& h : hits) {
        Dictionary entry;
        entry["position"] = Vector3(h.position.x, h.position.y, h.position.z);
        entry["normal"] = Vector3(h.normal.x, h.normal.y, h.normal.z);
        entry["fraction"] = h.fraction;
        entry["body_handle"] = static_cast<int>(h.bodyHandle);
        entry["layer"] = static_cast<int>(h.layer);
        results.push_back(entry);
    }
    return results;
}

Array OhaoViewport::overlap_sphere(const Vector3& center, float radius, int layer_mask) {
    auto handles = m_physics.overlapSphere(m_scene, glm::vec3(center.x, center.y, center.z),
                                            radius, static_cast<uint16_t>(layer_mask));
    Array results;
    for (auto h : handles) results.push_back(static_cast<int>(h));
    return results;
}

Array OhaoViewport::overlap_box(const Vector3& center, const Vector3& half_extents, const Vector3& rotation_deg, int layer_mask) {
    glm::vec3 radians = glm::radians(glm::vec3(rotation_deg.x, rotation_deg.y, rotation_deg.z));
    auto handles = m_physics.overlapBox(m_scene, glm::vec3(center.x, center.y, center.z),
                                         glm::vec3(half_extents.x, half_extents.y, half_extents.z),
                                         glm::quat(radians), static_cast<uint16_t>(layer_mask));
    Array results;
    for (auto h : handles) results.push_back(static_cast<int>(h));
    return results;
}

// ===== Collision Layers (delegates to PhysicsController) =====

void OhaoViewport::set_layer_collision(int layer1, int layer2, bool should_collide) {
    m_physics.setLayerCollision(m_scene, static_cast<uint16_t>(layer1), static_cast<uint16_t>(layer2), should_collide);
}

// ===== Constraints (delegates to PhysicsController) =====

int OhaoViewport::create_constraint_fixed(int b1, int b2, const Vector3& anchor) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintFixed(m_scene, static_cast<uint32_t>(b1), h2, glm::vec3(anchor.x, anchor.y, anchor.z));
}

int OhaoViewport::create_constraint_hinge(int b1, int b2, const Vector3& anchor, const Vector3& axis, float lmin, float lmax) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintHinge(m_scene, static_cast<uint32_t>(b1), h2,
        glm::vec3(anchor.x, anchor.y, anchor.z), glm::vec3(axis.x, axis.y, axis.z), lmin, lmax);
}

int OhaoViewport::create_constraint_slider(int b1, int b2, const Vector3& axis, float lmin, float lmax) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintSlider(m_scene, static_cast<uint32_t>(b1), h2,
        glm::vec3(axis.x, axis.y, axis.z), lmin, lmax);
}

int OhaoViewport::create_constraint_point(int b1, int b2, const Vector3& anchor1, const Vector3& anchor2) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintPoint(m_scene, static_cast<uint32_t>(b1), h2,
        glm::vec3(anchor1.x, anchor1.y, anchor1.z), glm::vec3(anchor2.x, anchor2.y, anchor2.z));
}

int OhaoViewport::create_constraint_distance(int b1, int b2, const Vector3& anchor1, const Vector3& anchor2, float mind, float maxd) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintDistance(m_scene, static_cast<uint32_t>(b1), h2,
        glm::vec3(anchor1.x, anchor1.y, anchor1.z), glm::vec3(anchor2.x, anchor2.y, anchor2.z), mind, maxd);
}

int OhaoViewport::create_constraint_cone(int b1, int b2, const Vector3& anchor, const Vector3& twist_axis, float half_cone_angle) {
    uint32_t h2 = b2 < 0 ? ohao::physics::backend::INVALID_BODY : static_cast<uint32_t>(b2);
    return m_physics.createConstraintCone(m_scene, static_cast<uint32_t>(b1), h2,
        glm::vec3(anchor.x, anchor.y, anchor.z), glm::vec3(twist_axis.x, twist_axis.y, twist_axis.z), half_cone_angle);
}

void OhaoViewport::destroy_constraint(int h)                               { m_physics.destroyConstraint(m_scene, static_cast<uint32_t>(h)); }
void OhaoViewport::set_constraint_enabled(int h, bool e)                   { m_physics.setConstraintEnabled(m_scene, static_cast<uint32_t>(h), e); }
void OhaoViewport::set_constraint_motor(int h, bool e, float s, float f)   { m_physics.setConstraintMotor(m_scene, static_cast<uint32_t>(h), e, s, f); }
void OhaoViewport::set_constraint_limits(int h, float mn, float mx)        { m_physics.setConstraintLimits(m_scene, static_cast<uint32_t>(h), mn, mx); }

void OhaoViewport::set_constraint_breaking(int h, float max_force, float max_torque) {
    m_physics.setConstraintBreaking(m_scene, static_cast<uint32_t>(h), max_force, max_torque);
}

Array OhaoViewport::get_and_clear_broken_constraints() {
    Array result;
    auto broken = m_physics.getAndClearBrokenConstraints(m_scene);
    for (uint32_t handle : broken) {
        result.append(static_cast<int>(handle));
    }
    return result;
}

// ===== Physics Grab/Throw =====

int OhaoViewport::grab_body(int body_handle, const Vector3& world_pos) {
    glm::vec3 pos(world_pos.x, world_pos.y, world_pos.z);
    return m_physics.grabBody(m_scene, static_cast<uint32_t>(body_handle), pos);
}

void OhaoViewport::move_grab(int token, const Vector3& world_pos) {
    glm::vec3 pos(world_pos.x, world_pos.y, world_pos.z);
    m_physics.moveGrab(m_scene, token, pos);
}

void OhaoViewport::release_grab(int token) {
    m_physics.releaseGrab(m_scene, token);
}

void OhaoViewport::throw_grab(int token, const Vector3& velocity) {
    glm::vec3 vel(velocity.x, velocity.y, velocity.z);
    m_physics.throwGrab(m_scene, token, vel);
}

// ===== Character Controller (delegates to PhysicsController) =====

int OhaoViewport::create_character(const Vector3& position, float capsule_radius, float capsule_height,
                                    float max_slope_deg, float mass) {
    return m_physics.createCharacter(m_scene, glm::vec3(position.x, position.y, position.z),
                                      capsule_radius, capsule_height, max_slope_deg, mass);
}

void OhaoViewport::destroy_character(int h) { m_physics.destroyCharacter(m_scene, static_cast<uint32_t>(h)); }

Dictionary OhaoViewport::get_character_state(int char_handle) {
    auto state = m_physics.getCharacterState(m_scene, static_cast<uint32_t>(char_handle));
    Dictionary result;
    result["position"] = Vector3(state.position.x, state.position.y, state.position.z);
    result["velocity"] = Vector3(state.velocity.x, state.velocity.y, state.velocity.z);
    result["ground_normal"] = Vector3(state.groundNormal.x, state.groundNormal.y, state.groundNormal.z);
    result["is_grounded"] = (state.groundState == 0);
    result["is_on_steep_ground"] = (state.groundState == 1);
    result["is_in_air"] = (state.groundState == 3);
    result["ground_state"] = state.groundState;
    result["ground_body"] = state.groundBody;
    return result;
}

void OhaoViewport::set_character_position(int h, const Vector3& p) {
    m_physics.setCharacterPosition(m_scene, static_cast<uint32_t>(h), glm::vec3(p.x, p.y, p.z));
}

void OhaoViewport::set_character_rotation(int h, const Vector3& r) {
    glm::vec3 rad = glm::radians(glm::vec3(r.x, r.y, r.z));
    m_physics.setCharacterRotation(m_scene, static_cast<uint32_t>(h), glm::quat(rad));
}

void OhaoViewport::set_character_velocity(int h, const Vector3& v) {
    m_physics.setCharacterVelocity(m_scene, static_cast<uint32_t>(h), glm::vec3(v.x, v.y, v.z));
}

void OhaoViewport::update_character(int h, float delta, const Vector3& gravity, const Vector3& movement_input) {
    m_physics.updateCharacter(m_scene, static_cast<uint32_t>(h), delta,
        glm::vec3(gravity.x, gravity.y, gravity.z), glm::vec3(movement_input.x, movement_input.y, movement_input.z));
}

// ===== Wireframe / Grid / Gizmo / Particles / Import =====

void OhaoViewport::set_wireframe_enabled(bool enabled) {
    m_wireframe_enabled = enabled;
    if (m_renderer) {
        ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
        if (deferred) deferred->setWireframeEnabled(enabled);
    }
}

void OhaoViewport::set_grid_enabled(bool enabled) {
    m_grid_enabled = enabled;
    if (m_renderer) {
        ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
        if (deferred) deferred->setGridEnabled(enabled);
    }
}

void OhaoViewport::import_model(const String& path) {
    m_scene_sync.importModel(m_scene, m_renderer, std::string(path.utf8().get_data()));
}

void OhaoViewport::set_gizmo_mode(int mode) {
    m_gizmo_mode = mode;
    if (m_renderer) {
        ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
        if (deferred) deferred->setGizmoMode(static_cast<ohao::GizmoMode>(mode));
    }
}

void OhaoViewport::set_gizmo_enabled(bool enabled) {
    m_gizmo_enabled = enabled;
    if (m_renderer) {
        ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
        if (deferred) deferred->setGizmoEnabled(enabled && m_selection.hasSelection());
    }
}

void OhaoViewport::spawn_particles(const Vector3& position, int type) {
    if (!m_renderer) return;
    ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
    if (deferred) {
        deferred->spawnParticles(
            glm::vec3(position.x, position.y, position.z),
            static_cast<ohao::ParticleType>(type));
    }
}

void OhaoViewport::spawn_particles_directed(const Vector3& position, int type, const Vector3& direction) {
    if (!m_renderer) return;
    ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
    if (deferred) {
        deferred->spawnParticles(
            glm::vec3(position.x, position.y, position.z),
            static_cast<ohao::ParticleType>(type),
            glm::vec3(direction.x, direction.y, direction.z));
    }
}

// ===== Actor Transform API (delegates to ActorController) =====

void OhaoViewport::set_actor_position(const String& actor_name, const Vector3& position) {
    m_actors.setPosition(m_scene, actor_name.utf8().get_data(), glm::vec3(position.x, position.y, position.z));
}

void OhaoViewport::set_actor_rotation(const String& actor_name, const Vector3& rotation_deg) {
    m_actors.setRotation(m_scene, actor_name.utf8().get_data(), glm::vec3(rotation_deg.x, rotation_deg.y, rotation_deg.z));
}

void OhaoViewport::set_actor_scale(const String& actor_name, const Vector3& scale) {
    m_actors.setScale(m_scene, actor_name.utf8().get_data(), glm::vec3(scale.x, scale.y, scale.z));
}

// ===== Actor Lifecycle (delegates to ActorController) =====

void OhaoViewport::remove_actor(const String& actor_name) {
    m_actors.removeActor(m_scene, m_renderer, actor_name.utf8().get_data());
}

bool OhaoViewport::has_actor(const String& actor_name) const {
    return m_actors.hasActor(m_scene, actor_name.utf8().get_data());
}

// ===== Texture / Material API (delegates to ActorController) =====

void OhaoViewport::set_actor_texture(const String& actor_name, const String& texture_path) {
    m_actors.setTexture(m_scene, m_renderer, actor_name.utf8().get_data(), SceneSync::resolveResPath(texture_path));
}

void OhaoViewport::set_actor_normal_map(const String& actor_name, const String& normal_path) {
    m_actors.setNormalMap(m_scene, m_renderer, actor_name.utf8().get_data(), SceneSync::resolveResPath(normal_path));
}

void OhaoViewport::set_actor_pbr(const String& actor_name, float metallic, float roughness) {
    m_actors.setPBR(m_scene, actor_name.utf8().get_data(), metallic, roughness);
}

void OhaoViewport::set_actor_material_preset(const String& actor_name, const String& preset_name) {
    UtilityFunctions::print("[OHAO] set_actor_material_preset: use OhaoPresets.apply_material() from GDScript");
}

// ===== Actor Physics API (delegates to ActorController) =====

int OhaoViewport::get_actor_body_handle(const String& actor_name)                      { return m_actors.getBodyHandle(m_scene, actor_name.utf8().get_data()); }
void OhaoViewport::set_actor_body_type(const String& actor_name, int type)              { m_actors.setBodyType(m_scene, actor_name.utf8().get_data(), type); }
void OhaoViewport::set_actor_mass(const String& actor_name, float mass)                 { m_actors.setMass(m_scene, actor_name.utf8().get_data(), mass); }
void OhaoViewport::set_actor_restitution(const String& actor_name, float restitution)   { m_actors.setRestitution(m_scene, actor_name.utf8().get_data(), restitution); }
void OhaoViewport::set_actor_friction(const String& actor_name, float friction)         { m_actors.setFriction(m_scene, actor_name.utf8().get_data(), friction); }
void OhaoViewport::set_actor_gravity_enabled(const String& actor_name, bool enabled)    { m_actors.setGravityEnabled(m_scene, actor_name.utf8().get_data(), enabled); }
void OhaoViewport::set_actor_gravity_scale(const String& actor_name, float scale)        { m_actors.setGravityScale(m_scene, actor_name.utf8().get_data(), scale); }
void OhaoViewport::set_actor_linear_damping(const String& actor_name, float damping)    { m_actors.setLinearDamping(m_scene, actor_name.utf8().get_data(), damping); }
void OhaoViewport::set_actor_angular_damping(const String& actor_name, float damping)   { m_actors.setAngularDamping(m_scene, actor_name.utf8().get_data(), damping); }

void OhaoViewport::apply_radial_impulse(const Vector3& center, float strength, float radius, int falloff) {
    m_actors.applyRadialImpulse(m_scene, glm::vec3(center.x, center.y, center.z), strength, radius, falloff);
}

void OhaoViewport::set_actor_linear_velocity(const String& actor_name, const Vector3& velocity) {
    m_actors.setLinearVelocity(m_scene, actor_name.utf8().get_data(), glm::vec3(velocity.x, velocity.y, velocity.z));
}

// === World Force Effects ===

static ohao::physics::PhysicsWorld* getPhysWorld(ohao::Scene* scene) {
    if (!scene) return nullptr;
    auto* w = scene->getPhysicsWorld();
    return (w && w->hasBackend()) ? w : nullptr;
}

void OhaoViewport::set_wind(const Vector3& direction, float strength, float turbulence) {
    if (auto* w = getPhysWorld(m_scene)) w->setWind(glm::vec3(direction.x, direction.y, direction.z), strength, turbulence);
}
void OhaoViewport::clear_wind() {
    if (auto* w = getPhysWorld(m_scene)) w->clearWind();
}
void OhaoViewport::set_water(float liquid_level, float density, float drag) {
    if (auto* w = getPhysWorld(m_scene)) w->setWater(liquid_level, density, drag);
}
void OhaoViewport::clear_water() {
    if (auto* w = getPhysWorld(m_scene)) w->clearWater();
}
int OhaoViewport::create_force_volume_box(const Vector3& center, const Vector3& half_extents, const Vector3& force) {
    auto* w = getPhysWorld(m_scene);
    if (!w) return -1;
    return w->createForceVolumeBox(
        glm::vec3(center.x, center.y, center.z),
        glm::vec3(half_extents.x, half_extents.y, half_extents.z),
        glm::vec3(force.x, force.y, force.z));
}
int OhaoViewport::create_force_volume_sphere(const Vector3& center, float radius, const Vector3& force) {
    auto* w = getPhysWorld(m_scene);
    if (!w) return -1;
    return w->createForceVolumeSphere(
        glm::vec3(center.x, center.y, center.z), radius,
        glm::vec3(force.x, force.y, force.z));
}
void OhaoViewport::destroy_force_volume(int handle) {
    if (auto* w = getPhysWorld(m_scene)) w->destroyForceVolume(handle);
}
void OhaoViewport::set_force_volume_enabled(int handle, bool enabled) {
    if (auto* w = getPhysWorld(m_scene)) w->setForceVolumeEnabled(handle, enabled);
}

void OhaoViewport::sync_actor_physics_shape(const String& actor_name) {
    m_actors.syncPhysicsShape(m_scene, actor_name.utf8().get_data());
}

// === Apply Forces by Actor Name ===
void OhaoViewport::apply_force_on_actor(const String& n, const Vector3& f, const Vector3& r) {
    m_actors.applyForce(m_scene, n.utf8().get_data(), glm::vec3(f.x,f.y,f.z), glm::vec3(r.x,r.y,r.z));
}
void OhaoViewport::apply_impulse_on_actor(const String& n, const Vector3& i, const Vector3& r) {
    m_actors.applyImpulse(m_scene, n.utf8().get_data(), glm::vec3(i.x,i.y,i.z), glm::vec3(r.x,r.y,r.z));
}
void OhaoViewport::apply_torque_on_actor(const String& n, const Vector3& t) {
    m_actors.applyTorque(m_scene, n.utf8().get_data(), glm::vec3(t.x,t.y,t.z));
}

// === Velocity / Mass Getters ===
Vector3 OhaoViewport::get_actor_linear_velocity(const String& n) {
    auto v = m_actors.getLinearVelocity(m_scene, n.utf8().get_data());
    return Vector3(v.x, v.y, v.z);
}
Vector3 OhaoViewport::get_actor_angular_velocity(const String& n) {
    auto v = m_actors.getAngularVelocity(m_scene, n.utf8().get_data());
    return Vector3(v.x, v.y, v.z);
}
float OhaoViewport::get_actor_mass(const String& n) {
    return m_actors.getMass(m_scene, n.utf8().get_data());
}

// === CCD ===
void OhaoViewport::set_actor_ccd_enabled(const String& n, bool enabled) {
    m_actors.setCCDEnabled(m_scene, n.utf8().get_data(), enabled);
}

// === Axis Lock ===
void OhaoViewport::set_actor_freeze_linear(const String& n, bool x, bool y, bool z) {
    uint8_t mask = (x ? 1 : 0) | (y ? 2 : 0) | (z ? 4 : 0);
    m_actors.setFreezeLinearAxes(m_scene, n.utf8().get_data(), mask);
}
void OhaoViewport::set_actor_freeze_rotation(const String& n, bool x, bool y, bool z) {
    uint8_t mask = (x ? 1 : 0) | (y ? 2 : 0) | (z ? 4 : 0);
    m_actors.setFreezeRotationalAxes(m_scene, n.utf8().get_data(), mask);
}

// === Sleep / Wake ===
void OhaoViewport::set_actor_awake(const String& actor_name, bool awake) {
    m_actors.setAwake(m_scene, actor_name.utf8().get_data(), awake);
}
bool OhaoViewport::is_actor_awake(const String& actor_name) {
    return m_actors.isAwake(m_scene, actor_name.utf8().get_data());
}

// === Collision Layer Assignment ===
void OhaoViewport::set_actor_layer(const String& actor_name, int layer) {
    m_actors.setLayer(m_scene, actor_name.utf8().get_data(), static_cast<uint16_t>(layer));
}

// === Springs ===
// Helper to find a RigidBody* by actor name
static ohao::physics::dynamics::RigidBody* findRigidBody(ohao::Scene* scene, const std::string& name) {
    if (!scene) return nullptr;
    auto actor = scene->findActor(name);
    if (!actor) return nullptr;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (!phys) return nullptr;
    return phys->getRigidBody().get();
}

int OhaoViewport::create_spring(const String& body1, const String& body2,
                                 float stiffness, float rest_length, float damping) {
    auto* w = getPhysWorld(m_scene);
    if (!w) return -1;
    auto* rb1 = findRigidBody(m_scene, body1.utf8().get_data());
    auto* rb2 = findRigidBody(m_scene, body2.utf8().get_data());
    if (!rb1 || !rb2) return -1;
    return w->createSpring(rb1, rb2, stiffness, rest_length, damping);
}

int OhaoViewport::create_anchor_spring(const String& body_name, const Vector3& anchor,
                                        float stiffness, float rest_length, float damping) {
    auto* w = getPhysWorld(m_scene);
    if (!w) return -1;
    auto* rb = findRigidBody(m_scene, body_name.utf8().get_data());
    if (!rb) return -1;
    return w->createAnchorSpring(rb, glm::vec3(anchor.x, anchor.y, anchor.z),
                                  stiffness, rest_length, damping);
}

void OhaoViewport::destroy_spring(int handle) {
    if (auto* w = getPhysWorld(m_scene)) w->destroySpring(handle);
}
void OhaoViewport::set_spring_enabled(int handle, bool enabled) {
    if (auto* w = getPhysWorld(m_scene)) w->setSpringEnabled(handle, enabled);
}

// ===== Audio =====

int OhaoViewport::play_sound(const String& path, int category, bool loop, float volume) {
    return static_cast<int>(m_audio.playSound(path, category, loop, volume));
}

int OhaoViewport::play_sound_at(const String& path, const Vector3& position, int category, bool loop, float volume) {
    return static_cast<int>(m_audio.playSoundAt(path, position, category, loop, volume));
}

void OhaoViewport::stop_sound(int handle) { m_audio.stopSound(static_cast<uint32_t>(handle)); }
void OhaoViewport::pause_sound(int handle) { m_audio.pauseSound(static_cast<uint32_t>(handle)); }
void OhaoViewport::resume_sound(int handle) { m_audio.resumeSound(static_cast<uint32_t>(handle)); }
void OhaoViewport::set_sound_volume(int handle, float volume) { m_audio.setSoundVolume(static_cast<uint32_t>(handle), volume); }
void OhaoViewport::set_sound_position(int handle, const Vector3& position) { m_audio.setSoundPosition(static_cast<uint32_t>(handle), position); }

void OhaoViewport::set_category_volume(int category, float volume) { m_audio.setCategoryVolume(category, volume); }
float OhaoViewport::get_category_volume(int category) const { return m_audio.getCategoryVolume(category); }
void OhaoViewport::stop_category(int category) { m_audio.stopCategory(category); }
void OhaoViewport::pause_category(int category) { m_audio.pauseCategory(category); }
void OhaoViewport::resume_category(int category) { m_audio.resumeCategory(category); }

void OhaoViewport::set_master_volume(float volume) { m_audio.setMasterVolume(volume); }
float OhaoViewport::get_master_volume() const { return m_audio.getMasterVolume(); }
void OhaoViewport::stop_all_sounds() { m_audio.stopAll(); }

// ===== Utility =====

Dictionary OhaoViewport::get_render_stats() const {
    Dictionary stats;
    stats["initialized"] = m_initialized;
    stats["width"] = m_width;
    stats["height"] = m_height;
    stats["render_mode"] = m_render_settings.getRenderMode() == 1 ? "Deferred" : "Forward";
    stats["bloom_enabled"] = m_render_settings.getBloomEnabled();
    stats["taa_enabled"] = m_render_settings.getTAAEnabled();
    stats["ssao_enabled"] = m_render_settings.getSSAOEnabled();
    stats["ssr_enabled"] = m_render_settings.getSSREnabled();
    stats["volumetrics_enabled"] = m_render_settings.getVolumetricsEnabled();
    stats["motion_blur_enabled"] = m_render_settings.getMotionBlurEnabled();
    stats["dof_enabled"] = m_render_settings.getDoFEnabled();
    stats["tonemapping_enabled"] = m_render_settings.getTonemappingEnabled();
    stats["synced_objects"] = m_scene_sync.getSyncedObjectCount();
    return stats;
}

} // namespace godot
