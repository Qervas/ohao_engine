#include "ecs/spawn.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <span>
#include <string>

namespace ohao::ecs {

namespace {

using render::kNoTexture;

// Submeshes from contiguous runs of materialPerTriangle. Ported from
// buildMaterialDrawRanges (render/deferred/gbuffer_pass.cpp:26-74): the per-triangle
// list is capped at the triangle count, and triangles past its end use material 0.
std::vector<render::SubmeshDesc> materialRuns(const Model& model, uint32_t triangleCount) {
    std::vector<render::SubmeshDesc> runs;
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

// Texture uploads ported from VulkanRenderer::uploadDeferredTextures
// (gpu/vulkan/light_upload.cpp:27-143): the same per-kind formats and texture types,
// in the same order (all albedo, then normal, roughness/metal, emissive), one
// upload per material. Fills the materials' texture indices.
void uploadTextures(BindlessTextureManager* tm, const Model& model, std::vector<render::MaterialDesc>& mats) {
    if (!tm) return;
    int loaded = 0;
    const auto upload = [&](const std::vector<int>& perMaterial, const std::vector<Model::TextureData>& pool,
                            VkFormat format, BindlessTextureType type, bool repackRoughMetal,
                            uint32_t render::MaterialDesc::*field) {
        const std::size_t n = std::min(perMaterial.size(), mats.size());
        for (std::size_t mi = 0; mi < n; ++mi) {
            const int idx = perMaterial[mi];
            if (idx < 0 || idx >= static_cast<int>(pool.size())) continue;
            const auto& td = pool[static_cast<std::size_t>(idx)];
            if (td.pixels.empty() || !texelsPresent(td)) continue;
            BindlessTextureHandle h;
            if (repackRoughMetal) {
                // light_upload.cpp:91-104: R=AO, G=roughness, B=metallic, A=255.
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
           BindlessTextureType::Albedo, false, &render::MaterialDesc::albedoTex);
    upload(model.materialNormalTexIndex, model.normalTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Normal, false, &render::MaterialDesc::normalTex);
    upload(model.materialRoughMetalTexIndex, model.roughMetalTextures, VK_FORMAT_R8G8B8A8_UNORM,
           BindlessTextureType::Roughness, true, &render::MaterialDesc::roughMetalTex);
    upload(model.materialEmissiveTexIndex, model.emissiveTextures, VK_FORMAT_R8G8B8A8_SRGB,
           BindlessTextureType::Emissive, false, &render::MaterialDesc::emissiveTex);
    if (loaded > 0) tm->updateDescriptorSet();
}

// The emissive auto-light, ported from VulkanRenderer::cacheEmissiveLights
// (gpu/vulkan/buffer_setup.cpp:139-203): the first material with an emissive texture
// index decides; the light exists only when its bright-pixel power exceeds 0.1.
std::optional<EmissiveProxyLight> emissiveProxy(const Model& model) {
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
            EmissiveProxyLight light;
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

// LEGACY RT GI material word, ported from VulkanRenderer::buildBLASTLAS
// (gpu/vulkan/rt_build.cpp:895-920).
GiMaterialLegacy giMaterialLegacy(const Model& model, const render::MaterialDesc* overrideMaterial) {
    glm::vec3 albedo(0.73f);
    float roughness = 0.95f;
    float metallic = 0.0f;
    if (overrideMaterial) {
        albedo = overrideMaterial->baseColor;
        roughness = overrideMaterial->roughness;
        metallic = overrideMaterial->metallic;
    }
    // uploadDeferredTextures (light_upload.cpp:73-79) overwrote the actor material's
    // roughness/metallic with model material 0 before buildBLASTLAS packed them.
    if (!model.materialColors.empty()) {
        roughness = model.materialColors[0].w;
        metallic = !model.materialMetallic.empty() ? model.materialMetallic[0] : 0.0f;
    }
    float packed = metallic > 0.5f ? -(roughness + 0.001f) : roughness;
    if (model.vertices.size() > 100) packed += (packed >= 0 ? 10.0f : -10.0f);
    return GiMaterialLegacy{glm::vec4(albedo, packed)};
}

} // namespace

SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                         std::string_view debugName, const render::MaterialDesc* overrideMaterial) {
    SpawnedModel out;
    const uint32_t triangleCount = static_cast<uint32_t>(model.indices.size() / 3);

    render::MeshData md;
    md.vertices = model.vertices;
    md.indices = model.indices;
    md.debugName = std::string(debugName);
    // The old path built material ranges only for models with materialColors
    // (gbuffer_pass.cpp:262-268); otherwise one range with material 0.
    if (!model.materialColors.empty()) {
        md.submeshes = materialRuns(model, triangleCount);
        if (!model.materialPerTriangle.empty()) {
            md.triangleMaterialSlot.assign(triangleCount, 0u);
            const std::size_t n = std::min<std::size_t>(triangleCount, model.materialPerTriangle.size());
            std::copy_n(model.materialPerTriangle.begin(), n, md.triangleMaterialSlot.begin());
        }
    }
    out.mesh = scene.registerMesh(md);

    std::vector<render::MaterialDesc> mats;
    if (!model.materialColors.empty()) {
        // gbuffer_pass.cpp:290-305 (hasModelMaterial branch).
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
        mats.push_back(overrideMaterial ? *overrideMaterial : render::MaterialDesc{});
    }
    out.materials.reserve(mats.size());
    for (const auto& m : mats) out.materials.push_back(scene.registerMaterial(m));

    out.emissive = emissiveProxy(model);
    return out;
}

entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial) {
    SpawnedModel up = uploadModel(scene, textures, model, name, overrideMaterial);
    auto& reg = world.registry();
    const entt::entity e = world.create(name, transform);
    reg.emplace<GiMaterialLegacy>(e, giMaterialLegacy(model, overrideMaterial));
    reg.emplace<MeshRenderer>(e, MeshRenderer{up.mesh, std::move(up.materials), true});
    if (up.emissive) reg.emplace<EmissiveProxyLight>(e, *up.emissive);
    return e;
}

entt::entity spawnLight(World& world, std::string_view name, const Light& light, const Transform& transform) {
    const entt::entity e = world.create(name, transform);
    world.registry().emplace<Light>(e, light);
    return e;
}

glm::quat lookRotation(glm::vec3 direction) {
    const glm::vec3 f = glm::normalize(direction);
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(f.y) > 0.999f) up = glm::vec3(0.0f, 0.0f, f.y > 0.0f ? 1.0f : -1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(f, up));
    const glm::vec3 trueUp = glm::cross(right, f);
    // Columns: local X -> right, Y -> up, Z -> -forward.
    return glm::normalize(glm::quat_cast(glm::mat3(right, trueUp, -f)));
}

} // namespace ohao::ecs
