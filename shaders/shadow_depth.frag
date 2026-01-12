#version 450

// Shadow depth fragment shader
// The depth is automatically written from gl_FragCoord.z
// We don't need to do anything here - just let the hardware write depth
void main() {
    // Depth is automatically written from the vertex shader's gl_Position.z
    // No explicit output needed for depth-only pass
}
