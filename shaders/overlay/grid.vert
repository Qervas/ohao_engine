#version 450

// Fullscreen triangle - no vertex input needed
// Generates 3 vertices that cover the entire screen

layout(location = 0) out vec2 fragUV;

void main() {
    // Generate fullscreen triangle
    fragUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUV * 2.0 - 1.0, 0.0, 1.0);
}
