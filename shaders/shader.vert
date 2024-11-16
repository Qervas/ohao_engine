#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) out vec3 fragColor;

// Room vertices (Cornell box-like setup)
vec3 positions[] = vec3[](
        // Back wall
        vec3(-3.0, -3.0, -3.0),
        vec3(3.0, -3.0, -3.0),
        vec3(3.0, 3.0, -3.0),
        vec3(-3.0, -3.0, -3.0),
        vec3(3.0, 3.0, -3.0),
        vec3(-3.0, 3.0, -3.0),

        // Left wall (red)
        vec3(-3.0, -3.0, 3.0),
        vec3(-3.0, -3.0, -3.0),
        vec3(-3.0, 3.0, -3.0),
        vec3(-3.0, -3.0, 3.0),
        vec3(-3.0, 3.0, -3.0),
        vec3(-3.0, 3.0, 3.0),

        // Right wall (green)
        vec3(3.0, -3.0, -3.0),
        vec3(3.0, -3.0, 3.0),
        vec3(3.0, 3.0, 3.0),
        vec3(3.0, -3.0, -3.0),
        vec3(3.0, 3.0, 3.0),
        vec3(3.0, 3.0, -3.0),

        // Floor
        vec3(-3.0, -3.0, 3.0),
        vec3(3.0, -3.0, 3.0),
        vec3(3.0, -3.0, -3.0),
        vec3(-3.0, -3.0, 3.0),
        vec3(3.0, -3.0, -3.0),
        vec3(-3.0, -3.0, -3.0),

        // Ceiling
        vec3(-3.0, 3.0, -3.0),
        vec3(3.0, 3.0, -3.0),
        vec3(3.0, 3.0, 3.0),
        vec3(-3.0, 3.0, -3.0),
        vec3(3.0, 3.0, 3.0),
        vec3(-3.0, 3.0, 3.0)
    );

vec3 colors[] = vec3[](
        // Back wall (white)
        vec3(0.2, 0.2, 0.8), vec3(0.2, 0.2, 0.8), vec3(0.2, 0.2, 0.8),
        vec3(0.2, 0.2, 0.8), vec3(0.2, 0.2, 0.8), vec3(0.2, 0.2, 0.8),

        // Left wall (red)
        vec3(0.75, 0.25, 0.25), vec3(0.75, 0.25, 0.25), vec3(0.75, 0.25, 0.25),
        vec3(0.75, 0.25, 0.25), vec3(0.75, 0.25, 0.25), vec3(0.75, 0.25, 0.25),

        // Right wall (green)
        vec3(0.25, 0.75, 0.25), vec3(0.25, 0.75, 0.25), vec3(0.25, 0.75, 0.25),
        vec3(0.25, 0.75, 0.25), vec3(0.25, 0.75, 0.25), vec3(0.25, 0.75, 0.25),

        // Floor (purple)
        vec3(0.5, 0.0, 0.5), vec3(0.5, 0.0, 0.5), vec3(0.5, 0.0, 0.5),
        vec3(0.5, 0.0, 0.5), vec3(0.5, 0.0, 0.5), vec3(0.5, 0.0, 0.5),

        // Ceiling (brown)
        vec3(0.5, 0.25, 0.0), vec3(0.5, 0.25, 0.0), vec3(0.5, 0.25, 0.0),
        vec3(0.5, 0.25, 0.0), vec3(0.5, 0.25, 0.0), vec3(0.5, 0.25, 0.0)

    );

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(positions[gl_VertexIndex], 1.0);
    fragColor = colors[gl_VertexIndex];
}
