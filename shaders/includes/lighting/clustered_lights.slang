// The forward copy of the deferred lighting loop (sub-project 12, OPEN ruling 19): the clustered
// light lookup, the attenuation and the Cook-Torrance BRDF of core/deferred_lighting.frag and
// includes/brdf/brdf_ggx.glsl, the same expressions in Slang for vfx.expand (particles, one sample
// at their centre) and the transparent pass's meshes (vfx/transparent_mesh.slang, per pixel). The
// GLSL is untouched, so no deferred golden moves; a parity test (vfx_gpu_test) pins the two: a
// forward-shaded opaque plane equals the deferred plane within 1 LSB with the RT terms off.
//
// The caller binds the light table and the froxel lists (LightGpu, kClusterCount counts then
// kClusterMaxLights indices a froxel) and passes them in.
// import includes.lighting.clustered_lights;
import shared.layout;

static const float kPi = 3.14159265359;
static const float kInvPi = 0.31830988618;
static const float kEpsilon = 1e-6;

// ---- brdf_common.glsl ----------------------------------------------------------------------

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 calculateF0(float3 albedo, float metallic)
{
    const float sharpened = smoothstep(0.35, 0.65, metallic);
    return lerp(float3(0.04), albedo, sharpened);
}

float3 burleyDiffuse(float3 diffuseColor, float roughness, float NdotV, float NdotL, float LdotH)
{
    const float f90 = 0.5 + 2.0 * roughness * LdotH * LdotH;
    const float lightScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotL, 5.0);
    const float viewScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotV, 5.0);
    return diffuseColor * kInvPi * lightScatter * viewScatter;
}

struct BRDFSurface
{
    float3 position;
    float3 normal;
    float3 viewDir;
    float3 albedo;
    float metallic;
    float roughness;
    float ao;
    float3 F0;
};

BRDFSurface initBRDFSurface(float3 position, float3 normal, float3 viewDir, float3 albedo, float metallic,
                            float roughness, float ao)
{
    BRDFSurface s;
    s.position = position;
    s.normal = normalize(normal);
    s.viewDir = normalize(viewDir);
    s.albedo = albedo;
    s.metallic = smoothstep(0.35, 0.65, metallic);
    s.roughness = max(roughness, 0.04);
    s.ao = ao;
    s.F0 = calculateF0(albedo, metallic);
    return s;
}

// ---- brdf_ggx.glsl -------------------------------------------------------------------------

float distributionGGX(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = kPi * denom * denom;
    return a2 / max(denom, kEpsilon);
}

float geometrySmithCorrelated(float NdotV, float NdotL, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float lambdaV = NdotL * sqrt(a2 + (1.0 - a2) * NdotV * NdotV);
    const float lambdaL = NdotV * sqrt(a2 + (1.0 - a2) * NdotL * NdotL);
    return 0.5 / max(lambdaV + lambdaL, kEpsilon);
}

float3 evaluateSpecularBRDF(float3 N, float3 V, float3 L, float roughness, float3 F0, out float3 F)
{
    const float3 H = normalize(V + L);
    const float NdotV = max(dot(N, V), kEpsilon);
    const float NdotL = max(dot(N, L), kEpsilon);
    const float NdotH = max(dot(N, H), 0.0);
    const float HdotV = max(dot(H, V), 0.0);
    const float D = distributionGGX(NdotH, roughness);
    const float Vt = geometrySmithCorrelated(NdotV, NdotL, roughness);
    F = fresnelSchlickRoughness(HdotV, F0, roughness);
    return D * Vt * F;
}

float GGX_E(float cosTheta, float roughness)
{
    const float a = roughness;
    const float a2 = a * a;
    const float mu = cosTheta;
    const float e = 1.0 - (1.0 - mu) * (0.4022 * a2 + 0.7278 * a) - mu * (0.1412 * a2 + 0.1100 * a);
    return clamp(e, 0.0, 1.0);
}

float3 evaluateBRDFCore(BRDFSurface surface, float3 L, float3 specular, float3 F, float3 lightColor)
{
    const float3 N = surface.normal;
    const float3 V = surface.viewDir;
    const float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return float3(0.0);
    const float NdotV = max(dot(N, V), kEpsilon);
    const float3 kD = (float3(1.0) - F) * (1.0 - surface.metallic);
    const float3 H = normalize(V + L);
    const float LdotH = max(dot(L, H), 0.0);
    float3 diffuse = kD * burleyDiffuse(surface.albedo, surface.roughness, NdotV, NdotL, LdotH);
    const float E_o = GGX_E(NdotV, surface.roughness);
    const float E_i = GGX_E(NdotL, surface.roughness);
    const float Ems = (1.0 - E_o) * (1.0 - E_i);
    const float3 Favg = surface.F0 + (1.0 - surface.F0) / 21.0;
    const float3 fms = Favg * Ems / max(float3(1.0) - Favg * (1.0 - E_o), float3(0.001));
    diffuse += fms * kInvPi * (1.0 - surface.metallic) * surface.albedo;
    specular += fms * surface.metallic;
    return (diffuse + specular) * lightColor * NdotL;
}

float3 evaluateBRDF(BRDFSurface surface, float3 lightDir, float3 lightColor)
{
    const float3 L = normalize(lightDir);
    float3 F;
    const float3 specular = evaluateSpecularBRDF(surface.normal, surface.viewDir, L, surface.roughness, surface.F0, F);
    return evaluateBRDFCore(surface, L, specular, F, lightColor);
}

float3 evaluateBRDFRect(BRDFSurface surface, float3 P, float3 center, float3 e1, float3 e2, float3 Le)
{
    const float3 nArea = cross(e1, e2);
    const float area = length(nArea);
    if (area <= 0.0) return float3(0.0);
    const float3 n = nArea / area;
    if (dot(P - center, n) <= 0.0) return float3(0.0);
    const float3 corner = center - 0.5 * (e1 + e2);
    float3 v[4];
    v[0] = normalize(corner - P);
    v[1] = normalize(corner + e1 - P);
    v[2] = normalize(corner + e1 + e2 - P);
    v[3] = normalize(corner + e2 - P);
    float3 J = float3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        const float3 c = cross(v[i], v[(i + 1) & 3]);
        const float s = length(c);
        if (s > 1e-7) J += acos(clamp(dot(v[i], v[(i + 1) & 3]), -1.0, 1.0)) * (c / s);
    }
    J *= 0.5;
    if (dot(J, center - P) < 0.0) J = -J;
    const float E = dot(surface.normal, J);
    if (E <= 0.0) return float3(0.0);
    const float3 N = surface.normal, V = surface.viewDir;
    const float3 R = reflect(-V, N);
    const float rn = dot(R, n);
    float3 onPlane = center;
    if (rn < -1e-4) onPlane = P + R * (dot(center - P, n) / rn);
    const float3 d = onPlane - corner;
    const float3 rep = corner + clamp(dot(d, e1) / dot(e1, e1), 0.0, 1.0) * e1
                              + clamp(dot(d, e2) / dot(e2, e2), 0.0, 1.0) * e2 - P;
    const float dist = length(rep);
    const float3 specL = rep / dist;
    const float cosLight = dot(-specL, n);
    float3 F;
    float3 specular = float3(0.0);
    if (cosLight > 0.0)
    {
        const float a = surface.roughness * surface.roughness;
        const float rEq = sqrt(area * cosLight / kPi);
        const float aRect = clamp(a + rEq / (2.0 * dist), 0.0, 1.0);
        specular = evaluateSpecularBRDF(N, V, specL, sqrt(aRect), surface.F0, F)
                 * (area * cosLight / (dist * dist)) * max(dot(N, specL), 0.0) / E;
    }
    else
    {
        evaluateSpecularBRDF(N, V, normalize(J), surface.roughness, surface.F0, F);
    }
    return evaluateBRDFCore(surface, normalize(J), specular, F, Le * length(J));
}

float3 evaluateBRDFSphere(BRDFSurface surface, float3 toLight, float radius, float3 lightColor)
{
    const float3 L = normalize(toLight);
    const float3 R = reflect(-surface.viewDir, surface.normal);
    const float3 centerToRay = dot(toLight, R) * R - toLight;
    const float3 closest = toLight + centerToRay * clamp(radius / max(length(centerToRay), kEpsilon), 0.0, 1.0);
    const float a = surface.roughness * surface.roughness;
    const float aSphere = clamp(a + radius / (2.0 * length(toLight)), 0.0, 1.0);
    const float3 specL = normalize(closest);
    float3 F;
    float3 specular = evaluateSpecularBRDF(surface.normal, surface.viewDir, specL, sqrt(aSphere), surface.F0, F);
    specular *= max(dot(surface.normal, specL), 0.0) / max(dot(surface.normal, L), kEpsilon);
    return evaluateBRDFCore(surface, L, specular, F, lightColor);
}

float2 envBRDFApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

float specularOcclusion(float NdotV, float ao, float roughness)
{
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// A direction's texel coordinate in the equirectangular environment images: row 0 (v = 0) is the
// zenith, as the file stores it.
float2 equirectUV(float3 d)
{
    return float2(atan2(d.z, d.x) / 6.2831853 + 0.5, 0.5 - asin(clamp(d.y, -1.0, 1.0)) / 3.1415926);
}

// ---- deferred_lighting.frag's light loop ---------------------------------------------------

float calculateAttenuation(float3 lightPos, float3 fragPos, float range)
{
    const float distance = length(lightPos - fragPos);
    const float d2 = distance * distance;
    const float invSq = 1.0 / max(d2, 0.01);
    const float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
    return invSq * windowing * windowing;
}

float calculateSpotAttenuation(float3 L, float3 spotDir, float innerCone, float outerCone)
{
    const float theta = dot(L, -spotDir);
    return clamp((theta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
}

// The froxel lights reach `fragPos` from: (first list index, count). Without clusters every light
// (0, lightCount). `fragCoord` is the pixel (x right, y down), `viewDepth` the positive view-space
// distance along -Z.
uint2 lightList(StructuredBuffer<uint> clusters, bool clustered, uint lightCount, float2 fragCoord, float2 screen,
                float viewDepth, float sliceScale, float sliceBias)
{
    if (!clustered) return uint2(0u, lightCount);
    const uint tx = min(uint(fragCoord.x * float(kClusterTilesX) / screen.x), kClusterTilesX - 1u);
    const uint ty = min(uint(fragCoord.y * float(kClusterTilesY) / screen.y), kClusterTilesY - 1u);
    const float s = viewDepth > 0.0 ? floor(log(viewDepth) * sliceScale + sliceBias) : 0.0;
    const uint slice = uint(clamp(s, 0.0, float(kClusterSlices - 1u)));
    const uint cluster = (slice * kClusterTilesY + ty) * kClusterTilesX + tx;
    return uint2(kClusterCount + cluster * kClusterMaxLights, clusters[cluster]);
}

// One light's direction from `p` and its attenuated radiance (rgb), as the deferred loop computes
// them; radiance 0 where the deferred loop skips it (past its range, outside its cone).
void lightAt(LightGpu light, float3 p, out float3 L, out float3 radiance, out int type)
{
    type = int(light.position.w);
    float attenuation = 1.0;
    if (type == 0)
    {
        L = normalize(-light.direction.xyz);
    }
    else if (type == 1)
    {
        L = normalize(light.position.xyz - p);
        attenuation = calculateAttenuation(light.position.xyz, p, light.direction.w);
    }
    else if (type == 2)
    {
        L = normalize(light.position.xyz - p);
        attenuation = calculateAttenuation(light.position.xyz, p, light.direction.w);
        attenuation *= calculateSpotAttenuation(L, normalize(light.direction.xyz), light.params.x, light.params.y);
    }
    else
    {
        L = normalize(light.position.xyz - p);
    }
    radiance = light.color.rgb * light.color.w * attenuation;
}

// The direct light a surface receives (the deferred loop without its shadow terms: `shadow` is the
// visibility to apply to a directional light, the forward pass's one sun ray).
float3 directLight(BRDFSurface surface, StructuredBuffer<LightGpu> lights, StructuredBuffer<uint> clusters, uint2 list,
                   bool clustered, float sunVisibility)
{
    float3 Lo = float3(0.0);
    for (uint k = 0u; k < list.y; ++k)
    {
        const LightGpu light = lights[clustered ? clusters[list.x + k] : k];
        float3 L, radiance;
        int type;
        lightAt(light, surface.position, L, radiance, type);
        if (all(radiance == float3(0.0)) && type != 3) continue;
        float shadow = 1.0;
        if (type == 0 && int(round(light.params.w)) >= 0) shadow = sunVisibility;
        if (shadow == 0.0) continue;
        if (type == 3)
            Lo += evaluateBRDFRect(surface, surface.position, light.position.xyz, light.direction.xyz, light.params.xyz,
                                   radiance) * shadow;
        else if (type != 0 && light.params.z > 0.0)
            Lo += evaluateBRDFSphere(surface, light.position.xyz - surface.position, light.params.z, radiance) * shadow;
        else
            Lo += evaluateBRDF(surface, L, radiance) * shadow;
    }
    return Lo;
}

// The light a particle (a small isotropic puff, lit at its centre) receives: each light's
// attenuated radiance, a directional one times `sunVisibility`, over pi (a Lambertian puff
// facing the light).
float3 particleLight(float3 p, StructuredBuffer<LightGpu> lights, StructuredBuffer<uint> clusters, uint2 list,
                     bool clustered, float sunVisibility)
{
    float3 sum = float3(0.0);
    for (uint k = 0u; k < list.y; ++k)
    {
        const LightGpu light = lights[clustered ? clusters[list.x + k] : k];
        float3 L, radiance;
        int type;
        lightAt(light, p, L, radiance, type);
        if (type == 3)
        {
            // A rectangle: its radiance times the solid angle it covers seen from p, as a point.
            const float3 n = cross(light.direction.xyz, light.params.xyz);
            const float3 d = light.position.xyz - p;
            const float d2 = max(dot(d, d), 1e-4);
            radiance *= max(dot(normalize(n), -normalize(d)), 0.0) * length(n) / d2;
        }
        if (type == 0 && int(round(light.params.w)) >= 0) radiance *= sunVisibility;
        sum += radiance;
    }
    return sum * kInvPi;
}
