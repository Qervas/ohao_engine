#include "render/scene/model_upload.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>

namespace ohao::render {

namespace {

// Submeshes from contiguous runs of materialPerTriangle. Ported from
// buildMaterialDrawRanges (the old render/deferred/gbuffer_pass.cpp): the per-triangle
// list is capped at the triangle count, and triangles past its end use material 0.
std::vector<SubmeshDesc> materialRuns(const Model& model, uint32_t triangleCount) {
    std::vector<SubmeshDesc> runs;
    if (triangleCount == 0 || model.materialPerTriangle.empty()) return runs;   // one submesh, slot 0
    const uint32_t capped =
        std::min<uint32_t>(triangleCount, static_cast<uint32_t>(model.materialPerTriangle.size()));
    uint32_t runMaterial = model.materialPerTriangle[0];
    uint32_t runStart = 0;
    for (uint32_t tri = 1; tri < capped; ++tri) {
        const uint32_t m = model.materialPerTriangle[tri];
        if (m == runMaterial) continue;
        runs.push_back({runStart * 3, (tri - runStart) * 3, runMaterial});
        runMaterial = m;
        runStart = tri;
    }
    runs.push_back({runStart * 3, (capped - runStart) * 3, runMaterial});
    if (capped < triangleCount) runs.push_back({capped * 3, (triangleCount - capped) * 3, 0u});
    return runs;
}

bool texelsPresent(const Model::TextureData& td) {
    return td.width > 0 && td.height > 0 &&
           td.pixels.size() >= static_cast<std::size_t>(td.width) * static_cast<std::size_t>(td.height) * 4;
}

// Texture uploads, by kind in a fixed order (all albedo, then normal, roughness/metal,
// emissive, occlusion), one upload per material, each kind with its own format and texture type.
// Fills the materials' texture indices.
void uploadTextures(BindlessTextureManager* tm, const Model& model, std::vector<MaterialDesc>& mats) {
    if (!tm) return;
    int loaded = 0;
    const auto upload = [&](const std::vector<int>& perMaterial, const std::vector<Model::TextureData>& pool,
                            VkFormat format, BindlessTextureType type, bool repackRoughMetal,
                            uint32_t MaterialDesc::*field) {
        const std::size_t n = std::min(perMaterial.size(), mats.size());
        for (std::size_t mi = 0; mi < n; ++mi) {
            const int idx = perMaterial[mi];
            if (idx < 0 || idx >= static_cast<int>(pool.size())) continue;
            const auto& td = pool[static_cast<std::size_t>(idx)];
            if (td.pixels.empty() || !texelsPresent(td)) continue;
            BindlessTextureHandle h;
            if (repackRoughMetal) {
                // Roughness/metal maps keep R (glTF: unused, or occlusion in a packed ORM image),
                // G=roughness, B=metallic; A is forced to 255.
                std::vector<uint8_t> repacked(static_cast<std::size_t>(td.width) * td.height * 4);
                for (int p = 0; p < td.width * td.height; ++p) {
                    repacked[p * 4 + 0] = td.pixels[p * 4 + 0];
                    repacked[p * 4 + 1] = td.pixels[p * 4 + 1];
                    repacked[p * 4 + 2] = td.pixels[p * 4 + 2];
                    repacked[p * 4 + 3] = 255;
                }
                h = tm->loadTextureFromMemory(std::span<const uint8_t>(repacked), td.width, td.height, format, type);
            } else {
                h = tm->loadTextureFromMemory(std::span<const uint8_t>(td.pixels), td.width, td.height, format, type);
            }
            if (h.valid()) {
                mats[mi].*field = h.index;
                ++loaded;
            }
        }
    };
    upload(model.materialTextureIndex, model.albedoTextures, VK_FORMAT_R8G8B8A8_SRGB,
           BindlessTextureType::Albedo, false, &MaterialDesc::albedoTex);
    upload(model.materialNormalTexIndex, model.normalTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Normal, false, &MaterialDesc::normalTex);
    upload(model.materialRoughMetalTexIndex, model.roughMetalTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Roughness, true, &MaterialDesc::roughMetalTex);
    upload(model.materialEmissiveTexIndex, model.emissiveTextures, VK_FORMAT_R8G8B8A8_SRGB,
           BindlessTextureType::Emissive, false, &MaterialDesc::emissiveTex);
    // Occlusion (AO in R): its own image, or the roughMetal texture already uploaded (ORM).
    upload(model.materialOcclusionTexIndex, model.occlusionTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Roughness, false, &MaterialDesc::occlusionTex);
    for (std::size_t mi = 0; mi < std::min(model.materialOcclusionTexIndex.size(), mats.size()); ++mi)
        if (model.materialOcclusionTexIndex[mi] == Model::kOcclusionIsRoughMetal) mats[mi].occlusionTex = mats[mi].roughMetalTex;
    // KHR_materials_clearcoat's and _sheen's (material/material_layers.slang, row 11).
    upload(model.materialCoatTexIndex, model.coatTextures, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Roughness,
           false, &MaterialDesc::coatTex);
    upload(model.materialCoatRoughnessTexIndex, model.coatRoughnessTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Roughness, false, &MaterialDesc::coatRoughnessTex);
    upload(model.materialFuzzColorTexIndex, model.fuzzColorTextures, VK_FORMAT_R8G8B8A8_SRGB,
           BindlessTextureType::Albedo, false, &MaterialDesc::fuzzColorTex);
    upload(model.materialFuzzRoughnessTexIndex, model.fuzzRoughnessTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Roughness, false, &MaterialDesc::fuzzRoughnessTex);
    if (loaded > 0) tm->updateDescriptorSet();
}

} // namespace


MeshHandle uploadModelMesh(RenderScene& scene, const Model& model, std::string_view debugName) {
    const uint32_t triangleCount = static_cast<uint32_t>(model.indices.size() / 3);

    MeshData md;
    md.vertices = model.vertices;
    md.indices = model.indices;
    md.debugName = std::string(debugName);
    // The old path built material ranges only for models with materialColors;
    // otherwise one range with material 0.
    if (!model.materialColors.empty()) {
        md.submeshes = materialRuns(model, triangleCount);
        if (!model.materialPerTriangle.empty()) {
            md.triangleMaterialSlot.assign(triangleCount, 0u);
            const std::size_t n = std::min<std::size_t>(triangleCount, model.materialPerTriangle.size());
            std::copy_n(model.materialPerTriangle.begin(), n, md.triangleMaterialSlot.begin());
        }
    }
    return scene.registerMesh(md);
}

std::vector<MaterialDesc> modelMaterials(BindlessTextureManager* textures, const Model& model,
                                         const MaterialDesc* overrideMaterial) {
    std::vector<MaterialDesc> mats;
    if (!model.materialColors.empty()) {
        // The old GBufferPass::execute hasModelMaterial branch.
        mats.resize(model.materialColors.size());
        for (std::size_t i = 0; i < mats.size(); ++i) {
            mats[i].baseColor = glm::vec3(model.materialColors[i]);
            mats[i].roughness = model.materialColors[i].w;
            mats[i].metallic = i < model.materialMetallic.size() ? model.materialMetallic[i] : 0.0f;
            mats[i].doubleSided = i >= model.materialDoubleSided.size() || model.materialDoubleSided[i] != 0;
            if (i < model.materialAlphaMode.size()) mats[i].alphaMode = static_cast<AlphaMode>(model.materialAlphaMode[i]);
            if (i < model.materialAlphaCutoff.size()) mats[i].alphaCutoff = model.materialAlphaCutoff[i];
            if (i < model.materialBaseAlpha.size()) mats[i].baseAlpha = model.materialBaseAlpha[i];
            if (i < model.materialIor.size()) mats[i].specularIor = model.materialIor[i];
            if (i < model.materialSpecularWeight.size()) mats[i].specularWeight = model.materialSpecularWeight[i];
            if (i < model.materialSpecularColor.size()) mats[i].specularColor = model.materialSpecularColor[i];
            if (i < model.materialEmissionColor.size()) mats[i].emissionColor = model.materialEmissionColor[i];
            if (i < model.materialCoatWeight.size() && model.materialCoatWeight[i] > 0.0f) {
                mats[i].coatWeight = model.materialCoatWeight[i];
                mats[i].coatRoughness = model.materialCoatRoughness[i];
                mats[i].coatIor = 1.5f;        // glTF's clearcoat: IOR 1.5, no darkening
                mats[i].coatDarkening = 0.0f;
            }
            if (i < model.materialFuzzColor.size() && glm::any(glm::greaterThan(model.materialFuzzColor[i], glm::vec3(0.0f)))) {
                mats[i].fuzzWeight = 1.0f;
                mats[i].fuzzColor = model.materialFuzzColor[i];
                mats[i].fuzzRoughness = model.materialFuzzRoughness[i];
            }
        }
        uploadTextures(textures, model, mats);
        // An emissive texture emits at KHR_materials_emissive_strength (1 without it).
        for (std::size_t i = 0; i < mats.size(); ++i)
            if (mats[i].emissiveTex != kNoTexture)
                mats[i].emissiveStrength = i < model.materialEmissionStrength.size() ? model.materialEmissionStrength[i] : 1.0f;
    } else {
        mats.push_back(overrideMaterial ? *overrideMaterial : MaterialDesc{});
    }
    return mats;
}

UploadedModel uploadModel(RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                          std::string_view debugName, const MaterialDesc* overrideMaterial) {
    UploadedModel out;
    out.mesh = uploadModelMesh(scene, model, debugName);
    const std::vector<MaterialDesc> mats = modelMaterials(textures, model, overrideMaterial);
    out.materials.reserve(mats.size());
    for (const auto& m : mats) out.materials.push_back(scene.registerMaterial(m));
    return out;
}

} // namespace ohao::render
