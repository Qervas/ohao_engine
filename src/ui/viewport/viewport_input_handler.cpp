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
        case ViewportInputState::TranslateModal:
            processTranslateModalState(deltaTime);
            break;
        case ViewportInputState::RotateModal:
            processRotateModalState(deltaTime);
            break;
        case ViewportInputState::ScaleModal:
            processScaleModalState(deltaTime);
            break;
        default:
            break;
    }

    // WASD camera movement + Arrow key rotation (only in Idle state, not during modal transforms)
    if (currentState == ViewportInputState::Idle) {
        updateCameraMovement(deltaTime);
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
    // NOTE: We intentionally don't check WantCaptureKeyboard here
    // G/R/S are global modal shortcuts that should work anywhere in the app
    // (like Blender's design - they work even when panels are open)
    // Only real text input should block them, and ImGui doesn't provide a clean way to detect that

    // Check if in modal state
    bool inModalState = (currentState == ViewportInputState::TranslateModal ||
                         currentState == ViewportInputState::RotateModal ||
                         currentState == ViewportInputState::ScaleModal);

    if (inModalState) {
        handleModalKeys();  // X/Y/Z/ESC keys
        return;
    }

    // Get selection for modal entry
    Actor* selected = SelectionManager::get().getSelectedActor();

    // G/R/S - Modal transform entry (only if object selected)
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        std::cout << "[DEBUG] G key pressed. Selected: " << (selected ? selected->getName() : "NULL")
                  << ", HasTransform: " << (selected && selected->getTransform() ? "YES" : "NO") << std::endl;
        if (selected && selected->getTransform()) {
            enterTranslateModal();
            return;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        std::cout << "[DEBUG] R key pressed. Selected: " << (selected ? selected->getName() : "NULL")
                  << ", HasTransform: " << (selected && selected->getTransform() ? "YES" : "NO") << std::endl;
        if (selected && selected->getTransform()) {
            enterRotateModal();
            return;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        std::cout << "[DEBUG] S key pressed. Selected: " << (selected ? selected->getName() : "NULL")
                  << ", HasTransform: " << (selected && selected->getTransform() ? "YES" : "NO") << std::endl;
        if (selected && selected->getTransform()) {
            enterScaleModal();
            return;
        }
    }

    // Space to cycle gizmo modes (W/E removed to avoid conflict with camera movement)
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        cycleGizmoMode();
    }

    // F to focus on selected object
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        focusOnSelection();
    }

    // Delete selected object
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
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

void ViewportInputHandler::updateCameraMovement(float deltaTime) {
    if (!context) return;

    Camera& camera = context->getCamera();
    glm::vec3 movement(0.0f);
    bool moved = false;

    // WASD movement
    if (ImGui::IsKeyDown(ImGuiKey_W)) {
        movement += camera.getFront();
        moved = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_S)) {
        movement -= camera.getFront();
        moved = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_A)) {
        movement -= camera.getRight();
        moved = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_D)) {
        movement += camera.getRight();
        moved = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_Q)) {
        movement -= glm::vec3(0, 1, 0);  // Down
        moved = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_E)) {
        movement += glm::vec3(0, 1, 0);  // Up
        moved = true;
    }

    // Apply movement
    if (moved) {
        float speed = cameraMovementSpeed * deltaTime;
        // Speed up with Shift
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) {
            speed *= 3.0f;
        }
        camera.move(glm::normalize(movement) * speed);
    }

    // Arrow key rotation (inverted up/down for Vulkan convention)
    float rotationDelta = cameraRotationSpeed * deltaTime;
    bool rotated = false;

    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
        camera.rotate(rotationDelta, 0.0f);  // Pitch down (inverted)
        rotated = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
        camera.rotate(-rotationDelta, 0.0f);  // Pitch up (inverted)
        rotated = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
        camera.rotate(0.0f, -rotationDelta);  // Yaw left
        rotated = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
        camera.rotate(0.0f, rotationDelta);  // Yaw right
        rotated = true;
    }
}

// ============================================================================
// Modal Transform System
// ============================================================================

void ViewportInputHandler::enterTranslateModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform()) return;

    auto* transform = selected->getTransform();
    modalStartPosition = transform->getPosition();
    modalStartMousePos = getMousePosInViewport();
    currentConstraint = AxisConstraint::None;
    currentState = ViewportInputState::TranslateModal;

    std::cout << "[Modal] Translate - Move mouse, X/Y/Z to constrain, click to confirm, ESC to cancel" << std::endl;
}

void ViewportInputHandler::enterRotateModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform()) return;

    auto* transform = selected->getTransform();
    modalStartRotation = transform->getRotation();
    modalStartMousePos = getMousePosInViewport();
    currentConstraint = AxisConstraint::None;
    currentState = ViewportInputState::RotateModal;

    std::cout << "[Modal] Rotate - Move mouse, X/Y/Z to constrain, click to confirm, ESC to cancel" << std::endl;
}

void ViewportInputHandler::enterScaleModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform()) return;

    auto* transform = selected->getTransform();
    modalStartScale = transform->getScale();
    modalStartMousePos = getMousePosInViewport();
    currentConstraint = AxisConstraint::None;
    currentState = ViewportInputState::ScaleModal;

    std::cout << "[Modal] Scale - Move mouse, X/Y/Z to constrain, click to confirm, ESC to cancel" << std::endl;
}

void ViewportInputHandler::confirmModal() {
    std::cout << "[Modal] Transform confirmed" << std::endl;
    exitModal();
}

void ViewportInputHandler::cancelModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (selected && selected->getTransform()) {
        auto* transform = selected->getTransform();

        switch (currentState) {
            case ViewportInputState::TranslateModal:
                transform->setPosition(modalStartPosition);
                std::cout << "[Modal] Translate canceled" << std::endl;
                break;
            case ViewportInputState::RotateModal:
                transform->setRotation(modalStartRotation);
                std::cout << "[Modal] Rotate canceled" << std::endl;
                break;
            case ViewportInputState::ScaleModal:
                transform->setScale(modalStartScale);
                std::cout << "[Modal] Scale canceled" << std::endl;
                break;
            default:
                break;
        }

        if (context) {
            context->updateSceneBuffers();
        }
    }

    exitModal();
}

void ViewportInputHandler::exitModal() {
    currentState = ViewportInputState::Idle;
    currentConstraint = AxisConstraint::None;
}

void ViewportInputHandler::processTranslateModalState(float deltaTime) {
    // Left click to confirm
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        confirmModal();
        return;
    }

    // Right click to cancel
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        cancelModal();
        return;
    }

    // Update transform based on mouse movement
    updateTranslateModal();
}

void ViewportInputHandler::updateTranslateModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform() || !context) {
        cancelModal();
        return;
    }

    auto* transform = selected->getTransform();
    glm::vec2 currentMousePos = getMousePosInViewport();

    if (currentConstraint == AxisConstraint::None) {
        // FREE MOVEMENT - screen-space delta to view-plane movement
        glm::vec2 mouseDelta = currentMousePos - modalStartMousePos;

        Camera& camera = context->getCamera();
        glm::vec3 right = camera.getRight();
        glm::vec3 up = camera.getUp();

        float sensitivity = 0.01f;
        glm::vec3 offset = right * (mouseDelta.x * sensitivity) +
                          up * (-mouseDelta.y * sensitivity);

        transform->setPosition(modalStartPosition + offset);
    }
    else {
        // CONSTRAINED - Find closest point on constraint axis to mouse ray
        // This is more stable than ray-plane intersection
        Ray ray = getMouseRay();

        // Line-line closest point algorithm
        // Ray: P = ray.origin + t * ray.direction
        // Axis: Q = modalStartPosition + s * modalConstraintAxis
        // Find s that minimizes distance between P and Q

        glm::vec3 w0 = ray.origin - modalStartPosition;
        float a = glm::dot(ray.direction, ray.direction);
        float b = glm::dot(ray.direction, modalConstraintAxis);
        float c = glm::dot(modalConstraintAxis, modalConstraintAxis);
        float d = glm::dot(ray.direction, w0);
        float e = glm::dot(modalConstraintAxis, w0);

        float denom = a * c - b * b;

        if (std::abs(denom) > 0.0001f) {
            // Lines are not parallel
            float s = (b * d - a * e) / denom;

            // Clamp to reasonable range to avoid extreme values
            s = glm::clamp(s, -1000.0f, 1000.0f);

            glm::vec3 newPosition = modalStartPosition + modalConstraintAxis * s;
            transform->setPosition(newPosition);
        } else {
            // Lines are nearly parallel, use projection method as fallback
            glm::vec3 toMouse = ray.origin - modalStartPosition;
            float axisOffset = glm::dot(toMouse, modalConstraintAxis);
            transform->setPosition(modalStartPosition + modalConstraintAxis * axisOffset);
        }
    }

    context->updateSceneBuffers();
}

void ViewportInputHandler::processRotateModalState(float deltaTime) {
    // Left click to confirm
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        confirmModal();
        return;
    }

    // Right click to cancel
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        cancelModal();
        return;
    }

    // Update transform based on mouse movement
    updateRotateModal();
}

void ViewportInputHandler::updateRotateModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform() || !context) {
        cancelModal();
        return;
    }

    auto* transform = selected->getTransform();
    glm::vec2 currentMousePos = getMousePosInViewport();
    glm::vec2 mouseDelta = currentMousePos - modalStartMousePos;

    float sensitivity = 0.5f;  // degrees per pixel
    float angle = glm::radians(mouseDelta.x * sensitivity);

    glm::vec3 rotationAxis;
    if (currentConstraint == AxisConstraint::None) {
        // Rotate around view axis
        Camera& camera = context->getCamera();
        rotationAxis = camera.getFront();
    }
    else {
        // Rotate around constraint axis
        rotationAxis = modalConstraintAxis;
    }

    glm::quat rotation = glm::angleAxis(angle, rotationAxis);
    glm::quat newRotation = rotation * modalStartRotation;
    transform->setRotation(newRotation);

    context->updateSceneBuffers();
}

void ViewportInputHandler::processScaleModalState(float deltaTime) {
    // Left click to confirm
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        confirmModal();
        return;
    }

    // Right click to cancel
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        cancelModal();
        return;
    }

    // Update transform based on mouse movement
    updateScaleModal();
}

void ViewportInputHandler::updateScaleModal() {
    Actor* selected = SelectionManager::get().getSelectedActor();
    if (!selected || !selected->getTransform() || !context) {
        cancelModal();
        return;
    }

    auto* transform = selected->getTransform();
    glm::vec2 currentMousePos = getMousePosInViewport();
    glm::vec2 mouseDelta = currentMousePos - modalStartMousePos;

    float sensitivity = 0.01f;
    float scaleFactor = 1.0f + mouseDelta.x * sensitivity;
    scaleFactor = glm::max(scaleFactor, 0.01f);  // Prevent negative/zero

    if (currentConstraint == AxisConstraint::None) {
        // Uniform scale
        transform->setScale(modalStartScale * scaleFactor);
    }
    else {
        // Scale along specific axis
        glm::vec3 newScale = modalStartScale;
        int axisIndex = (currentConstraint == AxisConstraint::X) ? 0 :
                       (currentConstraint == AxisConstraint::Y) ? 1 : 2;
        newScale[axisIndex] = modalStartScale[axisIndex] * scaleFactor;
        transform->setScale(newScale);
    }

    context->updateSceneBuffers();
}

void ViewportInputHandler::handleModalKeys() {
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        setModalConstraint(AxisConstraint::X);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        setModalConstraint(AxisConstraint::Y);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        setModalConstraint(AxisConstraint::Z);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        cancelModal();
    }
}

void ViewportInputHandler::setModalConstraint(AxisConstraint constraint) {
    currentConstraint = constraint;

    switch (constraint) {
        case AxisConstraint::X:
            modalConstraintAxis = glm::vec3(1, 0, 0);
            std::cout << "[Modal] Constrained to X axis" << std::endl;
            break;
        case AxisConstraint::Y:
            // Vulkan uses Y-down in NDC, so we negate to match expected world-space Y-up behavior
            modalConstraintAxis = glm::vec3(0, -1, 0);
            std::cout << "[Modal] Constrained to Y axis" << std::endl;
            break;
        case AxisConstraint::Z:
            modalConstraintAxis = glm::vec3(0, 0, 1);
            std::cout << "[Modal] Constrained to Z axis" << std::endl;
            break;
        case AxisConstraint::None:
            std::cout << "[Modal] Free movement" << std::endl;
            break;
    }

    // Calculate constraint plane perpendicular to view
    if (constraint != AxisConstraint::None && context) {
        Camera& camera = context->getCamera();
        glm::vec3 viewDir = glm::normalize(camera.getFront());

        // Plane normal perpendicular to both axis and view
        modalConstraintPlaneNormal = glm::cross(modalConstraintAxis, viewDir);
        float len = glm::length(modalConstraintPlaneNormal);

        if (len < 0.001f) {
            // View aligned with axis, use fallback
            modalConstraintPlaneNormal = glm::cross(modalConstraintAxis, glm::vec3(0, 1, 0));
            len = glm::length(modalConstraintPlaneNormal);
            if (len < 0.001f) {
                // Axis is world up, use world right
                modalConstraintPlaneNormal = glm::cross(modalConstraintAxis, glm::vec3(1, 0, 0));
            }
        }
        modalConstraintPlaneNormal = glm::normalize(modalConstraintPlaneNormal);
    }
}

} // namespace ohao
