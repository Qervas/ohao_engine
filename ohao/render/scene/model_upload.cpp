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
// emissive), one upload per material, each kind with its own format and texture type.
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
                // Roughness/metal maps keep R=AO, G=roughness, B=metallic; A is forced to 255.
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
    if (loaded > 0) tm->updateDescriptorSet();
}

} // namespace

uint32_t legacyRtTintTexture(BindlessTextureManager* textures, const glm::vec3& baseColor) {
    if (!textures) return kNoTexture;
    // Ported from VulkanRenderer::uploadRTTextureArray's solid-color layers.
    const auto linearToSRGB = [](float v) -> uint8_t {
        const float s = (v <= 0.0031308f) ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    const std::array<uint8_t, 4> texel{linearToSRGB(baseColor.r), linearToSRGB(baseColor.g),
                                       linearToSRGB(baseColor.b), 255};
    char name[48];
    std::snprintf(name, sizeof(name), "ohao_legacy_rt_tint_%02x%02x%02x", texel[0], texel[1], texel[2]);
    if (const BindlessTextureHandle existing = textures->findTexture(name); existing.valid())
        return existing.index;
    const BindlessTextureHandle h = textures->loadTextureFromMemory(
        std::span<const uint8_t>(texel), 1, 1, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Albedo,
        /*generateMips=*/false);
    if (!h.valid()) return kNoTexture;
    textures->registerName(h, name);
    textures->updateDescriptorSet();
    return h.index;
}

std::optional<EmissiveProxy> emissiveProxy(const Model& model) {
    for (std::size_t mi = 0; mi < model.materialEmissiveTexIndex.size(); ++mi) {
        if (model.materialEmissiveTexIndex[mi] < 0) continue;

        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (const auto& v : model.vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        const glm::vec3 center = (bmin + bmax) * 0.5f;
        const float radius = glm::length(bmax - bmin) * 0.3f;

        glm::vec3 emColor(1.0f);
        float totalPower = 0.0f;
        const int eTexIdx = model.materialEmissiveTexIndex[mi];
        if (eTexIdx < static_cast<int>(model.emissiveTextures.size())) {
            const auto& etd = model.emissiveTextures[static_cast<std::size_t>(eTexIdx)];
            if (texelsPresent(etd)) {
                double r = 0, g = 0, b = 0;
                int brightPixels = 0;
                for (int p = 0; p < etd.width * etd.height; p++) {
                    const float pr = etd.pixels[p * 4 + 0] / 255.0f;
                    const float pg = etd.pixels[p * 4 + 1] / 255.0f;
                    const float pb = etd.pixels[p * 4 + 2] / 255.0f;
                    const float lum = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;
                    if (lum > 0.05f) {
                        r += pr; g += pg; b += pb;
                        brightPixels++;
                        totalPower += lum;
                    }
                }
                if (brightPixels > 0)
                    emColor = glm::vec3(r / brightPixels, g / brightPixels, b / brightPixels);
            }
        }

        if (totalPower > 0.1f) {
            EmissiveProxy light;
            light.localCenter = center;
            light.radius = radius;   // range = radius * 10 is applied by the light table
            light.color = emColor;
            light.intensity = std::min(totalPower * 0.1f, 20.0f);
            return light;
        }
        return std::nullopt;   // one light per model: only the first emissive material counts
    }
    return std::nullopt;
}

glm::vec4 giMaterialLegacyPacked(const Model& model, const MaterialDesc* overrideMaterial) {
    glm::vec3 albedo(0.73f);
    float roughness = 0.95f;
    float metallic = 0.0f;
    if (overrideMaterial) {
        albedo = overrideMaterial->baseColor;
        roughness = overrideMaterial->roughness;
        metallic = overrideMaterial->metallic;
    }
    // Roughness/metallic come from model material 0 whenever the model has materials,
    // even with an override material (the override sets only the albedo).
    if (!model.materialColors.empty()) {
        roughness = model.materialColors[0].w;
        metallic = !model.materialMetallic.empty() ? model.materialMetallic[0] : 0.0f;
    }
    float packed = metallic > 0.5f ? -(roughness + 0.001f) : roughness;
    if (model.vertices.size() > 100) packed += (packed >= 0 ? 10.0f : -10.0f);
    return glm::vec4(albedo, packed);
}

UploadedModel uploadModel(RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                          std::string_view debugName, const MaterialDesc* overrideMaterial) {
    UploadedModel out;
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
    out.mesh = scene.registerMesh(md);

    std::vector<MaterialDesc> mats;
    if (!model.materialColors.empty()) {
        // The old GBufferPass::execute hasModelMaterial branch.
        mats.resize(model.materialColors.size());
        for (std::size_t i = 0; i < mats.size(); ++i) {
            mats[i].baseColor = glm::vec3(model.materialColors[i]);
            mats[i].roughness = model.materialColors[i].w;
            mats[i].metallic = i < model.materialMetallic.size() ? model.materialMetallic[i] : 0.0f;
        }
        uploadTextures(textures, model, mats);
        for (auto& m : mats)
            if (m.emissiveTex != kNoTexture) m.emissiveStrength = 1.0f;
    } else {
        mats.push_back(overrideMaterial ? *overrideMaterial : MaterialDesc{});
    }
    // The old RT texture array's solid-color layer for every material without an
    // albedo texture (LEGACY, MaterialDesc::legacyRtTintTex).
    for (auto& m : mats)
        if (m.albedoTex == kNoTexture && m.legacyRtTintTex == kNoTexture)
            m.legacyRtTintTex = legacyRtTintTexture(textures, m.baseColor);
    out.materials.reserve(mats.size());
    for (const auto& m : mats) out.materials.push_back(scene.registerMaterial(m));

    out.emissive = emissiveProxy(model);
    return out;
}

} // namespace ohao::render
