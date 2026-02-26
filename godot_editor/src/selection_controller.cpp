#include "selection_controller.h"

#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/picking/picking_system.hpp"
#include "renderer/picking/ray.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"

#include <glm/glm.hpp>

namespace godot {

SelectionController::SelectionController() {}

void SelectionController::pickObjectAt(const Vector2& screen_pos, const Vector2& control_size,
                                        int render_width, int render_height,
                                        ohao::OffscreenRenderer* renderer, ohao::Scene* scene) {
    if (!renderer || !scene) return;

    ohao::Camera& camera = renderer->getCamera();
    ohao::PickingSystem picking;

    float scale_x = (control_size.x > 0) ? static_cast<float>(render_width) / control_size.x : 1.0f;
    float scale_y = (control_size.y > 0) ? static_cast<float>(render_height) / control_size.y : 1.0f;
    glm::vec2 screenCoord(screen_pos.x * scale_x, screen_pos.y * scale_y);
    glm::vec2 viewportSize(static_cast<float>(render_width), static_cast<float>(render_height));
    ohao::Ray ray = picking.screenToWorldRay(screenCoord, viewportSize, camera);

    ohao::PickResult result = picking.pickActor(ray, scene);

    if (result.hit && result.actor) {
        m_selected_actor = result.actor;
        m_selected_actor_name = String(result.actor->getName().c_str());
    } else {
        m_selected_actor = nullptr;
        m_selected_actor_name = "";
    }
}

void SelectionController::clearSelection() {
    m_selected_actor = nullptr;
    m_selected_actor_name = "";
}

} // namespace godot
