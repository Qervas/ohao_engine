#include "render/scene/material_table.hpp"

#include <bit>

#include "core/log.hpp"

namespace ohao::render {

bool MaterialTable::initialize(GpuAllocator& allocator) {
    m_allocator = &allocator;
    m_buffer = allocator.createBuffer(VkDeviceSize{kCapacity} * sizeof(Pack),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      AllocationUsage::GpuOnly);
    if (!m_buffer.isValid()) {
        OHAO_LOG(Error, render, "RenderScene: material table allocation failed");
        return false;
    }
    m_packs.reserve(kCapacity);
    m_doubleSided.reserve(kCapacity);
    m_alphaMode.reserve(kCapacity);
    m_emissive.reserve(kCapacity);
    add(MaterialDesc{});   // index 0: the default material
    return true;
}

void MaterialTable::shutdown() {
    if (m_allocator) m_allocator->destroyBuffer(m_buffer);
    m_buffer = GpuBuffer{};
    m_packs.clear();
    m_doubleSided.clear();
    m_alphaMode.clear();
    m_emissive.clear();
    m_layered = 0;
    m_dirty.clear();
    m_allocator = nullptr;
}

MaterialTable::Pack MaterialTable::pack(const MaterialDesc& d) {
    const auto bits = [](uint32_t u) { return std::bit_cast<float>(u); };
    return Pack{
        d.baseColor.r, d.baseColor.g, d.baseColor.b, bits(d.albedoTex),
        d.roughness, d.metallic, bits(d.normalTex), bits(d.emissiveTex),
        bits(d.roughMetalTex), d.emissiveStrength, d.subsurface, bits(d.occlusionTex),
        static_cast<float>(static_cast<uint32_t>(d.alphaMode)), d.alphaCutoff, d.opacity, d.baseAlpha,
        d.specularColor.r, d.specularColor.g, d.specularColor.b, d.specularWeight,
        d.specularIor, d.diffuseRoughness, d.baseWeight, bits(d.specularTex),
        d.emissionColor.r, d.emissionColor.g, d.emissionColor.b, bits(d.specularColorTex),
        d.coatColor.r, d.coatColor.g, d.coatColor.b, d.coatWeight,
        d.coatRoughness, d.coatAnisotropy, d.coatIor, d.coatDarkening,
        d.fuzzColor.r, d.fuzzColor.g, d.fuzzColor.b, d.fuzzWeight,
        d.fuzzRoughness, d.specularAnisotropy, d.tangentRotation, 0.0f,
        bits(d.coatTex), bits(d.coatRoughnessTex), bits(d.fuzzColorTex), bits(d.fuzzRoughnessTex),
        d.transmissionColor.r, d.transmissionColor.g, d.transmissionColor.b, d.transmissionWeight,
        d.transmissionDepth, d.thinWalled ? 1.0f : 0.0f, d.dispersionScale, d.dispersionAbbe,
        d.thinFilmWeight, d.thinFilmThickness, d.thinFilmIor, 0.0f,
        d.subsurfaceColor.r, d.subsurfaceColor.g, d.subsurfaceColor.b, d.subsurfaceRadius,
        d.subsurfaceRadiusScale.r, d.subsurfaceRadiusScale.g, d.subsurfaceRadiusScale.b, d.subsurfaceAnisotropy,
    };
}

MaterialHandle MaterialTable::add(const MaterialDesc& desc) {
    if (m_packs.size() >= kCapacity) {
        static bool logged = false;
        if (!logged) {
            OHAO_LOG(Warn, render, "RenderScene: material table full ({}): further materials use the default material",
                     kCapacity);
            logged = true;
        }
        return MaterialHandle{0, 0};
    }
    const auto index = static_cast<uint32_t>(m_packs.size());
    m_packs.push_back(pack(desc));
    m_doubleSided.push_back(desc.doubleSided ? 1u : 0u);
    // A transmissive material (OpenPBR step 3) is drawn by the transparent pass, which refracts the
    // lit frame behind it, and its TLAS instance is non-opaque (rt/alpha_test.slang passes rays by
    // its transmittance): as a blended one on the host. Its table row keeps its own mode.
    const bool transmissive = desc.transmissionWeight > 0.0f && desc.metallic < 1.0f;
    m_alphaMode.push_back(static_cast<uint8_t>(transmissive && desc.alphaMode == AlphaMode::Opaque ? AlphaMode::Blend
                                                                                                 : desc.alphaMode));
    m_emissive.push_back(desc.emissiveStrength > 0.0f && glm::any(glm::greaterThan(desc.emissionColor, glm::vec3(0.0f))) ? 1u
                                                                                                                       : 0u);
    if (layered(desc)) ++m_layered;
    m_dirty.insert(index);
    return MaterialHandle{index, 0};
}

void MaterialTable::recordUploads(detail::StagingBatch& batch) {
    for (auto it = m_dirty.begin(); it != m_dirty.end();) {
        const uint32_t first = *it;
        uint32_t last = first;
        for (++it; it != m_dirty.end() && *it == last + 1; ++it) last = *it;
        const uint32_t n = last - first + 1;
        batch.upload(m_buffer.buffer, VkDeviceSize{first} * sizeof(Pack), &m_packs[first],
                     VkDeviceSize{n} * sizeof(Pack));
    }
    m_dirty.clear();
}

} // namespace ohao::render
