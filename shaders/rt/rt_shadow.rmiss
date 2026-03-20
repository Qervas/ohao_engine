#version 460
#extension GL_EXT_ray_tracing : require

// Miss Shader — ray didn't hit anything = pixel is lit (not shadowed)

layout(location = 0) rayPayloadInEXT float shadowPayload;

void main() {
    shadowPayload = 1.0;  // no occlusion = fully lit
}
