#version 450

// Physical sky fragment shader — Preetham day + enhanced procedural night sky
// Runs as a fullscreen pass AFTER deferred lighting.
// Discards geometry pixels (depth < 1.0) and fills sky pixels with:
//   Day:   Preetham analytical daylight model
//   Night: Milky Way band, colored spectral-class stars, phased moon disc
//   Blend: smooth twilight transition with horizon glow

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// GBuffer depth — used to identify sky pixels (depth == 1.0 means no geometry)
layout(set = 0, binding = 0) uniform sampler2D depthBuffer;

// Cloud buffer (half-res RGBA16F, GENERAL layout)
// RGB = in-scattered cloud radiance, A = transmittance (1=transparent, 0=opaque)
layout(set = 0, binding = 1) uniform sampler2D cloudBuffer;

// Push constants (128 bytes)
layout(push_constant) uniform PushConstants {
    mat4  invViewProj;    // offset   0: reconstruct world-space view dir
    vec3  sunDirection;   // offset  64: normalized, pointing TOWARD sun
    float turbidity;      // offset  76: atmosphere turbidity [1..10]
    vec3  cameraPos;      // offset  80: world-space camera position
    float sunIntensity;   // offset  92: overall sky brightness scale
    vec3  groundColor;    // offset  96: color of the ground hemisphere
    float nightFactor;    // offset 108: 0=day, 1=full night
    vec3  moonDirection;  // offset 112: normalized, pointing TOWARD moon
    float starSeed;       // offset 124: time-based seed for star twinkle
} pc;

const float PI = 3.14159265359;

// Galactic north pole — shared by Milky Way and star density boost
const vec3 GAL_NORTH = vec3(0.1277, 0.7955, 0.5921); // pre-normalized

// ---------------------------------------------------------------------------
// Hash functions for procedural content
// ---------------------------------------------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec2 hash22(vec2 p) {
    float n = hash21(p);
    return vec2(n, hash21(p + n));
}

// ---------------------------------------------------------------------------
// 3D hash + value noise + FBM (for Milky Way structure)
// Using 3D noise on viewDir avoids rectangular grid artifacts from 2D (lon,lat).
// ---------------------------------------------------------------------------
float hash31(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float valueNoise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash31(i);
    float n100 = hash31(i + vec3(1, 0, 0));
    float n010 = hash31(i + vec3(0, 1, 0));
    float n110 = hash31(i + vec3(1, 1, 0));
    float n001 = hash31(i + vec3(0, 0, 1));
    float n101 = hash31(i + vec3(1, 0, 1));
    float n011 = hash31(i + vec3(0, 1, 1));
    float n111 = hash31(i + vec3(1, 1, 1));

    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z
    );
}

float fbm3D(vec3 p) {
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        value += amp * valueNoise3D(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return value;
}

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
// ---------------------------------------------------------------------------
vec3 preethamSky(vec3 viewDir, vec3 sunDir, float T) {
    sunDir.y  = max(sunDir.y, 0.001);
    sunDir    = normalize(sunDir);
    float sunTheta    = acos(clamp(sunDir.y, 0.0, 1.0));
    float cosTheta = max(viewDir.y, 1e-4);
    float gamma    = acos(clamp(dot(viewDir, sunDir), -1.0, 1.0));

    float Ay = 0.1787*T - 1.4630, By = -0.3554*T + 0.4275;
    float Cy = -0.0227*T + 5.3251, Dy = 0.1206*T - 2.5771, Ey = -0.0670*T + 0.3703;
    float Ax = -0.0193*T - 0.2592, Bx = -0.0665*T + 0.0008;
    float Cx = -0.0004*T + 0.2125, Dx = -0.0641*T - 0.8989, Ex = -0.0033*T + 0.0452;
    float Ay2 = -0.0167*T - 0.2608, By2 = -0.0950*T + 0.0092;
    float Cy2 = -0.0079*T + 0.2102, Dy2 = -0.0441*T - 1.6537, Ey2 = -0.0109*T + 0.0529;

    float FY0  = perez(Ay, By, Cy, Dy, Ey,  1.0, sunTheta);
    float Fx0  = perez(Ax, Bx, Cx, Dx, Ex,  1.0, sunTheta);
    float Fy0  = perez(Ay2,By2,Cy2,Dy2,Ey2, 1.0, sunTheta);
    float FYs  = perez(Ay, By, Cy, Dy, Ey,  cosTheta, gamma);
    float Fxs  = perez(Ax, Bx, Cx, Dx, Ex,  cosTheta, gamma);
    float Fys  = perez(Ay2,By2,Cy2,Dy2,Ey2, cosTheta, gamma);

    float chi = (4.0/9.0 - T/120.0) * (PI - 2.0*sunTheta);
    float Yz  = max((4.0453*T - 4.9710) * tan(chi) - 0.2155*T + 2.4192, 0.0);
    float T2 = T*T;
    float s1 = sunTheta, s2 = s1*s1, s3 = s2*s1;

    float xz = T2*(0.00166*s3 - 0.00375*s2 + 0.00209*s1)
               + T*(-0.02903*s3 + 0.06377*s2 - 0.03202*s1 + 0.00394)
               + (0.11693*s3 - 0.21196*s2 + 0.06052*s1 + 0.25886);
    float yz = T2*(0.00275*s3 - 0.00610*s2 + 0.00317*s1)
               + T*(-0.04214*s3 + 0.08970*s2 - 0.04153*s1 + 0.00516)
               + (0.15346*s3 - 0.26756*s2 + 0.06670*s1 + 0.26688);

    float Y = Yz * (FYs / FY0);
    float x = xz * (Fxs / Fx0);
    float y = yz * (Fys / Fy0);
    float X = (y > 0.0) ? Y / y * x : 0.0;
    float Z = (y > 0.0) ? Y / y * (1.0 - x - y) : 0.0;

    vec3 rgb;
    rgb.r =  3.2406*X - 1.5372*Y - 0.4986*Z;
    rgb.g = -0.9689*X + 1.8758*Y + 0.0415*Z;
    rgb.b =  0.0557*X - 0.2040*Y + 1.0570*Z;
    return max(rgb, vec3(0.0)) * 0.05;
}

// ---------------------------------------------------------------------------
// Star spectral color (stellar classification O/B → M)
// Real distribution: ~76% M, ~12% K, ~8% G, ~3% F, ~1% A/B/O
// ---------------------------------------------------------------------------
vec3 spectralColor(float h) {
    if (h < 0.01) return vec3(0.65, 0.75, 1.0);    // O/B: blue-white (very hot)
    if (h < 0.04) return vec3(0.82, 0.88, 1.0);     // A: white-blue
    if (h < 0.07) return vec3(1.0, 0.98, 0.92);     // F: yellow-white
    if (h < 0.15) return vec3(1.0, 0.93, 0.72);     // G: yellow (sun-like)
    if (h < 0.27) return vec3(1.0, 0.82, 0.55);     // K: orange
    return vec3(1.0, 0.68, 0.45);                    // M: red-orange (most common)
}

// ---------------------------------------------------------------------------
// Milky Way band — procedural galactic plane with FBM structure + dark rifts
// ---------------------------------------------------------------------------
vec3 milkyWay(vec3 viewDir) {
    if (viewDir.y < -0.02) return vec3(0.0);

    // Galactic latitude: angular distance from galactic plane
    float galSin = dot(viewDir, GAL_NORTH);
    float galLat = asin(clamp(galSin, -1.0, 1.0));

    // Band profile: gaussian falloff from galactic plane
    float bandWidth = 0.18; // ~10° angular width
    float band = exp(-galLat * galLat / (2.0 * bandWidth * bandWidth));

    // 3D noise on viewDir — no rectangular grid artifacts from 2D (lon,lat)
    vec3 noisePos = viewDir * 7.0;
    float structure = fbm3D(noisePos + vec3(7.3, 2.1, 4.5));

    // Dark rifts: separate 3D noise layer creates the Great Rift
    float darkRifts = fbm3D(noisePos * 1.3 + vec3(13.7, 5.3, 8.1));
    structure *= smoothstep(0.25, 0.55, darkRifts);

    // Galactic core: brighter bulge centered at one direction
    vec3 galRight = normalize(cross(GAL_NORTH, vec3(0.0, 1.0, 0.0)));
    vec3 galFwd   = cross(galRight, GAL_NORTH);
    float galLon  = atan(dot(viewDir, galRight), dot(viewDir, galFwd));
    float coreAngle = galLon - 0.8;
    float core = exp(-coreAngle * coreAngle / 0.4);
    core *= exp(-galLat * galLat / (2.0 * 0.12 * 0.12)); // core is narrower

    // Combine
    float brightness = band * (0.15 + 0.85 * structure) * (1.0 + core * 3.0);
    brightness *= 0.035;

    // Color: warmer near galactic core, cooler blue in spiral arms
    vec3 coreColor = vec3(1.0, 0.92, 0.75);
    vec3 armColor  = vec3(0.75, 0.82, 1.0);
    vec3 color = mix(armColor, coreColor, core * 0.7) * brightness;

    // Atmospheric extinction near horizon
    color *= smoothstep(0.0, 0.12, viewDir.y);

    return color;
}

// ---------------------------------------------------------------------------
// Procedural star field — spectral colors + magnitude distribution
// Returns colored star contribution for a given view direction.
// Bright anchor stars (~0.3%) get a visible glow halo.
// Star density increases near the galactic plane (Milky Way).
// ---------------------------------------------------------------------------
vec3 starField(vec3 viewDir, float seed) {
    if (viewDir.y < -0.02) return vec3(0.0);

    // Spherical UV
    float phi   = atan(viewDir.z, viewDir.x);
    float theta = acos(clamp(viewDir.y, -1.0, 1.0));
    vec2 uv = vec2(phi / (2.0 * PI) + 0.5, theta / PI);

    // Higher-resolution grid (300x150 = 45k cells)
    vec2 gridSize = vec2(300.0, 150.0);
    vec2 cell   = floor(uv * gridSize);
    vec2 cellUV = fract(uv * gridSize);

    // More stars near galactic plane (the Milky Way IS unresolved stars)
    float galLat = abs(asin(clamp(dot(viewDir, GAL_NORTH), -1.0, 1.0)));
    float galDensityBoost = mix(1.8, 1.0, smoothstep(0.0, 0.4, galLat));

    vec3 stars = vec3(0.0);

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            vec2 neighbor = cell + vec2(dx, dy);
            vec2 rnd = hash22(neighbor);

            // Density: ~5% base, boosted near galactic plane
            float threshold = 0.05 * galDensityBoost;
            if (rnd.x > threshold) continue;

            // Magnitude: position within the probability range
            // 0 = brightest (rarest), 1 = dimmest (most common)
            float magNorm = rnd.x / threshold;

            // Size and brightness scale with magnitude (quadratic brightness falloff)
            float starSize = mix(0.06, 0.012, magNorm);
            float baseBrightness = mix(1.2, 0.15, magNorm * magNorm);

            // Star position within cell
            vec2 starPos = vec2(hash21(neighbor + 7.1), hash21(neighbor + 13.7));
            vec2 diff = (cellUV - starPos) - vec2(dx, dy);
            float dist = length(diff);

            if (dist < starSize * 3.5) {
                float brightness = smoothstep(starSize, starSize * 0.2, dist);
                brightness *= baseBrightness;

                // Bright stars (~top 15%) get a soft outer glow halo
                if (magNorm < 0.15 && dist >= starSize * 0.2) {
                    float glowBright = smoothstep(starSize * 3.5, starSize * 0.5, dist) * 0.12;
                    brightness += glowBright;
                }

                // Twinkle (atmospheric scintillation — dim stars twinkle more)
                float twinklePhase = hash21(neighbor + 77.7) * 6.283;
                float twinkleSpeed = mix(0.8, 2.5, hash21(neighbor + 99.9));
                float twinkleAmt   = mix(0.08, 0.35, magNorm);
                float twinkle = 1.0 - twinkleAmt + twinkleAmt * sin(seed * twinkleSpeed + twinklePhase);
                brightness *= twinkle;

                // Spectral color from stellar classification
                vec3 color = spectralColor(hash21(neighbor + 42.0));
                stars = max(stars, brightness * color);
            }
        }
    }

    // Atmospheric extinction near horizon
    return stars * smoothstep(0.0, 0.15, viewDir.y);
}

// ---------------------------------------------------------------------------
// Moon disc with phase illumination, earthshine, and limb darkening
// Phase is computed from the sun-moon angle:
//   sun opposite moon → full moon (lit face toward camera)
//   sun behind moon   → new moon  (dark face toward camera)
// Disc brightness 4.0 is safe with Karis average in bloom downsample.
// ---------------------------------------------------------------------------
vec3 moonDisc(vec3 viewDir, vec3 moonDir, vec3 sunDir) {
    if (moonDir.y < -0.05) return vec3(0.0);

    float cosAngle = dot(viewDir, moonDir);

    // Disc mask
    float disc = smoothstep(0.9994, 0.9998, cosAngle);

    // Atmospheric glow (single continuous falloff)
    float rawGlow = smoothstep(0.985, 0.9998, cosAngle);
    float glow = pow(rawGlow, 1.5) * 0.5;

    // ── Phase illumination ──────────────────────────────────────
    // Build tangent frame on the moon disc
    vec3 t1 = (abs(moonDir.y) > 0.999)
            ? normalize(cross(moonDir, vec3(1.0, 0.0, 0.0)))
            : normalize(cross(moonDir, vec3(0.0, 1.0, 0.0)));
    vec3 t2 = cross(moonDir, t1);

    // Position on disc in tangent plane coordinates
    vec3  offset     = viewDir - moonDir * cosAngle;
    float u          = dot(offset, t1);
    float v          = dot(offset, t2);
    float discRadius = sqrt(1.0 - 0.9994 * 0.9994); // ~0.0346 rad
    float uN         = clamp(u / discRadius, -1.0, 1.0);
    float vN         = clamp(v / discRadius, -1.0, 1.0);
    float rSq        = min(uN * uN + vN * vN, 1.0);
    float zN         = sqrt(1.0 - rSq);

    // Surface normal on the sphere (pointing toward viewer at center)
    vec3 surfaceN = -moonDir * zN + t1 * uN + t2 * vN;

    // Illumination: surface faces the sun?
    float NdotL = dot(surfaceN, sunDir);
    float illumination = smoothstep(-0.05, 0.05, NdotL);

    // Earthshine: faint blue glow on the unlit side (reflected Earth-light)
    float earthshine = (1.0 - illumination) * 0.06;

    // Limb darkening: subtle center-to-edge brightness falloff
    float limb = 0.8 + 0.2 * zN;

    vec3 moonColor      = vec3(0.95, 0.93, 0.88);
    vec3 earthshineClr  = vec3(0.35, 0.45, 0.65);
    vec3 glowColor      = vec3(0.7, 0.75, 0.85);

    // Disc: lit surface + faint earthshine on dark side
    vec3 discLight = disc * moonColor * 4.0 * (illumination * limb + earthshine)
                   + disc * earthshineClr * earthshine * 0.5;

    // Glow: atmospheric scattering scales with illuminated fraction
    // full moon (illumFrac=1) → full glow, new moon (illumFrac=0) → minimal glow
    float illumFrac = clamp(0.5 - 0.5 * dot(sunDir, moonDir), 0.0, 1.0);
    vec3  glowLight = glow * glowColor * max(illumFrac, 0.08);

    return discLight + glowLight;
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
    float nf       = clamp(pc.nightFactor, 0.0, 1.0);
    vec3  moonDir  = normalize(pc.moonDirection);

    // ── Daytime sky (Preetham) ────────────────────────────────────────────
    vec3 daySky = vec3(0.0);
    if (viewDir.y > -0.05) {
        vec3 sd = viewDir;
        sd.y = max(sd.y, 0.001);
        sd   = normalize(sd);
        daySky = preethamSky(sd, sunDir, T) * pc.sunIntensity;

        // Warm horizon glow during daytime
        float horizonBlend = smoothstep(0.15, 0.0, viewDir.y);
        float sunVisibility = max(sunHeight, 0.0);
        vec3  horizonGlow   = vec3(0.9, 0.55, 0.3) * pc.sunIntensity * 0.25 * sunVisibility;
        daySky = mix(daySky, daySky * 0.6 + horizonGlow, horizonBlend * 0.5);
    } else {
        daySky = pc.groundColor;
    }

    // ── Night sky ─────────────────────────────────────────────────────────
    // Dark blue gradient: darker at zenith, slightly lighter at horizon
    vec3 nightZenith  = vec3(0.002, 0.004, 0.015);
    vec3 nightHorizon = vec3(0.008, 0.012, 0.035);
    float horizonBlendN = smoothstep(0.5, 0.0, max(viewDir.y, 0.0));
    vec3  nightGradient = mix(nightZenith, nightHorizon, horizonBlendN);

    // Below-horizon: gradual fade to dark ground
    if (viewDir.y < 0.02) {
        nightGradient = mix(nightGradient, vec3(0.001, 0.002, 0.005),
                            smoothstep(0.02, -0.25, viewDir.y));
    }

    // Milky Way band (diffuse galactic glow with dark rifts)
    nightGradient += milkyWay(viewDir);

    // Colored stars with realistic magnitude distribution
    vec3 stars = starField(viewDir, pc.starSeed);

    // Suppress stars within moon glow zone (moonlight washes them out)
    float moonProx = dot(viewDir, moonDir);
    stars *= 1.0 - smoothstep(0.92, 0.983, moonProx);

    nightGradient += stars;

    // NOTE: Moon is added AFTER the day/night mix (below), not here.
    // Adding it to nightGradient would cause it to be scaled by nf^2
    // (once here, once from the mix), creating a nonlinear pop instead
    // of a smooth fade-in.

    // ── Twilight glow ─────────────────────────────────────────────────────
    // When sun is near the horizon, warm orange glow in the sun's direction
    vec3 twilight = vec3(0.0);
    if (sunHeight > -0.2 && sunHeight < 0.1 && viewDir.y > -0.05) {
        float twilightStrength = 1.0 - smoothstep(0.0, 0.1, abs(sunHeight));
        twilightStrength *= smoothstep(-0.2, -0.05, sunHeight);

        vec3 sunAzimuth = normalize(vec3(sunDir.x, 0.0, sunDir.z));
        vec3 viewAzimuth = normalize(vec3(viewDir.x, 0.0, viewDir.z));
        float azimuthDot = max(dot(viewAzimuth, sunAzimuth), 0.0);
        float dirFactor = pow(azimuthDot, 3.0);
        float vertFactor = smoothstep(0.3, 0.0, max(viewDir.y, 0.0));

        vec3 warmGlow = vec3(0.8, 0.35, 0.1) * 0.15;
        twilight = warmGlow * twilightStrength * dirFactor * vertFactor;
    }

    // ── Blend day/night ───────────────────────────────────────────────────
    vec3 skyColor = mix(daySky, nightGradient, nf);
    skyColor += twilight;

    // ── Moon disc + phase (added after mix, scaled by nf once) ────────────
    skyColor += moonDisc(viewDir, moonDir, sunDir) * nf;

    // ── Sun disc (only above horizon, fades during twilight) ──────────────
    if (sunHeight > -0.05 && viewDir.y > -0.05) {
        float sunFade = smoothstep(-0.02, 0.08, sunHeight);
        float cosGamma  = dot(viewDir, sunDir);
        float sunDisc   = smoothstep(0.9994, 0.9998, cosGamma);
        float sunCorona = smoothstep(0.985, 0.9994, cosGamma) * 0.3;
        vec3  sunColor  = vec3(1.0, 0.92, 0.75) * pc.sunIntensity;
        skyColor += (sunDisc * 40.0 + sunCorona * 2.0) * sunColor * sunFade;
    }

    // Composite clouds over sky.
    // Skip when fully transparent (a >= 0.999) to avoid FP16 precision artifacts.
    vec4 cloudSample = texture(cloudBuffer, inTexCoord);
    if (cloudSample.a < 0.999) {
        skyColor = skyColor * cloudSample.a + max(cloudSample.rgb, vec3(0.0));
    }

    outColor = vec4(skyColor, 1.0);
}
