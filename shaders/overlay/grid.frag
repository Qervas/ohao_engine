#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputImage;
layout(set = 0, binding = 1) uniform sampler2D depthImage;

layout(push_constant) uniform GridParams {
    mat4 invViewProj;
    vec4 cameraPos;    // xyz = camera position, w = unused
    vec4 gridParams;   // x = major spacing, y = minor spacing, z = fade distance, w = line width
} params;

// Reconstruct world position from depth buffer
vec3 reconstructWorldPos(vec2 uv, float depth) {
    // Convert UV to clip space (-1 to 1)
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    // Invert the projection
    vec4 worldPos = params.invViewProj * clipPos;
    return worldPos.xyz / worldPos.w;
}

// Grid line intensity using screen-space derivatives for anti-aliasing
float gridLine(vec2 worldXZ, float spacing, float width) {
    vec2 grid = abs(fract(worldXZ / spacing - 0.5) - 0.5) / fwidth(worldXZ / spacing);
    float line = min(grid.x, grid.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    // Sample the input image (pass-through)
    vec4 sceneColor = texture(inputImage, fragUV);

    // Sample depth
    float depth = texture(depthImage, fragUV).r;

    // If depth is at far plane (no geometry), reconstruct a ground plane intersection
    // For the grid we only draw on the Y=0 plane

    vec3 worldPos = reconstructWorldPos(fragUV, depth);

    // Camera ray for ground plane intersection
    vec3 camPos = params.cameraPos.xyz;
    vec3 rayDir = normalize(worldPos - camPos);

    float majorSpacing = params.gridParams.x;   // e.g., 10.0
    float minorSpacing = params.gridParams.y;    // e.g., 1.0
    float fadeDistance = params.gridParams.z;     // e.g., 100.0

    // Intersect ray with Y=0 plane
    float t = -camPos.y / rayDir.y;
    bool hitGround = (t > 0.0 && (depth >= 0.999 || worldPos.y < 0.01));

    if (!hitGround && abs(worldPos.y) > 0.1) {
        outColor = sceneColor;
        return;
    }

    vec3 gridPos;
    if (depth >= 0.999) {
        // No geometry - use ground plane intersection
        if (t <= 0.0) {
            outColor = sceneColor;
            return;
        }
        gridPos = camPos + rayDir * t;
    } else {
        gridPos = worldPos;
    }

    // Only draw grid near Y=0
    float yFade = 1.0 - smoothstep(0.0, 2.0, abs(gridPos.y));
    if (yFade < 0.001) {
        outColor = sceneColor;
        return;
    }

    vec2 xz = gridPos.xz;

    // Distance from camera (for fade)
    float dist = length(gridPos - camPos);
    float distFade = 1.0 - smoothstep(fadeDistance * 0.5, fadeDistance, dist);

    // Minor grid lines
    float minor = gridLine(xz, minorSpacing, 1.0) * 0.3;

    // Major grid lines
    float major = gridLine(xz, majorSpacing, 1.5) * 0.6;

    // Axis lines (X = red, Z = blue)
    float axisWidth = 0.05;
    float xAxis = 1.0 - smoothstep(0.0, axisWidth, abs(xz.y));  // Z coordinate near 0 → X axis
    float zAxis = 1.0 - smoothstep(0.0, axisWidth, abs(xz.x));  // X coordinate near 0 → Z axis

    // Combine grid
    float gridIntensity = max(minor, major);
    vec3 gridColor = vec3(0.5) * gridIntensity;

    // Apply axis colors
    gridColor = mix(gridColor, vec3(0.8, 0.2, 0.2), xAxis * 0.8);  // Red for X axis
    gridColor = mix(gridColor, vec3(0.2, 0.2, 0.8), zAxis * 0.8);  // Blue for Z axis

    float totalIntensity = max(max(gridIntensity, xAxis * 0.8), zAxis * 0.8);
    float alpha = totalIntensity * distFade * yFade;

    // Composite grid over scene
    outColor = vec4(mix(sceneColor.rgb, gridColor, alpha), sceneColor.a);
}
