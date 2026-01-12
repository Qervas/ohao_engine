#pragma once

namespace ohao {

// Gizmo operation modes
enum class GizmoMode {
    Translate,
    Rotate,
    Scale
};

// Which axis/component of the gizmo is being interacted with
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

} // namespace ohao
