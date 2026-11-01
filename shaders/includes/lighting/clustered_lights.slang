// The clustered light loop of the deferred frame and the forward passes (core/deferred_lighting.slang,
// vfx/transparent_mesh.slang, vfx/expand.slang): the froxel lists, each light's attenuation, and the
// surface's response through OpenPBR (material/openpbr.slang's OpenPBRSurface), the BSDF the path tracer
// samples. A sphere light's specular is its sphere's (the representative point, the GGX lobe widened
// by its angular size) and its diffuse is the centre's; a rectangle's diffuse is its exact
// irradiance and its specular the representative point's.
//
// The caller binds the light table, the froxel lists (LightGpu, kClusterCount counts then
// kClusterMaxLights indices a froxel) and OpenPBR's tables, and passes them in.
// import includes.lighting.clustered_lights;
import shared.layout;
import material.openpbr;

static const float kPi = 3.14159265359;
static const float kInvPi = 0.31830988618;
static const float kEpsilon = 1e-6;

// A shaded point: its frame (z the shading normal; x an anisotropic material's tangent) and its BSDF.
struct Surface
{
    float3 position;
    float3 normal;
    float3 view;      // toward the viewer
    float3 t, b;
    float3 wo;        // the view in the local frame
    OpenPBRSurface bsdf;

    float3 toLocal(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, normal)); }
};

// `tangent`: an anisotropic material's (unit, in the plane of the normal), else 0 (any frame).
Surface makeSurface(float3 position, float3 normal, float3 view, MaterialInputs m, float3 tangent = float3(0.0))
{
    Surface s;
    s.position = position;
    s.normal = normalize(normal);
    s.view = normalize(view);
    if (dot(tangent, tangent) > 0.0)
    {
        s.t = tangent;
        s.b = cross(s.normal, tangent);
    }
    else
    {
        const float sign = s.normal.z >= 0.0 ? 1.0 : -1.0;   // Duff et al. 2017
        const float a = -1.0 / (sign + s.normal.z);
        const float c = s.normal.x * s.normal.y * a;
        s.t = float3(1.0 + sign * s.normal.x * s.normal.x * a, sign * c, -sign * s.normal.x);
        s.b = float3(c, sign + s.normal.y * s.normal.y * a, -s.normal.y);
    }
    // A view below the shading normal (a normal map tilting away) is taken at the horizon.
    float3 wo = s.toLocal(s.view);
    wo.z = max(wo.z, 1e-4);
    s.wo = normalize(wo);
    s.bsdf = OpenPBRSurface(m);
    return s;
}

// What a light of `radiance` arriving along L gives toward the viewer.
// Light from below the surface gives nothing here, also through a transmissive surface: the
// transparent pass refracts the lit frame behind it, which already holds that light.
float3 evaluateLight(Surface s, float3 L, float3 radiance, OpenPBRTables t)
{
    const float3 wi = s.toLocal(L);
    if (wi.z <= 0.0) return float3(0.0);
    return s.bsdf.eval(s.wo, wi, t) * radiance;
}

// A sphere of `radius` at `toLight` (from the point): the diffuse toward its centre, the specular
// toward the point of the sphere nearest the reflection with the lobe widened by its angular size
// (Karis 2013), so a mirror shows the sphere.
float3 evaluateSphere(Surface s, float3 toLight, float radius, float3 radiance, OpenPBRTables t)
{
    const float3 L = normalize(toLight);
    const float3 R = reflect(-s.view, s.normal);
    const float3 centerToRay = dot(toLight, R) * R - toLight;
    const float3 closest = toLight + centerToRay * clamp(radius / max(length(centerToRay), kEpsilon), 0.0, 1.0);
    const float da = radius / (2.0 * length(toLight));
    float3 specular = float3(0.0), diffuse = float3(0.0), unused;
    const float3 wl = s.toLocal(L), wc = s.toLocal(normalize(closest));
    if (wl.z > 0.0) s.bsdf.evalLobes(s.wo, wl, t, unused, diffuse);
    if (wc.z > 0.0) s.bsdf.widened(da).evalLobes(s.wo, wc, t, specular, unused);
    return (specular + diffuse) * radiance;
}

// A rectangle (centre, edges e1 and e2, one-sided toward e1 x e2) of radiance Le: the diffuse by
// its exact irradiance (the polygon's vector form factor J, the edge integral), the specular by the
// representative point with the lobe widened by the rectangle's size as a disc of equal area.
float3 evaluateRect(Surface s, float3 center, float3 e1, float3 e2, float3 Le, OpenPBRTables t)
{
    const float3 P = s.position;
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
        const float sc = length(c);
        if (sc > 1e-7) J += acos(clamp(dot(v[i], v[(i + 1) & 3]), -1.0, 1.0)) * (c / sc);
    }
    J *= 0.5;
    if (dot(J, center - P) < 0.0) J = -J;
    const float E = dot(s.normal, J);
    if (E <= 0.0) return float3(0.0);
    // Diffuse: the lobe toward J, per unit cosine, times the irradiance.
    const float3 Lj = normalize(J);
    float3 specular = float3(0.0), diffuse, unused;
    s.bsdf.evalLobes(s.wo, s.toLocal(Lj), t, unused, diffuse);
    diffuse *= E / max(dot(s.normal, Lj), kEpsilon);
    // Specular: the representative point's direction, times the solid angle the rectangle covers.
    const float3 R = reflect(-s.view, s.normal);
    const float rn = dot(R, n);
    float3 onPlane = center;
    if (rn < -1e-4) onPlane = P + R * (dot(center - P, n) / rn);
    const float3 d = onPlane - corner;
    const float3 rep = corner + clamp(dot(d, e1) / dot(e1, e1), 0.0, 1.0) * e1
                              + clamp(dot(d, e2) / dot(e2, e2), 0.0, 1.0) * e2 - P;
    const float dist = length(rep);
    const float3 specL = rep / dist;
    const float cosLight = dot(-specL, n);
    if (cosLight > 0.0)
    {
        const float rEq = sqrt(area * cosLight / kPi);
        s.bsdf.widened(rEq / (2.0 * dist)).evalLobes(s.wo, s.toLocal(specL), t, specular, unused);
        specular *= area * cosLight / (dist * dist);
    }
    return (specular + diffuse) * Le;
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

// ---- the light loop (core/deferred_lighting.slang, vfx/transparent_mesh.slang) --------------

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

// What the loop shadows (a light's params.w: -1 a flash, 0 casts no shadow, 1 casts shadows).
// `traced`: RT direct light (rt/restir_di.slang) lit this pixel with every light but flashes, so the
// loop adds only the flashes. Otherwise one visibility for every shadow-casting directional light
// (the deferred frame's CSM, the forward pass's sun ray); other lights unshadowed.
struct LightVisibility
{
    bool traced;
    float directional;
};

LightVisibility directionalVisibility(float v)
{
    LightVisibility vis;
    vis.traced = false;
    vis.directional = v;
    return vis;
}

// The direct light a surface receives from its froxel's lights.
float3 directLight(Surface s, StructuredBuffer<LightGpu> lights, StructuredBuffer<uint> clusters, uint2 list,
                   bool clustered, LightVisibility vis, OpenPBRTables t)
{
    float3 Lo = float3(0.0);
    for (uint k = 0u; k < list.y; ++k)
    {
        const LightGpu light = lights[clustered ? clusters[list.x + k] : k];
        float3 L, radiance;
        int type;
        lightAt(light, s.position, L, radiance, type);
        if (all(radiance == float3(0.0)) && type != 3) continue;   // adds exactly zero: past its range or cone
        const int casts = int(round(light.params.w));
        if (vis.traced && casts >= 0) continue;   // RT direct light lit it
        const float shadow = casts > 0 && type == 0 ? vis.directional : 1.0;
        if (shadow == 0.0) continue;   // fully occluded: adds exactly zero
        if (type == 3)
            Lo += evaluateRect(s, light.position.xyz, light.direction.xyz, light.params.xyz, radiance, t) * shadow;
        else if (type != 0 && light.params.z > 0.0)
            Lo += evaluateSphere(s, light.position.xyz - s.position, light.params.z, radiance, t) * shadow;
        else
            Lo += evaluateLight(s, L, radiance, t) * shadow;
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
        if (type == 0 && int(round(light.params.w)) > 0) radiance *= sunVisibility;
        sum += radiance;
    }
    return sum * kInvPi;
}
