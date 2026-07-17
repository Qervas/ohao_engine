#pragma once

// Bind dense albedo map into Deferred ground materials (beauty SoT).
// GBuffer model path: findMaterialTextureIndex → "<actor>_albedo_0".

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

/// Upload dense map and wire every ground tile to sample it in Deferred GBuffer.
[[nodiscard]] inline bool bindGroundAlbedoMap(VulkanRenderer& renderer, inverse::InverseScene& inv,
                                              const DiffAlbedoMap& map) {
    auto* tm = renderer.getTextureManager();
    if (!tm || map.empty() || inv.groundMats.empty() || inv.groundTiles.empty()) return false;

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

    // Safe replace: wait-idle unload of previous SoT image, then upload.
    if (auto old = tm->findTexture(kGroundAlbedoSoTLogical);
        old.valid() && old != tm->getDefaultWhiteTexture() && old != tm->getDefaultBlackTexture()) {
        tm->unloadTexture(old);
    }

    auto handle = tm->loadTextureFromMemory(
        std::span<const std::uint8_t>(rgba), map.desc.width, map.desc.height,
        VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Albedo, /*generateMips=*/false);
    if (!handle.valid() || handle == tm->getDefaultWhiteTexture() ||
        handle == tm->getDefaultBlackTexture()) {
        return false;
    }
    tm->registerName(handle, kGroundAlbedoSoTLogical);

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

} // namespace ohao::diff
