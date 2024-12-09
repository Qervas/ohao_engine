#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    // Add small depth offset to prevent z-fighting
    gl_FragDepth = gl_FragCoord.z - 0.0001;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.viewPos - fragPos);
    vec3 L = normalize(ubo.lightPos - fragPos);

    // Double-sided lighting: flip normal if needed
    if (dot(N, V) < 0.0) {
        N = -N;
    }

    // Calculate light attenuation with a minimum threshold
    float distance = length(ubo.lightPos - fragPos);
    float attenuation = max(ubo.lightIntensity / (distance * distance), 0.1);

    // Direct lighting with energy conservation
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = fragColor * NdotL;

    // Stronger ambient term for better visibility of dark areas
    vec3 ambient = 0.3 * fragColor;

    // Add emissive contribution for light material
    vec3 emissive = vec3(0.0);
    if (fragColor == vec3(1.0)) { // Light material is white
        emissive = ubo.lightColor * ubo.lightIntensity;
    }

    // Combine lighting components
    vec3 color = ambient + diffuse * attenuation * ubo.lightColor + emissive;

    // HDR tone mapping
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
