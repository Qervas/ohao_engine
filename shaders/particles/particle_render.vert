#version 450

// Particle Render Vertex Shader
// Generates camera-facing billboard quads from particle data

struct Particle {
    vec4 position;  // xyz = position, w = current lifetime
    vec4 velocity;  // xyz = velocity, w = maxLifetime
    vec4 color;     // rgba
    vec4 params;    // x = size, y = rotation, z = type, w = alive
};

layout(std430, set = 0, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(push_constant) uniform RenderParams {
    mat4 viewProj;
    vec3 cameraRight;
    float pad1;
    vec3 cameraUp;
    float pad2;
} render;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

// Billboard quad vertices (2 triangles = 6 vertices per particle)
const vec2 quadVertices[6] = vec2[](
    vec2(-0.5, -0.5),
    vec2( 0.5, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

void main() {
    uint particleIndex = gl_VertexIndex / 6;
    uint vertexInQuad = gl_VertexIndex % 6;

    Particle p = particles[particleIndex];

    // Skip dead particles (move off-screen)
    if (p.params.w < 0.5) {
        gl_Position = vec4(0.0, 0.0, -10.0, 1.0);
        return;
    }

    vec2 quadPos = quadVertices[vertexInQuad];
    float size = p.params.x;

    // Billboard: expand quad in camera space
    vec3 worldPos = p.position.xyz +
                    render.cameraRight * quadPos.x * size +
                    render.cameraUp * quadPos.y * size;

    gl_Position = render.viewProj * vec4(worldPos, 1.0);
    fragColor = p.color;
    fragUV = quadPos + 0.5; // Map to [0, 1]
}
