#pragma once

#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace godot {

/**
 * OhaoViewport - Custom viewport that renders using OHAO's Vulkan renderer
 *
 * This node replaces Godot's rendering with our custom Vulkan pipeline.
 * It syncs scene data from Godot to OHAO and displays the result.
 */
class OhaoViewport : public SubViewport {
    GDCLASS(OhaoViewport, SubViewport)

private:
    bool m_initialized = false;
    bool m_render_enabled = true;

    // OHAO engine pointers (will be initialized)
    void* m_vulkan_context = nullptr;
    void* m_scene = nullptr;

protected:
    static void _bind_methods();

public:
    OhaoViewport();
    ~OhaoViewport();

    // Lifecycle
    void _ready() override;
    void _process(double delta) override;

    // OHAO Engine control
    void initialize_renderer();
    void shutdown_renderer();
    bool is_renderer_initialized() const { return m_initialized; }

    // Rendering
    void set_render_enabled(bool enabled);
    bool get_render_enabled() const { return m_render_enabled; }

    // Scene sync - pull data from Godot scene tree
    void sync_scene();
};

} // namespace godot
