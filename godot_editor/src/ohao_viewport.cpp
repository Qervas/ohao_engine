#include "ohao_viewport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/font.hpp>

// Godot 3D node types for scene sync
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/primitive_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

// Include OHAO headers
#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "renderer/passes/post_processing_pipeline.hpp"
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
}

OhaoViewport::OhaoViewport() {
    UtilityFunctions::print("[OHAO] AAA Viewport created");
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
    UtilityFunctions::print("[OHAO] AAA Viewport ready - initializing deferred renderer");

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

    // Draw "OHAO Engine" text overlay when scene is empty
    if (!has_scene_meshes()) {
        Ref<Font> font = get_theme_default_font();
        if (font.is_valid()) {
            String text = "OHAO AAA Engine";
            int font_size = 32;
            Vector2 text_size = font->get_string_size(text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size);
            Vector2 center = get_size() / 2.0;
            Vector2 text_pos = center - text_size / 2.0;
            text_pos.y += text_size.y / 2.0;  // Adjust for baseline

            // Draw text with slight transparency
            draw_string(font, text_pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.6, 0.6, 0.7, 0.8));

            // Draw subtitle
            String subtitle = m_render_mode == 1 ? "Deferred Rendering Mode" : "Forward Rendering Mode";
            int sub_size = 14;
            Vector2 sub_text_size = font->get_string_size(subtitle, HORIZONTAL_ALIGNMENT_CENTER, -1, sub_size);
            Vector2 sub_pos = center - sub_text_size / 2.0;
            sub_pos.y += text_size.y / 2.0 + 30;
            draw_string(font, sub_pos, subtitle, HORIZONTAL_ALIGNMENT_LEFT, -1, sub_size, Color(0.5, 0.5, 0.55, 0.6));

            // Draw hint
            String hint = "Load a scene or sync from editor";
            Vector2 hint_size = font->get_string_size(hint, HORIZONTAL_ALIGNMENT_CENTER, -1, sub_size);
            Vector2 hint_pos = center - hint_size / 2.0;
            hint_pos.y += text_size.y / 2.0 + 50;
            draw_string(font, hint_pos, hint, HORIZONTAL_ALIGNMENT_LEFT, -1, sub_size, Color(0.4, 0.4, 0.45, 0.5));
        }
    }
}

bool OhaoViewport::has_scene_meshes() const {
    if (!m_renderer) return false;
    return m_renderer->hasSceneMeshes();
}

void OhaoViewport::initialize_renderer() {
    if (m_initialized) {
        return;
    }

    UtilityFunctions::print("[OHAO] Initializing Vulkan AAA renderer...");

    // Create offscreen renderer
    m_renderer = new ohao::OffscreenRenderer(m_width, m_height);
    if (!m_renderer->initialize()) {
        UtilityFunctions::printerr("[OHAO] Failed to initialize renderer!");
        delete m_renderer;
        m_renderer = nullptr;
        return;
    }

    // Set render mode (default to Deferred)
    m_renderer->setRenderMode(m_render_mode == 1 ? ohao::RenderMode::Deferred : ohao::RenderMode::Forward);

    // Create scene
    m_scene = new ohao::Scene("GodotScene");
    m_renderer->setScene(m_scene);

    // Create Godot image and texture
    m_image = Image::create(m_width, m_height, false, Image::FORMAT_RGBA8);
    m_texture = ImageTexture::create_from_image(m_image);

    m_initialized = true;

    // Apply initial render settings
    apply_render_settings();

    UtilityFunctions::print("[OHAO] AAA Renderer initialized: ", m_width, "x", m_height,
                            " Mode: ", m_render_mode == 1 ? "Deferred" : "Forward");
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

// === AAA Render Settings Implementation ===

void OhaoViewport::apply_render_settings() {
    if (!m_renderer) return;

    // Get the deferred renderer's post-processing pipeline
    ohao::DeferredRenderer* deferred = m_renderer->getDeferredRenderer();
    if (!deferred) return;

    ohao::PostProcessingPipeline* pp = deferred->getPostProcessing();
    if (!pp) return;

    // Apply all post-processing settings
    pp->setBloomEnabled(m_bloom_enabled);
    pp->setTAAEnabled(m_taa_enabled);
    pp->setSSAOEnabled(m_ssao_enabled);
    pp->setSSREnabled(m_ssr_enabled);
    pp->setVolumetricsEnabled(m_volumetrics_enabled);
    pp->setMotionBlurEnabled(m_motion_blur_enabled);
    pp->setDoFEnabled(m_dof_enabled);
    pp->setTonemappingEnabled(m_tonemapping_enabled);

    // Tonemapping
    pp->setTonemapOperator(static_cast<ohao::TonemapOperator>(m_tonemap_operator));
    pp->setExposure(m_exposure);
    pp->setGamma(m_gamma);

    // Bloom
    pp->setBloomThreshold(m_bloom_threshold);
    pp->setBloomIntensity(m_bloom_intensity);

    // SSAO
    pp->setSSAORadius(m_ssao_radius);
    pp->setSSAOIntensity(m_ssao_intensity);

    // SSR
    pp->setSSRMaxDistance(m_ssr_max_distance);
    pp->setSSRThickness(m_ssr_thickness);

    // Volumetrics
    pp->setVolumetricDensity(m_volumetric_density);
    pp->setVolumetricScattering(m_volumetric_scattering);
    pp->setFogColor(glm::vec3(m_fog_color.r, m_fog_color.g, m_fog_color.b));

    // Motion blur
    pp->setMotionBlurIntensity(m_motion_blur_intensity);
    pp->setMotionBlurSamples(static_cast<uint32_t>(m_motion_blur_samples));

    // DoF
    pp->setDoFFocusDistance(m_dof_focus_distance);
    pp->setDoFAperture(m_dof_aperture);
    pp->setDoFMaxBlurRadius(m_dof_max_blur);

    // TAA
    pp->setTAABlendFactor(m_taa_blend_factor);
}

void OhaoViewport::set_render_mode(int mode) {
    m_render_mode = mode;
    if (m_renderer) {
        m_renderer->setRenderMode(mode == 1 ? ohao::RenderMode::Deferred : ohao::RenderMode::Forward);
        UtilityFunctions::print("[OHAO] Render mode set to: ", mode == 1 ? "Deferred" : "Forward");
    }
}

// Post-processing toggles
void OhaoViewport::set_bloom_enabled(bool enabled) {
    m_bloom_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_taa_enabled(bool enabled) {
    m_taa_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_ssao_enabled(bool enabled) {
    m_ssao_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_ssr_enabled(bool enabled) {
    m_ssr_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_volumetrics_enabled(bool enabled) {
    m_volumetrics_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_motion_blur_enabled(bool enabled) {
    m_motion_blur_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_dof_enabled(bool enabled) {
    m_dof_enabled = enabled;
    apply_render_settings();
}

void OhaoViewport::set_tonemapping_enabled(bool enabled) {
    m_tonemapping_enabled = enabled;
    apply_render_settings();
}

// Tonemapping settings
void OhaoViewport::set_tonemap_operator(int op) {
    m_tonemap_operator = op;
    apply_render_settings();
}

void OhaoViewport::set_exposure(float exposure) {
    m_exposure = exposure;
    apply_render_settings();
}

void OhaoViewport::set_gamma(float gamma) {
    m_gamma = gamma;
    apply_render_settings();
}

// Bloom settings
void OhaoViewport::set_bloom_threshold(float threshold) {
    m_bloom_threshold = threshold;
    apply_render_settings();
}

void OhaoViewport::set_bloom_intensity(float intensity) {
    m_bloom_intensity = intensity;
    apply_render_settings();
}

// SSAO settings
void OhaoViewport::set_ssao_radius(float radius) {
    m_ssao_radius = radius;
    apply_render_settings();
}

void OhaoViewport::set_ssao_intensity(float intensity) {
    m_ssao_intensity = intensity;
    apply_render_settings();
}

// SSR settings
void OhaoViewport::set_ssr_max_distance(float dist) {
    m_ssr_max_distance = dist;
    apply_render_settings();
}

void OhaoViewport::set_ssr_thickness(float thickness) {
    m_ssr_thickness = thickness;
    apply_render_settings();
}

// Volumetric settings
void OhaoViewport::set_volumetric_density(float density) {
    m_volumetric_density = density;
    apply_render_settings();
}

void OhaoViewport::set_volumetric_scattering(float g) {
    m_volumetric_scattering = g;
    apply_render_settings();
}

void OhaoViewport::set_fog_color(const Color& color) {
    m_fog_color = color;
    apply_render_settings();
}

// Motion blur settings
void OhaoViewport::set_motion_blur_intensity(float intensity) {
    m_motion_blur_intensity = intensity;
    apply_render_settings();
}

void OhaoViewport::set_motion_blur_samples(int samples) {
    m_motion_blur_samples = samples;
    apply_render_settings();
}

// DoF settings
void OhaoViewport::set_dof_focus_distance(float distance) {
    m_dof_focus_distance = distance;
    apply_render_settings();
}

void OhaoViewport::set_dof_aperture(float aperture) {
    m_dof_aperture = aperture;
    apply_render_settings();
}

void OhaoViewport::set_dof_max_blur(float blur) {
    m_dof_max_blur = blur;
    apply_render_settings();
}

// TAA settings
void OhaoViewport::set_taa_blend_factor(float factor) {
    m_taa_blend_factor = factor;
    apply_render_settings();
}

// Utility
Dictionary OhaoViewport::get_render_stats() const {
    Dictionary stats;
    stats["initialized"] = m_initialized;
    stats["width"] = m_width;
    stats["height"] = m_height;
    stats["render_mode"] = m_render_mode == 1 ? "Deferred" : "Forward";
    stats["bloom_enabled"] = m_bloom_enabled;
    stats["taa_enabled"] = m_taa_enabled;
    stats["ssao_enabled"] = m_ssao_enabled;
    stats["ssr_enabled"] = m_ssr_enabled;
    stats["volumetrics_enabled"] = m_volumetrics_enabled;
    stats["motion_blur_enabled"] = m_motion_blur_enabled;
    stats["dof_enabled"] = m_dof_enabled;
    stats["tonemapping_enabled"] = m_tonemapping_enabled;
    stats["synced_objects"] = m_synced_object_count;
    return stats;
}

// === Scene Management ===

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
    if (!m_scene) {
        UtilityFunctions::printerr("[OHAO] add_directional_light: scene is null!");
        return;
    }

    std::string actorName = name.utf8().get_data();
    UtilityFunctions::print("[OHAO] Adding directional light '", name, "' pos=(", position.x, ",", position.y, ",", position.z,
                            ") dir=(", direction.x, ",", direction.y, ",", direction.z, ") intensity=", intensity);

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
            UtilityFunctions::print("[OHAO] Light component configured successfully");
        } else {
            UtilityFunctions::printerr("[OHAO] Failed to get LightComponent from actor!");
        }

        // Verify actor was added to scene
        UtilityFunctions::print("[OHAO] Scene now has ", static_cast<int>(m_scene->getAllActors().size()), " actors");
    } else {
        UtilityFunctions::printerr("[OHAO] Failed to create directional light actor!");
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

// ===== Scene Sync from Godot =====

void OhaoViewport::sync_from_godot(Node* root_node) {
    if (!root_node) {
        UtilityFunctions::printerr("[OHAO] sync_from_godot: root_node is null!");
        return;
    }

    if (!m_scene || !m_renderer) {
        UtilityFunctions::printerr("[OHAO] sync_from_godot: renderer not initialized!");
        return;
    }

    UtilityFunctions::print("[OHAO] Syncing from Godot scene: ", root_node->get_name());

    // First pass: count objects without modifying scene
    m_synced_object_count = 0;
    count_syncable_objects(root_node);

    if (m_synced_object_count == 0) {
        UtilityFunctions::print("[OHAO] No syncable objects found in Godot scene (need MeshInstance3D, lights, etc.)");
        UtilityFunctions::print("[OHAO] Keeping existing OHAO scene. Add 3D objects in Godot's 3D editor first.");
        return;
    }

    // Clear existing OHAO scene only if we have objects to sync
    m_scene->removeAllActors();
    m_synced_object_count = 0;

    // Traverse the Godot scene tree and create OHAO actors
    traverse_and_sync(root_node);

    // Debug: List all actors in scene after sync
    UtilityFunctions::print("[OHAO] === Actors in scene after sync (scene ptr=", reinterpret_cast<uint64_t>(m_scene), ") ===");
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto lightComp = actor->getComponent<ohao::LightComponent>();
        if (lightComp) {
            auto pos = actor->getTransform()->getPosition();
            auto dir = lightComp->getDirection();
            UtilityFunctions::print("[OHAO]   LIGHT: '", actor->getName().c_str(), "' type=",
                                    static_cast<int>(lightComp->getLightType()),
                                    " pos=(", pos.x, ",", pos.y, ",", pos.z, ")",
                                    " dir=(", dir.x, ",", dir.y, ",", dir.z, ")");
        } else {
            UtilityFunctions::print("[OHAO]   Actor: '", actor->getName().c_str(), "'");
        }
    }
    UtilityFunctions::print("[OHAO] === End actor list ===");

    // Rebuild buffers
    if (m_renderer->updateSceneBuffers()) {
        UtilityFunctions::print("[OHAO] Sync complete: ", m_synced_object_count, " objects synced");
    } else {
        UtilityFunctions::print("[OHAO] Sync complete but no renderable meshes found");
    }
}

void OhaoViewport::count_syncable_objects(Node* node) {
    if (!node) return;

    // Check for syncable node types
    if (Object::cast_to<MeshInstance3D>(node)) {
        MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(node);
        if (mesh_instance->get_mesh().is_valid()) {
            m_synced_object_count++;
        }
    }
    else if (Object::cast_to<DirectionalLight3D>(node) ||
             Object::cast_to<OmniLight3D>(node) ||
             Object::cast_to<SpotLight3D>(node)) {
        m_synced_object_count++;
    }

    // Recurse into children
    int child_count = node->get_child_count();
    for (int i = 0; i < child_count; i++) {
        count_syncable_objects(node->get_child(i));
    }
}

void OhaoViewport::traverse_and_sync(Node* node) {
    if (!node) return;

    // Check if this is a Node3D (has transform)
    Node3D* node3d = Object::cast_to<Node3D>(node);
    if (node3d) {
        // Get global transform
        Transform3D transform = node3d->get_global_transform();
        Vector3 position = transform.origin;
        Vector3 rotation = transform.basis.get_euler();
        Vector3 scale = transform.basis.get_scale();
        String name = node->get_name();

        // Check for MeshInstance3D
        MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(node);
        if (mesh_instance) {
            Ref<Mesh> mesh = mesh_instance->get_mesh();
            if (mesh.is_valid()) {
                // Get material color if available
                Color color(0.8f, 0.8f, 0.8f, 1.0f);  // Default gray

                // Try to get material from mesh instance
                Ref<Material> mat = mesh_instance->get_surface_override_material(0);
                if (!mat.is_valid() && mesh.is_valid()) {
                    mat = mesh->surface_get_material(0);
                }

                if (mat.is_valid()) {
                    StandardMaterial3D* std_mat = Object::cast_to<StandardMaterial3D>(mat.ptr());
                    if (std_mat) {
                        color = std_mat->get_albedo();
                    }
                }

                // Determine mesh type and add to OHAO
                // Note: Multiply node scale by mesh size to get actual dimensions
                BoxMesh* box_mesh = Object::cast_to<BoxMesh>(mesh.ptr());
                SphereMesh* sphere_mesh = Object::cast_to<SphereMesh>(mesh.ptr());
                CylinderMesh* cylinder_mesh = Object::cast_to<CylinderMesh>(mesh.ptr());
                PlaneMesh* plane_mesh = Object::cast_to<PlaneMesh>(mesh.ptr());

                if (box_mesh) {
                    Vector3 mesh_size = box_mesh->get_size();
                    Vector3 final_scale = Vector3(scale.x * mesh_size.x, scale.y * mesh_size.y, scale.z * mesh_size.z);
                    add_cube(name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (sphere_mesh) {
                    float radius = sphere_mesh->get_radius();
                    Vector3 final_scale = Vector3(scale.x * radius * 2.0f, scale.y * radius * 2.0f, scale.z * radius * 2.0f);
                    add_sphere(name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (cylinder_mesh) {
                    float radius = cylinder_mesh->get_top_radius();
                    float height = cylinder_mesh->get_height();
                    Vector3 final_scale = Vector3(scale.x * radius * 2.0f, scale.y * height, scale.z * radius * 2.0f);
                    add_cylinder(name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (plane_mesh) {
                    Vector2 mesh_size = plane_mesh->get_size();
                    Vector3 final_scale = Vector3(scale.x * mesh_size.x, scale.y, scale.z * mesh_size.y);
                    add_plane(name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else {
                    // Unknown mesh type - try as cube for now
                    UtilityFunctions::print("[OHAO] Unknown mesh type for '", name, "', treating as cube");
                    add_cube(name, position, rotation * (180.0f / Math_PI), scale, color);
                    m_synced_object_count++;
                }
            }
        }

        // Check for DirectionalLight3D
        DirectionalLight3D* dir_light = Object::cast_to<DirectionalLight3D>(node);
        if (dir_light) {
            Color color = dir_light->get_color();
            float intensity = dir_light->get_param(Light3D::PARAM_ENERGY);
            // Direction is -Z in local space, transformed by rotation
            Vector3 direction = -transform.basis.get_column(2);
            add_directional_light(name, position, direction, color, intensity);
            m_synced_object_count++;
        }

        // Check for OmniLight3D (point light)
        OmniLight3D* omni_light = Object::cast_to<OmniLight3D>(node);
        if (omni_light) {
            Color color = omni_light->get_color();
            float intensity = omni_light->get_param(Light3D::PARAM_ENERGY);
            float range = omni_light->get_param(Light3D::PARAM_RANGE);
            add_point_light(name, position, color, intensity, range);
            m_synced_object_count++;
        }

        // Check for SpotLight3D
        SpotLight3D* spot_light = Object::cast_to<SpotLight3D>(node);
        if (spot_light) {
            // TODO: Add spot light support
            UtilityFunctions::print("[OHAO] SpotLight3D '", name, "' not fully supported yet, treating as point light");
            Color color = spot_light->get_color();
            float intensity = spot_light->get_param(Light3D::PARAM_ENERGY);
            float range = spot_light->get_param(Light3D::PARAM_RANGE);
            add_point_light(name, position, color, intensity, range);
            m_synced_object_count++;
        }

        // Check for Camera3D
        Camera3D* camera = Object::cast_to<Camera3D>(node);
        if (camera) {
            // Update OHAO camera to match Godot camera
            ohao::Camera& ohao_cam = m_renderer->getCamera();
            ohao_cam.setPosition(to_glm(position));
            // Convert rotation - Godot uses radians, OHAO uses degrees for setRotation
            ohao_cam.setRotation(glm::degrees(rotation.x), glm::degrees(rotation.y));
            UtilityFunctions::print("[OHAO] Camera synced at position: (", position.x, ", ", position.y, ", ", position.z, ")");
        }
    }

    // Recursively traverse children
    int child_count = node->get_child_count();
    for (int i = 0; i < child_count; i++) {
        traverse_and_sync(node->get_child(i));
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
