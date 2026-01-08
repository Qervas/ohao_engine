#pragma once
#include <glm/glm.hpp>
#include "renderer/picking/picking_system.hpp"
#include "renderer/picking/ray.hpp"
#include "renderer/camera/camera.hpp"
#include "ui/selection/selection_manager.hpp"
#include "imgui.h"

namespace ohao {

class Window;
class Scene;
class VulkanContext;
class TransformGizmo;

// Gizmo operation modes
enum class GizmoMode {
    Translate,
    Rotate,
    Scale
};

// Input state machine states
enum class ViewportInputState {
    Idle,           // No active input
    CameraOrbit,    // Right-click dragging for camera orbit
    CameraPan,      // Middle-click dragging for camera pan
    GizmoDrag,      // Dragging a gizmo axis
    BoxSelect       // Future: box selection
};

// Gizmo axis identifiers
enum class GizmoAxis {
    None = 0,
    X = 1,
    Y = 2,
    Z = 4,
    XY = X | Y,
    XZ = X | Z,
    YZ = Y | Z,
    XYZ = X | Y | Z  // Uniform scale
};

// Enable bitwise operations for GizmoAxis
inline GizmoAxis operator|(GizmoAxis a, GizmoAxis b) {
    return static_cast<GizmoAxis>(static_cast<int>(a) | static_cast<int>(b));
}
inline GizmoAxis operator&(GizmoAxis a, GizmoAxis b) {
    return static_cast<GizmoAxis>(static_cast<int>(a) & static_cast<int>(b));
}
inline bool hasAxis(GizmoAxis composite, GizmoAxis single) {
    return (static_cast<int>(composite) & static_cast<int>(single)) != 0;
}

class ViewportInputHandler {
public:
    ViewportInputHandler() = default;
    ~ViewportInputHandler() = default;

    void initialize(
        VulkanContext* context,
        Window* window,
        PickingSystem* pickingSystem
    );

    // Called every frame from main.cpp (in edit mode only)
    void update(float deltaTime);

    // Viewport state setters (called by UIManager)
    void setViewportHovered(bool hovered) { isViewportHovered = hovered; }
    bool getViewportHovered() const { return isViewportHovered; }

    void setViewportBounds(const glm::vec2& min, const glm::vec2& max);
    glm::vec2 getViewportMin() const { return viewportMin; }
    glm::vec2 getViewportMax() const { return viewportMax; }
    glm::vec2 getViewportSize() const { return viewportSize; }

    // Play mode control (when true, this handler is disabled)
    void setPlayMode(bool playMode) { isPlayMode = playMode; }
    bool getPlayMode() const { return isPlayMode; }

    // Gizmo mode control
    void setGizmoMode(GizmoMode mode);
    GizmoMode getGizmoMode() const { return currentGizmoMode; }
    void cycleGizmoMode();

    // Current state accessors
    ViewportInputState getCurrentState() const { return currentState; }
    GizmoAxis getHoveredAxis() const { return hoveredAxis; }
    GizmoAxis getActiveAxis() const { return activeAxis; }

    // Transform gizmo reference (set externally)
    void setTransformGizmo(TransformGizmo* gizmo) { transformGizmo = gizmo; }
    TransformGizmo* getTransformGizmo() const { return transformGizmo; }

    // Camera orbit settings
    float orbitSensitivity = 0.3f;
    float panSensitivity = 0.01f;
    float zoomSensitivity = 1.0f;

private:
    VulkanContext* context = nullptr;
    Window* window = nullptr;
    PickingSystem* pickingSystem = nullptr;
    TransformGizmo* transformGizmo = nullptr;

    // State
    ViewportInputState currentState = ViewportInputState::Idle;
    GizmoMode currentGizmoMode = GizmoMode::Translate;
    bool isViewportHovered = false;
    bool isPlayMode = false;

    // Viewport bounds in screen coordinates
    glm::vec2 viewportMin{0.0f};
    glm::vec2 viewportMax{0.0f};
    glm::vec2 viewportSize{0.0f};

    // Camera orbit state
    glm::vec2 lastMousePos{0.0f};
    glm::vec3 orbitTarget{0.0f};  // Point to orbit around
    float orbitDistance = 5.0f;

    // Gizmo interaction state
    GizmoAxis hoveredAxis = GizmoAxis::None;
    GizmoAxis activeAxis = GizmoAxis::None;
    glm::vec3 dragStartPosition{0.0f};
    glm::vec3 dragStartScale{1.0f};
    glm::quat dragStartRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float dragStartAngle = 0.0f;
    glm::vec3 dragPlaneOrigin{0.0f};
    glm::vec3 dragPlaneNormal{0.0f, 1.0f, 0.0f};

    // State processing
    void processIdleState(float deltaTime);
    void processCameraOrbitState(float deltaTime);
    void processCameraPanState(float deltaTime);
    void processGizmoDragState(float deltaTime);

    // Input helpers
    glm::vec2 getMousePosInViewport() const;
    Ray getMouseRay() const;
    bool isMouseInViewport() const;

    // Event handlers
    void handleLeftClick();
    void handleLeftRelease();
    void handleRightClickStart();
    void handleRightClickEnd();
    void handleMiddleClickStart();
    void handleMiddleClickEnd();
    void handleKeyboardShortcuts();
    void handleMouseScroll(float scrollDelta);

    // Gizmo helpers
    void updateGizmoHover();
    void beginGizmoDrag();
    void updateGizmoDrag();
    void endGizmoDrag();

    // Camera helpers
    void updateCameraOrbit(const glm::vec2& mouseDelta);
    void updateCameraPan(const glm::vec2& mouseDelta);
    void updateCameraZoom(float scrollDelta);
    void focusOnSelection();
};

} // namespace ohao
