#pragma once
// What the importer makes of a source mesh, before the cooker writes it (cook.cpp): the arrays of one mesh, its
// materials as parallel lists, and the decoded images its materials use. Only the cooker sees it.
#include "asset/vertex.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace ohao {

struct Model {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // Per-triangle material index (for RT texture lookup)
    // materialPerTriangle[triangleIdx] = GLTF material index
    std::vector<uint32_t> materialPerTriangle;

    // The joints Vertex::boneIndices index, by name, in that index order (Assimp numbers bones
    // file-wide in the order it meets them, not in a skin's joint order: an animation skeleton
    // maps them by name, anim::boneToJointMap). Empty for a model without bones.
    std::vector<std::string> boneNames;

    // Per-material base color: vec4(r, g, b, roughness)
    std::vector<glm::vec4> materialColors;
    // Per-material metallic factor (0 = dielectric, 1 = metal)
    std::vector<float> materialMetallic;
    // Per-material sidedness (glTF doubleSided, through Assimp's TWOSIDED): 0 one-sided, its
    // back faces culled.
    std::vector<uint8_t> materialDoubleSided;
    // Per-material alpha (glTF alphaMode, alphaCutoff, baseColorFactor.a, through Assimp): mode 0
    // opaque, 1 mask, 2 blend (render::AlphaMode). Empty: every material opaque.
    std::vector<uint8_t> materialAlphaMode;
    std::vector<float> materialAlphaCutoff;
    std::vector<float> materialBaseAlpha;
    // Per-material OpenPBR inputs from glTF's KHR_materials_* (through Assimp; OpenPBR's defaults
    // where absent): specular_ior (KHR_materials_ior), specular_weight and specular_color
    // (KHR_materials_specular's factors), emission_color (emissiveFactor) and its luminance
    // (KHR_materials_emissive_strength, 1 without it).
    std::vector<float> materialIor;
    std::vector<float> materialSpecularWeight;
    std::vector<glm::vec3> materialSpecularColor;
    std::vector<glm::vec3> materialEmissionColor;
    std::vector<float> materialEmissionStrength;
    // OpenPBR step 2 from KHR_materials_clearcoat (coat_weight, coat_roughness; glTF's coat has
    // IOR 1.5 and no darkening) and KHR_materials_sheen (fuzz_color, fuzz_roughness; fuzz_weight 1
    // where the colour is not black): their factors here, their textures below. The coat's normal
    // texture is not read (the coat shares the base's normal).
    std::vector<float> materialCoatWeight;
    std::vector<float> materialCoatRoughness;
    std::vector<glm::vec3> materialFuzzColor;
    std::vector<float> materialFuzzRoughness;
    // OpenPBR step 3 from KHR_materials_transmission (transmission_weight: its factor) and
    // KHR_materials_volume (transmission_color and transmission_depth: its attenuation colour and
    // distance; a volume with no distance, infinite, absorbs nothing). Thickness is not read.
    std::vector<float> materialTransmission;
    std::vector<glm::vec3> materialTransmissionColor;
    std::vector<float> materialTransmissionDepth;

    // Per-material texture data (RGBA pixels, decoded from GLTF images)
    struct TextureData {
        std::vector<uint8_t> pixels;  // RGBA8
        int width = 0, height = 0;
        int materialIndex = -1;       // which material this texture belongs to
    };
    std::vector<TextureData> albedoTextures;
    std::vector<TextureData> normalTextures;
    std::vector<TextureData> roughMetalTextures;
    std::vector<TextureData> emissiveTextures;
    std::vector<TextureData> occlusionTextures;
    // KHR_materials_clearcoat's clearcoatTexture (R: coat_weight) and clearcoatRoughnessTexture
    // (G: coat_roughness); KHR_materials_sheen's sheenColorTexture (RGB, sRGB: fuzz_color) and
    // sheenRoughnessTexture (A: fuzz_roughness). Each an index into its list, -1 none; one image
    // packing two of them is decoded and uploaded once per use.
    std::vector<TextureData> coatTextures;
    std::vector<TextureData> coatRoughnessTextures;
    std::vector<TextureData> fuzzColorTextures;
    std::vector<TextureData> fuzzRoughnessTextures;
    std::vector<int> materialCoatTexIndex;
    std::vector<int> materialCoatRoughnessTexIndex;
    std::vector<int> materialFuzzColorTexIndex;
    std::vector<int> materialFuzzRoughnessTexIndex;
    std::vector<int> materialTextureIndex;
    std::vector<int> materialNormalTexIndex;
    std::vector<int> materialRoughMetalTexIndex;
    std::vector<int> materialEmissiveTexIndex;
    // glTF occlusionTexture (AO in R): an index into occlusionTextures, -1 none, or
    // kOcclusionIsRoughMetal when it is the same image as the material's roughMetal texture
    // (packed ORM), which is then uploaded once and sampled for both.
    static constexpr int kOcclusionIsRoughMetal = -2;
    std::vector<int> materialOcclusionTexIndex;

    [[nodiscard]] bool empty() const noexcept { return vertices.empty() || indices.empty(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
};

} // namespace ohao
