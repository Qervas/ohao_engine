#version 450

// Physical sky fragment shader — Preetham analytical daylight model
// Runs as a fullscreen pass AFTER deferred lighting.
// Discards geometry pixels (depth < 1.0) and fills sky pixels with
// a physically-based sky color derived from sun position + turbidity.

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// GBuffer depth — used to identify sky pixels (depth == 1.0 means no geometry)
layout(set = 0, binding = 0) uniform sampler2D depthBuffer;

// Push constants (112 bytes, well under 256B NVIDIA limit)
layout(push_constant) uniform PushConstants {
    mat4  invViewProj;   // offset   0: reconstruct world-space view dir
    vec3  sunDirection;  // offset  64: normalized, pointing TOWARD sun
    float turbidity;     // offset  76: atmosphere turbidity [1..10]
    vec3  cameraPos;     // offset  80: world-space camera position
    float sunIntensity;  // offset  92: overall sky brightness scale
    vec3  groundColor;   // offset  96: color of the ground hemisphere
    float pad0;          // offset 108
} pc;

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// Preetham Perez distribution function
// F(theta, gamma) = (1 + A*exp(B/cos(theta))) * (1 + C*exp(D*gamma) + E*cos^2(gamma))
// ---------------------------------------------------------------------------
float perez(float A, float B, float C, float D, float E, float cosTheta, float gamma) {
    float cosGamma = cos(gamma);
    float safeInvCos = 1.0 / max(cosTheta, 1e-4);
    return (1.0 + A * exp(B * safeInvCos)) * (1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma);
}

// ---------------------------------------------------------------------------
// Preetham sky model
// Returns linear RGB sky radiance for a given view direction and sun position.
// T = turbidity [1..10];  sunDir must point TOWARD the sun, sunDir.y > 0.
// ---------------------------------------------------------------------------
vec3 preethamSky(vec3 viewDir, vec3 sunDir, float T) {
    // Clamp sun above horizon to keep model valid
    sunDir.y  = max(sunDir.y, 0.001);
    sunDir    = normalize(sunDir);
    float sunTheta    = acos(clamp(sunDir.y, 0.0, 1.0));
    float cosSunTheta = sunDir.y;

    // Clamp view above very small angle from nadir (prevents cos(theta)=0 singularity)
    float cosTheta = max(viewDir.y, 1e-4);
    float gamma    = acos(clamp(dot(viewDir, sunDir), -1.0, 1.0));

    // --- Perez coefficients (luminance Y) ---
    float Ay = 0.1787*T - 1.4630;
    float By = -0.3554*T + 0.4275;
    float Cy = -0.0227*T + 5.3251;
    float Dy =  0.1206*T - 2.5771;
    float Ey = -0.0670*T + 0.3703;

    // --- Perez coefficients (chromaticity x) ---
    float Ax = -0.0193*T - 0.2592;
    float Bx = -0.0665*T + 0.0008;
    float Cx = -0.0004*T + 0.2125;
    float Dx = -0.0641*T - 0.8989;
    float Ex = -0.0033*T + 0.0452;

    // --- Perez coefficients (chromaticity y) ---
    float Ay2 = -0.0167*T - 0.2608;
    float By2 = -0.0950*T + 0.0092;
    float Cy2 = -0.0079*T + 0.2102;
    float Dy2 = -0.0441*T - 1.6537;
    float Ey2 = -0.0109*T + 0.0529;

    // Normalization denominators: F(theta=0, gamma=sunTheta)
    float FY0  = perez(Ay, By, Cy, Dy, Ey,  1.0, sunTheta);
    float Fx0  = perez(Ax, Bx, Cx, Dx, Ex,  1.0, sunTheta);
    float Fy0  = perez(Ay2,By2,Cy2,Dy2,Ey2, 1.0, sunTheta);

    // Sample values
    float FYs  = perez(Ay, By, Cy, Dy, Ey,  cosTheta, gamma);
    float Fxs  = perez(Ax, Bx, Cx, Dx, Ex,  cosTheta, gamma);
    float Fys  = perez(Ay2,By2,Cy2,Dy2,Ey2, cosTheta, gamma);

    // Zenith luminance (kcd/m^2)
    float chi = (4.0/9.0 - T/120.0) * (PI - 2.0*sunTheta);
    float Yz  = max((4.0453*T - 4.9710) * tan(chi) - 0.2155*T + 2.4192, 0.0);

    // Zenith chromaticity
    float T2 = T*T;
    float s1 = sunTheta, s2 = s1*s1, s3 = s2*s1;

    float xz = T2*(0.00166*s3 - 0.00375*s2 + 0.00209*s1)
               + T*(-0.02903*s3 + 0.06377*s2 - 0.03202*s1 + 0.00394)
               + (0.11693*s3 - 0.21196*s2 + 0.06052*s1 + 0.25886);

    float yz = T2*(0.00275*s3 - 0.00610*s2 + 0.00317*s1)
               + T*(-0.04214*s3 + 0.08970*s2 - 0.04153*s1 + 0.00516)
               + (0.15346*s3 - 0.26756*s2 + 0.06670*s1 + 0.26688);

    // Final CIE xyY
    float Y = Yz * (FYs / FY0);
    float x = xz * (Fxs / Fx0);
    float y = yz * (Fys / Fy0);

    // CIE xyY -> XYZ -> linear sRGB (D65)
    float X = (y > 0.0) ? Y / y * x : 0.0;
    float Z = (y > 0.0) ? Y / y * (1.0 - x - y) : 0.0;

    vec3 rgb;
    rgb.r =  3.2406*X - 1.5372*Y - 0.4986*Z;
    rgb.g = -0.9689*X + 1.8758*Y + 0.0415*Z;
    rgb.b =  0.0557*X - 0.2040*Y + 1.0570*Z;

    // Scale from kcd/m^2 to a tonemappable HDR range
    return max(rgb, vec3(0.0)) * 0.05;
}

// ---------------------------------------------------------------------------
void main() {
    // Skip geometry pixels (depth < 1.0 means geometry was written by GBuffer)
    float depth = texture(depthBuffer, inTexCoord).r;
    if (depth < 0.9999) {
        discard;
    }

    // Reconstruct world-space view direction from NDC at far plane (z=1 in Vulkan)
    vec2  ndc      = inTexCoord * 2.0 - 1.0;
    vec4  worldFar = pc.invViewProj * vec4(ndc, 1.0, 1.0);
    worldFar /= worldFar.w;
    vec3  viewDir  = normalize(worldFar.xyz - pc.cameraPos);

    vec3  sunDir   = normalize(pc.sunDirection);
    float sunHeight = sunDir.y;   // 1=zenith, 0=horizon, -1=nadir
    float T        = clamp(pc.turbidity, 1.0, 10.0);

    vec3 skyColor;

    if (viewDir.y > -0.05) {
        // Above/near horizon: Preetham model
        vec3 sd = viewDir;
        sd.y = max(sd.y, 0.001);
        sd   = normalize(sd);
        skyColor = preethamSky(sd, sunDir, T) * pc.sunIntensity;

        // Warm horizon glow (blends in when view is near horizon)
        float horizonBlend = smoothstep(0.15, 0.0, viewDir.y);
        float sunVisibility = max(sunHeight, 0.0);
        vec3  horizonGlow   = vec3(0.9, 0.55, 0.3) * pc.sunIntensity * 0.25 * sunVisibility;
        skyColor = mix(skyColor, skyColor * 0.6 + horizonGlow, horizonBlend * 0.5);
    } else {
        // Below horizon: ground color with slight fog blend
        skyColor = pc.groundColor;
    }

    // Fade to night sky when sun dips below horizon
    if (sunHeight < 0.1) {
        float nightBlend = smoothstep(0.1, -0.2, sunHeight);
        vec3  nightSky   = vec3(0.003, 0.006, 0.025);
        skyColor = mix(skyColor, nightSky, nightBlend);
    }

    // Sun disk + corona (only when sun is visible)
    if (sunHeight > -0.05 && viewDir.y > -0.05) {
        float cosGamma    = dot(viewDir, sunDir);
        float sunDisc     = smoothstep(0.9997, 0.9999, cosGamma);          // ~0.8° radius
        float sunCorona   = smoothstep(0.990, 0.9997, cosGamma) * 0.4;    // soft glow
        vec3  sunColor    = vec3(1.0, 0.92, 0.75) * pc.sunIntensity;
        float sunFade     = smoothstep(0.0, 0.08, sunHeight);
        skyColor += (sunDisc * 60.0 + sunCorona * 3.0) * sunColor * sunFade;
    }

    outColor = vec4(skyColor, 1.0);
}
