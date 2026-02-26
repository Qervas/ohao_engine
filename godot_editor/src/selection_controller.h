#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <glm/glm.hpp>

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class Scene;
    class Actor;
}

namespace godot {

// Forward declare Godot types
class Control;

/**
 * SelectionController - Object picking and selection highlight
 *
 * Extracted from OhaoViewport. Handles ray-casting picks and
 * 2D overlay drawing for the selection bracket indicator.
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

    // Accessors
    ohao::Actor* getSelectedActor() const { return m_selected_actor; }
    String getSelectedActorName() const { return m_selected_actor_name; }
    bool hasSelection() const { return m_selected_actor != nullptr; }

private:
    ohao::Actor* m_selected_actor = nullptr;
    String m_selected_actor_name;
};

} // namespace godot
