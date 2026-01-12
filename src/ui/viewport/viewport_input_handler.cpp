#include "viewport_input_handler.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/gizmo/transform_gizmo.hpp"
#include "ui/window/window.hpp"
#include "engine/component/transform_component.hpp"
#include <GLFW/glfw3.h>
#include <iostream>
#include <glm/gtc/quaternion.hpp>

namespace ohao {

void ViewportInputHandler::initialize(
    VulkanContext* ctx,
    Window* win,
    PickingSystem* picking
) {
    context = ctx;
    window = win;
    pickingSystem = picking;
}

void ViewportInputHandler::update(float deltaTime) {
    // Don't process input in play mode or if not hovering viewport
    if (isPlayMode) return;

    // Handle keyboard shortcuts regardless of hover (for mode switching)
    handleKeyboardShortcuts();

    // Only process mouse input when hovering viewport
    if (!isViewportHovered) return;

    // Handle mouse scroll for zoom
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f) {
        handleMouseScroll(io.MouseWheel);
    }

    // State machine
    switch (currentState) {
        case ViewportInputState::Idle:
            processIdleState(deltaTime);
            break;
        case ViewportInputState::CameraOrbit:
            processCameraOrbitState(deltaTime);
            break;
        case ViewportInputState::CameraPan:
            processCameraPanState(deltaTime);
            break;
        case ViewportInputState::GizmoDrag:
            processGizmoDragState(deltaTime);
            break;
        default:
            break;
    }
}

void ViewportInputHandler::processIdleState(float deltaTime) {
    // Update gizmo hover state
    updateGizmoHover();

    // Right click - start camera orbit
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        handleRightClickStart();
        return;
    }

    // Middle click - start camera pan
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        handleMiddleClickStart();
        return;
    }

    // Left click - select object or start gizmo drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        handleLeftClick();
        return;
    }
}

void ViewportInputHandler::processCameraOrbitState(float deltaTime) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        handleRightClickEnd();
        return;
    }

    glm::vec2 currentMousePos = glm::vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
    glm::vec2 delta = currentMousePos - lastMousePos;
    lastMousePos = currentMousePos;

    updateCameraOrbit(delta);
}

void ViewportInputHandler::processCameraPanState(float deltaTime) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        handleMiddleClickEnd();
        return;
    }

    glm::vec2 currentMousePos = glm::vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
    glm::vec2 delta = currentMousePos - lastMousePos;
    lastMousePos = currentMousePos;

    updateCameraPan(delta);
}

void ViewportInputHandler::processGizmoDragState(float deltaTime) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        endGizmoDrag();
        return;
    }

    updateGizmoDrag();
}

void ViewportInputHandler::setViewportBounds(const glm::vec2& min, const glm::vec2& max) {
    viewportMin = min;
    viewportMax = max;
    viewportSize = max - min;
}

void ViewportInputHandler::setGizmoMode(GizmoMode mode) {
    currentGizmoMode = mode;
    std::cout << "[ViewportInput] Gizmo mode: ";
    switch (mode) {
        case GizmoMode::Translate: std::cout << "Translate (W)" << std::endl; break;
        case GizmoMode::Rotate: std::cout << "Rotate (E)" << std::endl; break;
        case GizmoMode::Scale: std::cout << "Scale (R)" << std::endl; break;
    }
}

void ViewportInputHandler::cycleGizmoMode() {
    switch (currentGizmoMode) {
        case GizmoMode::Translate:
            setGizmoMode(GizmoMode::Rotate);
            break;
        case GizmoMode::Rotate:
            setGizmoMode(GizmoMode::Scale);
            break;
        case GizmoMode::Scale:
            setGizmoMode(GizmoMode::Translate);
            break;
    }
}

glm::vec2 ViewportInputHandler::getMousePosInViewport() const {
    ImVec2 mousePos = ImGui::GetMousePos();
    return glm::vec2(mousePos.x - viewportMin.x, mousePos.y - viewportMin.y);
}

Ray ViewportInputHandler::getMouseRay() const {
    if (!context || !pickingSystem) return Ray();

    glm::vec2 localPos = getMousePosInViewport();
    return pickingSystem->screenToWorldRay(localPos, viewportSize, context->getCamera());
}

bool ViewportInputHandler::isMouseInViewport() const {
    ImVec2 mousePos = ImGui::GetMousePos();
    return mousePos.x >= viewportMin.x && mousePos.x <= viewportMax.x &&
           mousePos.y >= viewportMin.y && mousePos.y <= viewportMax.y;
}

void ViewportInputHandler::handleLeftClick() {
    if (!pickingSystem || !context) return;

    // Check if clicking on gizmo first
    if (hoveredAxis != GizmoAxis::None) {
        beginGizmoDrag();
        return;
    }

    // Otherwise, do object picking
    Ray ray = getMouseRay();
    Scene* scene = context->getScene();
    if (!scene) return;

    PickResult result = pickingSystem->pickActor(ray, scene);

    if (result.hit && result.actor) {
        // Select the hit actor
        SelectionManager::get().setSelectedActor(result.actor);
        std::cout << "[ViewportInput] Selected: " << result.actor->getName() << std::endl;

        // Update orbit target to selected object
        if (auto* transform = result.actor->getTransform()) {
            orbitTarget = transform->getWorldPosition();
        }
    } else {
        // Clicked empty space - clear selection
        SelectionManager::get().clearSelection();
        std::cout << "[ViewportInput] Selection cleared" << std::endl;
    }
}

void ViewportInputHandler::handleLeftRelease() {
    if (currentState == ViewportInputState::GizmoDrag) {
        endGizmoDrag();
    }
}

void ViewportInputHandler::handleRightClickStart() {
    currentState = ViewportInputState::CameraOrbit;
    lastMousePos = glm::vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);

    // Set orbit target to selected object if available
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (selected && selected->getTransform()) {
        orbitTarget = selected->getTransform()->getWorldPosition();
    }
}

void ViewportInputHandler::handleRightClickEnd() {
    currentState = ViewportInputState::Idle;
}

void ViewportInputHandler::handleMiddleClickStart() {
    currentState = ViewportInputState::CameraPan;
    lastMousePos = glm::vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
}

void ViewportInputHandler::handleMiddleClickEnd() {
    currentState = ViewportInputState::Idle;
}

void ViewportInputHandler::handleKeyboardShortcuts() {
    // Only handle shortcuts when viewport is focused or hovered
    if (!isViewportHovered && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
        return;
    }

    // Don't process if ImGui wants keyboard input (e.g., text input)
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // Gizmo mode switching (W/E/R)
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        setGizmoMode(GizmoMode::Translate);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        setGizmoMode(GizmoMode::Rotate);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        setGizmoMode(GizmoMode::Scale);
    }

    // Space to cycle modes
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        cycleGizmoMode();
    }

    // F to focus on selected object
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        focusOnSelection();
    }

    // Delete selected object
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        Actor* selected = SelectionManager::get().getSelectedActor();
        if (selected && context && context->getScene()) {
            std::cout << "[ViewportInput] Deleting: " << selected->getName() << std::endl;
            context->getScene()->removeActor(selected->getID());
            SelectionManager::get().clearSelection();
        }
    }
}

void ViewportInputHandler::handleMouseScroll(float scrollDelta) {
    updateCameraZoom(scrollDelta);
}

void ViewportInputHandler::updateGizmoHover() {
    // Transform gizmo disabled - needs rework
    hoveredAxis = GizmoAxis::None;
}

void ViewportInputHandler::beginGizmoDrag() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform()) return;

    auto* transform = selected->getTransform();
    // Use world matrix position for consistency with rendering
    dragStartPosition = glm::vec3(transform->getWorldMatrix()[3]);
    dragStartScale = transform->getScale();
    dragStartRotation = transform->getRotation();

    activeAxis = hoveredAxis;
    currentState = ViewportInputState::GizmoDrag;

    // Start drag on the TransformGizmo if available
    if (transformGizmo && activeAxis != GizmoAxis::None) {
        Ray ray = getMouseRay();
        glm::vec3 cameraPos = context->getCamera().getPosition();
        transformGizmo->beginDrag(activeAxis, ray.origin, ray.direction, dragStartPosition, cameraPos);
    }

    std::cout << "[ViewportInput] Started gizmo drag on axis " << static_cast<int>(activeAxis) << std::endl;
}

void ViewportInputHandler::updateGizmoDrag() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform()) {
        endGizmoDrag();
        return;
    }

    if (!transformGizmo) return;

    auto* transform = selected->getTransform();
    Ray ray = getMouseRay();

    switch (currentGizmoMode) {
        case GizmoMode::Translate: {
            // Get new position from gizmo's drag calculation
            glm::vec3 cameraPos = context->getCamera().getPosition();
            glm::vec3 newPosition = transformGizmo->updateDrag(ray.origin, ray.direction, cameraPos);
            transform->setPosition(newPosition);
            break;
        }
        case GizmoMode::Rotate:
            // TODO: Implement rotation mode
            break;
        case GizmoMode::Scale:
            // TODO: Implement scale mode
            break;
    }
}

void ViewportInputHandler::endGizmoDrag() {
    // End drag on the TransformGizmo
    if (transformGizmo) {
        transformGizmo->endDrag();
    }

    activeAxis = GizmoAxis::None;
    currentState = ViewportInputState::Idle;
    std::cout << "[ViewportInput] Ended gizmo drag" << std::endl;

    // Update scene buffers after transform change
    if (context) {
        context->updateSceneBuffers();
    }
}

void ViewportInputHandler::updateCameraOrbit(const glm::vec2& mouseDelta) {
    if (!context) return;

    Camera& camera = context->getCamera();

    // Calculate orbit rotation
    float yawDelta = -mouseDelta.x * orbitSensitivity;
    float pitchDelta = -mouseDelta.y * orbitSensitivity;

    // Get current camera orientation
    float currentPitch = camera.getPitch();
    float currentYaw = camera.getYaw();

    // Apply rotation
    float newPitch = glm::clamp(currentPitch + pitchDelta, -89.0f, 89.0f);
    float newYaw = currentYaw + yawDelta;

    // Calculate new camera position orbiting around target
    glm::vec3 cameraPos = camera.getPosition();
    float distance = glm::length(cameraPos - orbitTarget);

    // Convert to spherical coordinates
    float pitchRad = glm::radians(newPitch);
    float yawRad = glm::radians(newYaw);

    glm::vec3 newOffset;
    newOffset.x = distance * cos(pitchRad) * cos(yawRad);
    newOffset.y = distance * sin(pitchRad);
    newOffset.z = distance * cos(pitchRad) * sin(yawRad);

    glm::vec3 newCameraPos = orbitTarget + newOffset;

    camera.setPosition(newCameraPos);
    camera.setRotation(newPitch, newYaw + 90.0f);  // Adjust yaw offset
}

void ViewportInputHandler::updateCameraPan(const glm::vec2& mouseDelta) {
    if (!context) return;

    Camera& camera = context->getCamera();

    // Pan camera based on its right and up vectors
    glm::vec3 right = camera.getRight();
    glm::vec3 up = camera.getUp();

    glm::vec3 panOffset = right * (-mouseDelta.x * panSensitivity) +
                          up * (mouseDelta.y * panSensitivity);

    camera.move(panOffset);
    orbitTarget += panOffset;  // Move orbit target with camera
}

void ViewportInputHandler::updateCameraZoom(float scrollDelta) {
    if (!context) return;

    Camera& camera = context->getCamera();

    // Dolly camera towards/away from orbit target
    glm::vec3 cameraPos = camera.getPosition();
    glm::vec3 direction = glm::normalize(orbitTarget - cameraPos);
    float distance = glm::length(orbitTarget - cameraPos);

    // Calculate new distance with exponential scaling for smooth zoom
    float zoomFactor = 1.0f - scrollDelta * zoomSensitivity * 0.1f;
    float newDistance = glm::clamp(distance * zoomFactor, 0.5f, 100.0f);

    glm::vec3 newCameraPos = orbitTarget - direction * newDistance;
    camera.setPosition(newCameraPos);
}

void ViewportInputHandler::focusOnSelection() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform() || !context) return;

    glm::vec3 targetPos = selected->getTransform()->getWorldPosition();
    orbitTarget = targetPos;

    // Focus camera on the target
    context->getCamera().focusOnPoint(targetPos, 5.0f);

    std::cout << "[ViewportInput] Focused on: " << selected->getName() << std::endl;
}

} // namespace ohao
