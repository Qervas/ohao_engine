#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <glm/glm.hpp>

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class DeferredRenderer;
    class Scene;
    class Actor;
}

namespace godot {

// Forward declare Godot types
class Control;

/// Screen-space selection bracket data returned by computeSelectionBounds().
struct SelectionOverlay {
    bool visible = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;  // bracket rect (padded)
    float cornerX = 0, cornerY = 0;                  // corner line lengths
};

/**
 * SelectionController - Object picking and selection highlight
 *
 * Extracted from OhaoViewport. Handles ray-casting picks,
 * selection bracket computation, and gizmo positioning.
 */
class SelectionController {
public:
    SelectionController();

    // Pick an object at screen position. Updates selected actor.
    void pickObjectAt(const Vector2& screen_pos, const Vector2& control_size,
                      int render_width, int render_height,
                      ohao::OffscreenRenderer* renderer, ohao::Scene* scene);

    // Clear selection (e.g. before scene clear)
    void clearSelection();

    // Compute screen-space selection bracket for current selection.
    // All glm math lives here; caller only needs Godot draw calls.
    SelectionOverlay computeSelectionBounds(const glm::mat4& viewProj, float ctrlW, float ctrlH);

    // Position gizmo on selected actor via DeferredRenderer.
    void updateGizmo(ohao::OffscreenRenderer* renderer, bool gizmoEnabled);

    // Accessors
    ohao::Actor* getSelectedActor() const { return m_selected_actor; }
    String getSelectedActorName() const { return m_selected_actor_name; }
    bool hasSelection() const { return m_selected_actor != nullptr; }

private:
    ohao::Actor* m_selected_actor = nullptr;
    String m_selected_actor_name;
};

} // namespace godot
