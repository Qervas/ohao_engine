#include "selection_controller.h"

#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "renderer/picking/picking_system.hpp"
#include "renderer/picking/ray.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/asset/model.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cfloat>
#include <algorithm>

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

SelectionOverlay SelectionController::computeSelectionBounds(const glm::mat4& viewProj, float ctrlW, float ctrlH) {
    SelectionOverlay overlay;
    if (!m_selected_actor) return overlay;

    auto transform = m_selected_actor->getTransform();
    if (!transform) return overlay;

    glm::vec3 world_pos = transform->getPosition();
    glm::vec3 scale = transform->getScale();
    glm::quat rotation = transform->getRotation();
    glm::mat4 world_matrix = glm::translate(glm::mat4(1.0f), world_pos)
                           * glm::mat4_cast(rotation)
                           * glm::scale(glm::mat4(1.0f), scale);

    // Determine local-space AABB
    glm::vec3 local_min(0.0f), local_max(0.0f);

    auto model = m_selected_actor->getModel();
    if (model && !model->vertices.empty()) {
        local_min = glm::vec3(FLT_MAX);
        local_max = glm::vec3(-FLT_MAX);
        for (const auto& v : model->vertices) {
            local_min = glm::min(local_min, v.position);
            local_max = glm::max(local_max, v.position);
        }
    } else {
        local_min = glm::vec3(-0.5f);
        local_max = glm::vec3(0.5f);
    }

    // Early-out if center is behind camera
    glm::vec4 center_clip = viewProj * glm::vec4(world_pos, 1.0f);
    if (center_clip.w < 0.001f) return overlay;

    // Project 8 AABB corners to screen space
    float min_sx = 1e9f, min_sy = 1e9f, max_sx = -1e9f, max_sy = -1e9f;
    int valid_count = 0;

    for (int i = 0; i < 8; i++) {
        glm::vec3 local_corner(
            (i & 1) ? local_max.x : local_min.x,
            (i & 2) ? local_max.y : local_min.y,
            (i & 4) ? local_max.z : local_min.z
        );
        glm::vec4 world_corner = world_matrix * glm::vec4(local_corner, 1.0f);
        glm::vec4 clip = viewProj * world_corner;
        if (clip.w < 0.001f) continue;
        float sx = (clip.x / clip.w * 0.5f + 0.5f) * ctrlW;
        float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * ctrlH;
        min_sx = std::min(min_sx, sx);
        min_sy = std::min(min_sy, sy);
        max_sx = std::max(max_sx, sx);
        max_sy = std::max(max_sy, sy);
        valid_count++;
    }

    if (valid_count < 2) return overlay;

    // Pad and enforce minimum dimensions
    float pad = 12.0f;
    float min_dim = 40.0f;
    float w = max_sx - min_sx;
    float h = max_sy - min_sy;
    if (w < min_dim) { float d = (min_dim - w) * 0.5f; min_sx -= d; max_sx += d; }
    if (h < min_dim) { float d = (min_dim - h) * 0.5f; min_sy -= d; max_sy += d; }
    min_sx -= pad; min_sy -= pad;
    max_sx += pad; max_sy += pad;

    // Corner line lengths
    float box_w = max_sx - min_sx;
    float box_h = max_sy - min_sy;
    float corner_frac = 0.3f;
    float cx = box_w * corner_frac;
    float cy = box_h * corner_frac;
    float max_corner = 20.0f;
    if (cx > max_corner) cx = max_corner;
    if (cy > max_corner) cy = max_corner;

    overlay.visible = true;
    overlay.minX = min_sx;
    overlay.minY = min_sy;
    overlay.maxX = max_sx;
    overlay.maxY = max_sy;
    overlay.cornerX = cx;
    overlay.cornerY = cy;
    return overlay;
}

void SelectionController::updateGizmo(ohao::OffscreenRenderer* renderer, bool gizmoEnabled) {
    if (!renderer) return;
    ohao::DeferredRenderer* deferred = renderer->getDeferredRenderer();
    if (!deferred) return;

    if (gizmoEnabled && m_selected_actor) {
        auto transform = m_selected_actor->getTransform();
        if (transform) {
            glm::vec3 pos = transform->getPosition();
            glm::mat4 gizmoModel = glm::translate(glm::mat4(1.0f), pos);
            deferred->setGizmoTransform(gizmoModel);
            deferred->setGizmoEnabled(true);
            return;
        }
    }
    deferred->setGizmoEnabled(false);
}

} // namespace godot
