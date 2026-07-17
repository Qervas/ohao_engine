#pragma once

// Bind dense albedo map into Deferred ground materials (beauty SoT).
// Ground quads use atlas UVs; bindless albedo is the sampled source of truth.
// Falls back cleanly if upload fails (does not force white without a real map).

#include "render/diff/diff_map.hpp"

#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "inverse/scene_builder.hpp"
#include "scene/component/mesh_component.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace ohao::diff {

inline constexpr const char* kGroundAlbedoSoTName = "diff_ground_albedo_sot";

/// Upload map RGB as bindless albedo and point every ground material at it.
/// On success: white verts/base so texture is pure SoT.
/// On failure: leaves materials untouched (caller keeps tile θ via applyTheta).
inline bool bindGroundAlbedoMap(VulkanRenderer& renderer, inverse::InverseScene& inv,
                                const DiffAlbedoMap& map) {
    auto* tm = renderer.getTextureManager();
    if (!tm || map.empty() || inv.groundMats.empty()) return false;

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

    // Drop previous binding name + GPU image (wait so in-flight draws finish).
    if (auto old = tm->findTexture(kGroundAlbedoSoTName); old.valid()) {
        // Erase our logical name even if tex.name is memory_*
        // unloadTexture only erases tex.name — re-register overwrites maps after load.
        tm->unloadTexture(old);
    }

    auto handle = tm->loadTextureFromMemory(
        std::span<const std::uint8_t>(rgba), map.desc.width, map.desc.height,
        VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Albedo, /*generateMips=*/false);
    // loadTextureFromMemory returns default white on failure — reject that.
    if (!handle.valid() || handle == tm->getDefaultWhiteTexture() ||
        handle == tm->getDefaultBlackTexture()) {
        return false;
    }
    tm->registerName(handle, kGroundAlbedoSoTName);

    // Sanity: GBuffer must resolve the name.
    if (!tm->findTexture(kGroundAlbedoSoTName).valid()) {
        std::cerr << "[diff] bindGroundAlbedoMap: name register failed\n";
        return false;
    }

    const glm::vec3 white(1.f);
    for (size_t k = 0; k < inv.groundMats.size(); ++k) {
        auto* mat = inv.groundMats[k];
        if (!mat) continue;
        mat->getMaterial().useAlbedoTexture = true;
        mat->getMaterial().albedoTexture = kGroundAlbedoSoTName;
        mat->getMaterial().baseColor = white;
        if (k < inv.groundTiles.size() && inv.groundTiles[k]) {
            if (auto mesh = inv.groundTiles[k]->getComponent<MeshComponent>()) {
                if (auto model = mesh->getModel()) {
                    for (auto& v : model->vertices) v.color = white;
                }
            }
        }
    }
    return true;
}

} // namespace ohao::diff
