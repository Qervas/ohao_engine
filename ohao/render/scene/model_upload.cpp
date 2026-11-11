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

// The field of the desc that takes a slot's table index, and what the table calls its kind.
struct SlotUse {
    cooked::TextureSlot slot;
    BindlessTextureType type;
    uint32_t MaterialDesc::*field;
};
constexpr SlotUse kSlots[] = {
    {cooked::TextureSlot::Albedo, BindlessTextureType::Albedo, &MaterialDesc::albedoTex},
    {cooked::TextureSlot::Normal, BindlessTextureType::Normal, &MaterialDesc::normalTex},
    {cooked::TextureSlot::RoughMetal, BindlessTextureType::Roughness, &MaterialDesc::roughMetalTex},
    {cooked::TextureSlot::Emissive, BindlessTextureType::Emissive, &MaterialDesc::emissiveTex},
    {cooked::TextureSlot::Occlusion, BindlessTextureType::Roughness, &MaterialDesc::occlusionTex},
    // KHR_materials_clearcoat's and _sheen's (material/material_layers.slang, row 11).
    {cooked::TextureSlot::Coat, BindlessTextureType::Roughness, &MaterialDesc::coatTex},
    {cooked::TextureSlot::CoatRoughness, BindlessTextureType::Roughness, &MaterialDesc::coatRoughnessTex},
    {cooked::TextureSlot::FuzzColor, BindlessTextureType::Albedo, &MaterialDesc::fuzzColorTex},
    {cooked::TextureSlot::FuzzRoughness, BindlessTextureType::Roughness, &MaterialDesc::fuzzRoughnessTex},
};

// The GPU's format of a cooked texture (cooked::TextureKind decided it at cook time).
VkFormat formatOf(const cooked::Texture& t) {
    switch (t.format) {
        case cooked::TextureFormat::Bc7: return t.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        case cooked::TextureFormat::Bc5: return VK_FORMAT_BC5_UNORM_BLOCK;
        case cooked::TextureFormat::Rgba8: break;
    }
    return t.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

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
            // The texture's name is its content and its kind, so the format is the kind's.
            const cooked::TextureKind kind = cooked::kindOf(use.slot);
            const VkFormat format = kind == cooked::TextureKind::Normal  ? VK_FORMAT_BC5_UNORM_BLOCK
                                    : kind == cooked::TextureKind::Color ? VK_FORMAT_BC7_SRGB_BLOCK
                                                                         : VK_FORMAT_BC7_UNORM_BLOCK;
            const ContentTexture tex = textures->contentTexture({name.hi, name.lo, format, use.type}, [&]() -> TexturePixels {
                auto opened = open(name);
                if (!opened) {
                    OHAO_LOG(Error, asset, "{}", opened.error());
                    return {};
                }
                const cooked::Texture& t = opened.value();
                if (formatOf(t) != format) {
                    OHAO_LOG(Error, asset, "a cooked texture is not in its kind's format");
                    return {};
                }
                TexturePixels px;
                px.width = t.width;
                px.height = t.height;
                px.keep = t.map;
                px.levels = t.levels;
                px.channelMin = t.channelMin;
                return px;
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
