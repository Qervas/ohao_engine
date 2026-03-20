#version 460
#extension GL_EXT_ray_tracing : require

// Miss Shader for GI — ray escaped the scene, no indirect contribution
layout(location = 0) rayPayloadInEXT vec4 giPayload;

void main() {
    giPayload = vec4(0.0, 0.0, 0.0, -1.0);  // miss marker
}
