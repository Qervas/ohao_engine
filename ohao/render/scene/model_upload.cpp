#include "render/scene/model_upload.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"

#include <algorithm>
#include <string>

namespace ohao::render {

namespace {

// Submeshes from runs of one material slot in the per-triangle list.
std::vector<SubmeshDesc> materialRuns(std::span<const uint32_t> slotOfTriangle) {
    std::vector<SubmeshDesc> runs;
    const auto count = static_cast<uint32_t>(slotOfTriangle.size());
    uint32_t runStart = 0;
    for (uint32_t tri = 1; tri <= count; ++tri) {
        if (tri < count && slotOfTriangle[tri] == slotOfTriangle[runStart]) continue;
        runs.push_back({runStart * 3, (tri - runStart) * 3, slotOfTriangle[runStart]});
        runStart = tri;
    }
    return runs;
}

// How a material's texture slot is uploaded and which field of the desc takes its table index.
struct SlotUse {
    cooked::TextureSlot slot;
    VkFormat format;
    BindlessTextureType type;
    bool opaque;
    uint32_t MaterialDesc::*field;
};
// Colour is sRGB, data linear. A roughness/metal map keeps R (unused, or occlusion in a packed ORM image),
// G roughness, B metallic; its alpha is written 255.
constexpr SlotUse kSlots[] = {
    {cooked::TextureSlot::Albedo, VK_FORMAT_R8G8B8A8_SRGB, BindlessTextureType::Albedo, false, &MaterialDesc::albedoTex},
    {cooked::TextureSlot::Normal, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Normal, false, &MaterialDesc::normalTex},
    {cooked::TextureSlot::RoughMetal, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness, true, &MaterialDesc::roughMetalTex},
    {cooked::TextureSlot::Emissive, VK_FORMAT_R8G8B8A8_SRGB, BindlessTextureType::Emissive, false, &MaterialDesc::emissiveTex},
    {cooked::TextureSlot::Occlusion, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness, false, &MaterialDesc::occlusionTex},
    // KHR_materials_clearcoat's and _sheen's (material/material_layers.slang, row 11).
    {cooked::TextureSlot::Coat, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness, false, &MaterialDesc::coatTex},
    {cooked::TextureSlot::CoatRoughness, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness, false, &MaterialDesc::coatRoughnessTex},
    {cooked::TextureSlot::FuzzColor, VK_FORMAT_R8G8B8A8_SRGB, BindlessTextureType::Albedo, false, &MaterialDesc::fuzzColorTex},
    {cooked::TextureSlot::FuzzRoughness, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness, false, &MaterialDesc::fuzzRoughnessTex},
};

} // namespace

MeshHandle uploadMesh(RenderScene& scene, const cooked::Mesh& mesh, std::string_view debugName) {
    MeshData md;
    md.vertices = mesh.vertices;
    md.indices = mesh.indices;
    md.debugName = std::string(debugName);
    // A mesh with materials has a slot per triangle; one without (a primitive) is one submesh, slot 0.
    if (!mesh.materials.empty() && !mesh.materialOfTriangle.empty()) {
        const std::size_t triangles = mesh.indices.size() / 3;
        const auto slots = mesh.materialOfTriangle.first(std::min(triangles, mesh.materialOfTriangle.size()));
        md.submeshes = materialRuns(slots);
        if (slots.size() < triangles)   // triangles past the list's end use material 0
            md.submeshes.push_back({static_cast<uint32_t>(slots.size() * 3), static_cast<uint32_t>((triangles - slots.size()) * 3), 0u});
        md.triangleMaterialSlot.assign(triangles, 0u);
        std::copy(slots.begin(), slots.end(), md.triangleMaterialSlot.begin());
    }
    return scene.registerMesh(md);
}

MeshMaterials meshMaterials(BindlessTextureManager* textures, const cooked::Mesh& mesh, const TextureSource& open) {
    MeshMaterials out;
    std::vector<MaterialDesc>& mats = out.descs;
    mats.resize(mesh.materials.size());
    for (std::size_t i = 0; i < mats.size(); ++i) {
        const cooked::Material& in = mesh.materials[i];
        MaterialDesc& d = mats[i];
        d.baseColor = glm::vec3(in.colorRoughness);
        d.roughness = in.colorRoughness.w;
        d.metallic = in.metallic;
        d.doubleSided = in.doubleSided != 0;
        d.alphaMode = static_cast<AlphaMode>(in.alphaMode);
        d.alphaCutoff = in.alphaCutoff;
        d.baseAlpha = in.baseAlpha;
        d.specularIor = in.ior;
        d.specularWeight = in.specularWeight;
        d.specularColor = in.specularColor;
        d.emissionColor = in.emissionColor;
        if (in.coatWeight > 0.0f) {
            d.coatWeight = in.coatWeight;
            d.coatRoughness = in.coatRoughness;
            d.coatIor = 1.5f;        // glTF's clearcoat: IOR 1.5, no darkening
            d.coatDarkening = 0.0f;
        }
        if (in.transmission > 0.0f) {
            d.transmissionWeight = in.transmission;
            d.transmissionColor = in.transmissionColor;
            d.transmissionDepth = in.transmissionDepth;
        }
        if (glm::any(glm::greaterThan(in.fuzzColor, glm::vec3(0.0f)))) {
            d.fuzzWeight = 1.0f;
            d.fuzzColor = in.fuzzColor;
            d.fuzzRoughness = in.fuzzRoughness;
        }
        if (!textures) continue;
        for (const SlotUse& use : kSlots) {
            const ContentHash name = in.textures[static_cast<std::size_t>(use.slot)];
            if (!name) continue;
            const ContentTexture tex = textures->contentTexture(
                {name.hi, name.lo, use.format, use.type, use.opaque}, [&]() -> TexturePixels {
                    auto opened = open(name);
                    if (!opened) {
                        OHAO_LOG(Error, asset, "{}", opened.error());
                        return {};
                    }
                    const cooked::Texture& t = opened.value();
                    return {t.pixels, t.width, t.height, t.map};
                });
            if (!tex.handle.valid()) continue;
            d.*use.field = tex.handle.index;
            out.held.push_back(tex.handle.index);
            // Roughness in G: its smallest texel, for caustics' casters.
            if (use.slot == cooked::TextureSlot::RoughMetal) d.roughTexMin = static_cast<float>(tex.channelMin[1]) / 255.0f;
            else if (use.slot == cooked::TextureSlot::CoatRoughness) d.coatRoughTexMin = static_cast<float>(tex.channelMin[1]) / 255.0f;
        }
        // Occlusion (AO in R) in the roughness/metal image (a packed ORM): that texture, sampled for both.
        if (in.occlusionIsRoughMetal) d.occlusionTex = d.roughMetalTex;
        // An emissive texture emits at KHR_materials_emissive_strength (1 without it).
        if (d.emissiveTex != kNoTexture) d.emissiveStrength = in.emissionStrength;
    }
    return out;
}

} // namespace ohao::render
