#include "ohao_viewport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>

// TODO: Include OHAO headers when linking is set up
// #include "renderer/vulkan_context.hpp"
// #include "engine/scene/scene.hpp"

namespace godot {

void OhaoViewport::_bind_methods() {
    // Methods
    ClassDB::bind_method(D_METHOD("initialize_renderer"), &OhaoViewport::initialize_renderer);
    ClassDB::bind_method(D_METHOD("shutdown_renderer"), &OhaoViewport::shutdown_renderer);
    ClassDB::bind_method(D_METHOD("is_renderer_initialized"), &OhaoViewport::is_renderer_initialized);
    ClassDB::bind_method(D_METHOD("sync_scene"), &OhaoViewport::sync_scene);

    // Properties
    ClassDB::bind_method(D_METHOD("set_render_enabled", "enabled"), &OhaoViewport::set_render_enabled);
    ClassDB::bind_method(D_METHOD("get_render_enabled"), &OhaoViewport::get_render_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "render_enabled"), "set_render_enabled", "get_render_enabled");
}

OhaoViewport::OhaoViewport() {
    UtilityFunctions::print("[OHAO] Viewport created");
}

OhaoViewport::~OhaoViewport() {
    shutdown_renderer();
}

void OhaoViewport::_ready() {
    UtilityFunctions::print("[OHAO] Viewport ready - initializing renderer");
    initialize_renderer();
}

void OhaoViewport::_process(double delta) {
    if (!m_initialized || !m_render_enabled) {
        return;
    }

    // Sync Godot scene to OHAO scene
    sync_scene();

    // TODO: Render frame with OHAO Vulkan renderer
    // if (m_vulkan_context) {
    //     static_cast<ohao::VulkanContext*>(m_vulkan_context)->drawFrame();
    // }
}

void OhaoViewport::initialize_renderer() {
    if (m_initialized) {
        return;
    }

    UtilityFunctions::print("[OHAO] Initializing Vulkan renderer...");

    // TODO: Initialize OHAO Vulkan context
    // m_vulkan_context = new ohao::VulkanContext();
    // m_scene = new ohao::Scene();
    // static_cast<ohao::VulkanContext*>(m_vulkan_context)->initialize();

    m_initialized = true;
    UtilityFunctions::print("[OHAO] Renderer initialized");
}

void OhaoViewport::shutdown_renderer() {
    if (!m_initialized) {
        return;
    }

    UtilityFunctions::print("[OHAO] Shutting down renderer...");

    // TODO: Cleanup OHAO
    // delete static_cast<ohao::VulkanContext*>(m_vulkan_context);
    // delete static_cast<ohao::Scene*>(m_scene);

    m_vulkan_context = nullptr;
    m_scene = nullptr;
    m_initialized = false;
}

void OhaoViewport::set_render_enabled(bool enabled) {
    m_render_enabled = enabled;
}

void OhaoViewport::sync_scene() {
    // Traverse Godot scene tree and sync to OHAO scene
    // This is where we convert Godot nodes to OHAO actors

    // TODO: Implement scene sync
    // For each MeshInstance3D child:
    //   - Get transform
    //   - Get mesh data
    //   - Create/update corresponding OHAO Actor
    //
    // Example:
    // for (int i = 0; i < get_child_count(); i++) {
    //     Node* child = get_child(i);
    //     if (MeshInstance3D* mesh = Object::cast_to<MeshInstance3D>(child)) {
    //         Transform3D transform = mesh->get_global_transform();
    //         // Sync to OHAO...
    //     }
    // }
}

} // namespace godot
