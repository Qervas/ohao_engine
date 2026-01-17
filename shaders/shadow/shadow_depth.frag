#version 450

// shadow_depth.frag - Shadow map depth pass fragment shader
// Part of OHAO Engine shader system
// Location: shadow/shadow_depth.frag
//
// Depth-only pass for shadow map generation.
// The depth is automatically written from gl_FragCoord.z

void main() {
    // Depth is automatically written from the vertex shader's gl_Position.z
    // No explicit output needed for depth-only pass
}
