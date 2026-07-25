#pragma once

// Bind dense albedo map into Deferred ground materials (beauty SoT).
// GBuffer model path: findMaterialTextureIndex → "<actor>_albedo_0".
// Fast path: updateTextureFromMemory when SoT slot already exists (M1c).

#include "render/diff/diff_map.hpp"

#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "inverse/scene_builder.hpp"
#include "scene/component/mesh_component.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ohao::diff {

inline constexpr const char* kGroundAlbedoSoTLogical = "diff_ground_albedo_sot";
inline constexpr const char* kGroundOrmSoTLogical = "diff_ground_orm_sot";

/// Ensure named RGBA texture exists (create once) or update in place.
[[nodiscard]] inline bool upsertRgbaMap(BindlessTextureManager* tm, const char* logicalName,
                                        const DiffAlbedoMap& map, BindlessTextureType type,
                                        BindlessTextureHandle& outHandle) {
    if (!tm || map.empty()) return false;
    std::vector<std::uint8_t> rgba(map.pixelCount() * 4u);
    for (size_t i = 0; i < map.pixelCount(); ++i) {
        rgba[i * 4 + 0] =
            static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 0], 0.f, 1.f) * 255.f + 0.5f);
        rgba[i * 4 + 1] =
            static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 1], 0.f, 1.f) * 255.f + 0.5f);
        rgba[i * 4 + 2] =
            static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 2], 0.f, 1.f) * 255.f + 0.5f);
        rgba[i * 4 + 3] = 255;
    }
    BindlessTextureHandle handle = tm->findTexture(logicalName);
    const bool sizeOk = handle.valid() && [&]() {
        const auto* info = tm->getTextureInfo(handle);
        return info && info->width == map.desc.width && info->height == map.desc.height;
    }();
    if (sizeOk) {
        if (!tm->updateTextureFromMemory(handle, std::span<const std::uint8_t>(rgba), map.desc.width,
                                         map.desc.height))
            return false;
        outHandle = handle;
        return true;
    }
    if (handle.valid() && handle != tm->getDefaultWhiteTexture() &&
        handle != tm->getDefaultBlackTexture()) {
        tm->unloadTexture(handle);
    }
    handle = tm->loadTextureFromMemory(std::span<const std::uint8_t>(rgba), map.desc.width,
                                       map.desc.height, VK_FORMAT_R8G8B8A8_UNORM, type,
                                       /*generateMips=*/false);
    if (!handle.valid() || handle == tm->getDefaultWhiteTexture() ||
        handle == tm->getDefaultBlackTexture())
        return false;
    tm->registerName(handle, logicalName);
    tm->setTexturePersistent(handle, true);
    outHandle = handle;
    return true;
}

/// ORM packing: R=AO(1), G=roughness, B=metallic.
/// Base material roughness/metal should be 1 so texture channels are absolute
/// (GBuffer: roughness *= orm.g, metallic *= orm.b).
inline void packOrmMap(const DiffAlbedoMap& roughMap, float metalScalar, DiffAlbedoMap& ormOut) {
    if (roughMap.empty()) return;
    if (ormOut.empty() || ormOut.desc.width != roughMap.desc.width ||
        ormOut.desc.height != roughMap.desc.height)
        ormOut.allocate(roughMap.desc.width, roughMap.desc.height);
    const float m = std::clamp(metalScalar, 0.f, 1.f);
    for (size_t i = 0; i < roughMap.pixelCount(); ++i) {
        // rough stored in G (and optionally rgb[i*3+0] if single-channel fill)
        const float rgh = std::clamp(roughMap.rgb[i * 3 + 1] > 1e-6f ? roughMap.rgb[i * 3 + 1]
                                                                     : roughMap.rgb[i * 3 + 0],
                                     0.04f, 1.f);
        ormOut.rgb[i * 3 + 0] = 1.f; // AO
        ormOut.rgb[i * 3 + 1] = rgh;
        ormOut.rgb[i * 3 + 2] = m; // absolute metal when base metallic = 1
    }
}

/// Pack free roughness + free metallic maps into ORM (absolute channels).
inline void packOrmRoughMetal(const DiffAlbedoMap& roughMap, const DiffAlbedoMap& metalMap,
                              DiffAlbedoMap& ormOut) {
    if (roughMap.empty() || metalMap.empty() || roughMap.pixelCount() != metalMap.pixelCount())
        return;
    if (ormOut.empty() || ormOut.desc.width != roughMap.desc.width ||
        ormOut.desc.height != roughMap.desc.height)
        ormOut.allocate(roughMap.desc.width, roughMap.desc.height);
    for (size_t i = 0; i < roughMap.pixelCount(); ++i) {
        const float rgh = std::clamp(roughMap.rgb[i * 3 + 1] > 1e-6f ? roughMap.rgb[i * 3 + 1]
                                                                     : roughMap.rgb[i * 3 + 0],
                                     0.04f, 1.f);
        const float met = std::clamp(metalMap.rgb[i * 3 + 2] > 1e-6f ? metalMap.rgb[i * 3 + 2]
                                                                     : metalMap.rgb[i * 3 + 0],
                                     0.f, 1.f);
        ormOut.rgb[i * 3 + 0] = 1.f;
        ormOut.rgb[i * 3 + 1] = rgh;
        ormOut.rgb[i * 3 + 2] = met;
    }
}

/// Fill single-channel roughness into DiffAlbedoMap (G channel).
inline void fillRoughMap(DiffAlbedoMap& map, float rough) {
    if (map.empty()) return;
    const float r = std::clamp(rough, 0.04f, 1.f);
    for (size_t i = 0; i < map.pixelCount(); ++i) {
        map.rgb[i * 3 + 0] = r;
        map.rgb[i * 3 + 1] = r;
        map.rgb[i * 3 + 2] = r;
    }
}

/// Upload dense map and wire every ground tile to sample it in Deferred GBuffer.
[[nodiscard]] inline bool bindGroundAlbedoMap(VulkanRenderer& renderer, inverse::InverseScene& inv,
                                              const DiffAlbedoMap& map,
                                              bool /*replacePrevious*/ = true) {
    auto* tm = renderer.getTextureManager();
    if (!tm || map.empty() || inv.groundMats.empty() || inv.groundTiles.empty()) return false;

    BindlessTextureHandle handle{};
    if (!upsertRgbaMap(tm, kGroundAlbedoSoTLogical, map, BindlessTextureType::Albedo, handle))
        return false;

    const glm::vec3 white(1.f);
    for (size_t k = 0; k < inv.groundTiles.size(); ++k) {
        auto* actor = inv.groundTiles[k];
        auto* mat = (k < inv.groundMats.size()) ? inv.groundMats[k] : nullptr;
        if (!actor) continue;

        const std::string actorTexName = actor->getName() + "_albedo_0";
        tm->registerName(handle, actorTexName);

        float rough = 0.5f;
        float metal = 0.0f;
        if (mat) {
            rough = mat->getMaterial().roughness;
            metal = mat->getMaterial().metallic;
            mat->getMaterial().useAlbedoTexture = true;
            mat->getMaterial().albedoTexture = actorTexName;
            mat->getMaterial().baseColor = white;
        }

        if (auto mesh = actor->getComponent<MeshComponent>()) {
            if (auto model = mesh->getModel()) {
                // w = base roughness (multiplied by ORM.g when roughmetal bound)
                const glm::vec4 matCol(1.f, 1.f, 1.f, rough);
                if (model->materialColors.empty()) model->materialColors.push_back(matCol);
                else model->materialColors[0] = matCol;
                if (model->materialMetallic.empty()) model->materialMetallic.push_back(metal);
                else model->materialMetallic[0] = metal;
                for (auto& v : model->vertices) v.color = white;
            }
        }
    }
    return true;
}

/// Bind albedo + ORM (roughness in G). Base roughness/metal set to 1 so maps are absolute.
[[nodiscard]] inline bool bindGroundAlbedoOrmMaps(VulkanRenderer& renderer, inverse::InverseScene& inv,
                                                  const DiffAlbedoMap& albedo,
                                                  const DiffAlbedoMap& orm) {
    auto* tm = renderer.getTextureManager();
    if (!tm || albedo.empty() || orm.empty() || inv.groundMats.empty()) return false;

    BindlessTextureHandle albH{}, ormH{};
    if (!upsertRgbaMap(tm, kGroundAlbedoSoTLogical, albedo, BindlessTextureType::Albedo, albH))
        return false;
    if (!upsertRgbaMap(tm, kGroundOrmSoTLogical, orm, BindlessTextureType::Roughness, ormH))
        return false;

    const glm::vec3 white(1.f);
    for (size_t k = 0; k < inv.groundTiles.size(); ++k) {
        auto* actor = inv.groundTiles[k];
        auto* mat = (k < inv.groundMats.size()) ? inv.groundMats[k] : nullptr;
        if (!actor) continue;
        const std::string albName = actor->getName() + "_albedo_0";
        const std::string ormName = actor->getName() + "_roughmetal_0";
        tm->registerName(albH, albName);
        tm->registerName(ormH, ormName);
        if (mat) {
            mat->getMaterial().useAlbedoTexture = true;
            mat->getMaterial().albedoTexture = albName;
            mat->getMaterial().useRoughnessTexture = true;
            mat->getMaterial().roughnessTexture = ormName;
            mat->getMaterial().baseColor = white;
            mat->getMaterial().roughness = 1.f; // absolute via ORM.g
            mat->getMaterial().metallic = 1.f;  // absolute via ORM.b
        }
        if (auto mesh = actor->getComponent<MeshComponent>()) {
            if (auto model = mesh->getModel()) {
                // base 1 → roughness = orm.g, metal = orm.b (absolute maps)
                const glm::vec4 matCol(1.f, 1.f, 1.f, 1.f);
                if (model->materialColors.empty()) model->materialColors.push_back(matCol);
                else model->materialColors[0] = matCol;
                if (model->materialMetallic.empty()) model->materialMetallic.push_back(1.f);
                else model->materialMetallic[0] = 1.f;
                for (auto& v : model->vertices) v.color = white;
            }
        }
    }
    return true;
}

} // namespace ohao::diff
