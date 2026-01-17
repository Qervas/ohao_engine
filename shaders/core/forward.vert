#version 450

// forward.vert - Forward rendering vertex shader
// Part of OHAO Engine shader system
// Location: core/forward.vert

// Vertex inputs
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
// Future: location 4 = tangent (vec4, w = handedness)

// Vertex outputs
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragPos;
layout(location = 3) out vec2 fragTexCoord;

// Camera matrices (binding 0)
layout(binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} camera;

// Per-object model transform and material (push constant)
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} pc;

void main() {
    // Transform to world space
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    // Transform normal to world space
    // Using the normal matrix (inverse transpose of model matrix)
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    // Pass through texture coordinates
    fragTexCoord = inTexCoord;

    // Combine material base color with vertex color
    // Vertex color of (1,1,1) means "use material color only"
    fragColor = pc.baseColor * inColor;

    // Transform to clip space
    gl_Position = camera.proj * camera.view * worldPos;
}
