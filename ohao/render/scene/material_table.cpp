#include "render/scene/material_table.hpp"

#include <bit>
#include <iostream>

namespace ohao::render {

bool MaterialTable::initialize(GpuAllocator& allocator) {
    m_allocator = &allocator;
    m_buffer = allocator.createBuffer(VkDeviceSize{kCapacity} * sizeof(Pack),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      AllocationUsage::GpuOnly);
    if (!m_buffer.isValid()) {
        std::cerr << "[RenderScene] material table allocation failed\n";
        return false;
    }
    m_packs.reserve(kCapacity);
    add(MaterialDesc{});   // index 0: the default material
    return true;
}

void MaterialTable::shutdown() {
    if (m_allocator) m_allocator->destroyBuffer(m_buffer);
    m_buffer = GpuBuffer{};
    m_packs.clear();
    m_dirty.clear();
    m_allocator = nullptr;
}

MaterialTable::Pack MaterialTable::pack(const MaterialDesc& d) {
    const auto bits = [](uint32_t u) { return std::bit_cast<float>(u); };
    return Pack{
        d.baseColor.r, d.baseColor.g, d.baseColor.b, bits(d.albedoTex),
        d.roughness, d.metallic, bits(d.normalTex), bits(d.emissiveTex),
        bits(d.roughMetalTex), d.emissiveStrength, 0.0f, 0.0f,
    };
}

MaterialHandle MaterialTable::add(const MaterialDesc& desc) {
    if (m_packs.size() >= kCapacity) {
        static bool logged = false;
        if (!logged) {
            std::cerr << "[RenderScene] material table full (" << kCapacity
                      << "): further materials use the default material\n";
            logged = true;
        }
        return MaterialHandle{0, 0};
    }
    const auto index = static_cast<uint32_t>(m_packs.size());
    m_packs.push_back(pack(desc));
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
