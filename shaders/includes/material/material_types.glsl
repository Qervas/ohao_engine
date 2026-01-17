// material_types.glsl - Material data structures for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/material/material_types.glsl
//
// Defines the MaterialData struct for PBR materials.
// Layout is std140 compatible (96 bytes total).

#ifndef OHAO_MATERIAL_TYPES_GLSL
#define OHAO_MATERIAL_TYPES_GLSL

#include "includes/common/constants.glsl"

// =============================================================================
// Material Data Structure
// =============================================================================

// MaterialData - GPU representation of a PBR material (96 bytes, std140 layout)
// This structure is designed to be tightly packed while maintaining alignment.
// It matches the C++ MaterialData struct in material_types.hpp
struct MaterialData {
    // Offset 0: Base color (albedo) with alpha
    vec4 baseColor;         // xyz = albedo, w = alpha

    // Offset 16: Emission color with intensity
    vec4 emissive;          // xyz = emission color, w = emission intensity

    // Offset 32: PBR parameters
    float metallic;         // 0 = dielectric, 1 = metal
    float roughness;        // 0 = smooth, 1 = rough (perceptual)
    float ao;               // Ambient occlusion (0 = occluded, 1 = not occluded)
    float ior;              // Index of refraction (default 1.5)

    // Offset 48: Material flags and texture indices
    uint flags;             // Material flags bitfield (see MATERIAL_FLAG_*)
    int albedoMapIndex;     // -1 = no texture
    int normalMapIndex;     // -1 = no texture
    int metallicRoughnessMapIndex; // -1 = no texture (R=metallic, G=roughness)

    // Offset 64: Additional texture indices
    int aoMapIndex;         // -1 = no texture
    int emissiveMapIndex;   // -1 = no texture
    float normalScale;      // Normal map intensity (default 1.0)
    float occlusionStrength;// AO map strength (default 1.0)

    // Offset 80: UV transform
    vec2 uvScale;           // Texture coordinate scale
    vec2 uvOffset;          // Texture coordinate offset

    // Total: 96 bytes (std140 compatible)
};

// =============================================================================
// Surface Data (Intermediate Representation)
// =============================================================================

// SurfaceData - Evaluated surface properties at a fragment
// This is the result of sampling all material textures and combining
// with vertex attributes.
struct SurfaceData {
    vec3 position;          // World position
    vec3 normal;            // World normal (possibly from normal map)
    vec3 geometryNormal;    // Original geometry normal
    vec3 viewDir;           // View direction (from surface to camera)
    vec2 texCoord;          // Texture coordinates

    // Material properties
    vec3 albedo;            // Final albedo color
    float alpha;            // Final alpha
    float metallic;         // Final metallic value
    float roughness;        // Final roughness value
    float ao;               // Final ambient occlusion
    vec3 emissive;          // Final emission color (pre-multiplied with intensity)
    vec3 F0;                // Pre-computed base reflectivity

    // Flags
    bool isFrontFace;       // True if front-facing
};

// =============================================================================
// Helper Functions
// =============================================================================

// Check if a material flag is set
bool hasFlag(MaterialData material, uint flag) {
    return (material.flags & flag) != 0u;
}

// Get default material data
MaterialData getDefaultMaterial() {
    MaterialData mat;
    mat.baseColor = vec4(1.0, 1.0, 1.0, 1.0);
    mat.emissive = vec4(0.0, 0.0, 0.0, 0.0);
    mat.metallic = DEFAULT_METALLIC;
    mat.roughness = DEFAULT_ROUGHNESS;
    mat.ao = DEFAULT_AO;
    mat.ior = 1.5;
    mat.flags = MATERIAL_FLAG_NONE;
    mat.albedoMapIndex = -1;
    mat.normalMapIndex = -1;
    mat.metallicRoughnessMapIndex = -1;
    mat.aoMapIndex = -1;
    mat.emissiveMapIndex = -1;
    mat.normalScale = 1.0;
    mat.occlusionStrength = 1.0;
    mat.uvScale = vec2(1.0);
    mat.uvOffset = vec2(0.0);
    return mat;
}

// Initialize surface data from vertex attributes and material
SurfaceData initSurfaceData(vec3 position, vec3 normal, vec2 texCoord,
                             vec3 viewPos, bool isFrontFace) {
    SurfaceData surface;
    surface.position = position;
    surface.normal = normalize(normal);
    surface.geometryNormal = surface.normal;
    surface.viewDir = normalize(viewPos - position);
    surface.texCoord = texCoord;

    // Default material values (will be overwritten by texture sampling)
    surface.albedo = vec3(1.0);
    surface.alpha = 1.0;
    surface.metallic = DEFAULT_METALLIC;
    surface.roughness = DEFAULT_ROUGHNESS;
    surface.ao = DEFAULT_AO;
    surface.emissive = vec3(0.0);
    surface.F0 = vec3(0.04); // Dielectric F0

    surface.isFrontFace = isFrontFace;
    return surface;
}

// Calculate base reflectivity (F0) from surface data
vec3 calculateF0(SurfaceData surface) {
    vec3 dielectricF0 = vec3(0.04);
    return mix(dielectricF0, surface.albedo, surface.metallic);
}

// Apply double-sided shading (flip normal if back-facing)
void applyDoubleSided(inout SurfaceData surface) {
    if (!surface.isFrontFace) {
        surface.normal = -surface.normal;
        surface.geometryNormal = -surface.geometryNormal;
    }
}

#endif // OHAO_MATERIAL_TYPES_GLSL
