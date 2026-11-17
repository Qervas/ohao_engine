#include "render/rt/caustic_pass.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"
#include "render/scene/render_scene.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace ohao::render {

namespace {

float u2f(uint32_t u) { return std::bit_cast<float>(u); }

} // namespace

bool CausticPass::allocate(GpuBuffer& b, VkDeviceSize size, bool hostVisible) {
    m_allocator->retire(b);
    // The counter is read on the host (host-cached: uncached host reads are slow); the rest is read at random
    // by the trace: device-local.
    b = m_allocator->createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  hostVisible ? AllocationUsage::GpuToCpu : AllocationUsage::GpuOnly, hostVisible);
    return b.isValid() && (!hostVisible || b.getMappedData() != nullptr);
}

bool CausticPass::create(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    if (!allocate(m_table, kRows * sizeof(glm::vec4), false) || !allocate(m_splat, 16, false) ||
        !allocate(m_stats, kStatsBytes, true)) {
        OHAO_LOG(Error, render, "CausticPass: its buffers could not be created");
        destroy();
        return false;
    }
    std::memset(m_stats.getMappedData(), 0, kStatsBytes);
    m_allocator->flushBuffer(m_stats);
    m_clearSplat = true;
    m_clearCounts = true;
    return true;
}

void CausticPass::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_splatFull) OHAO_LOG(Info, render, "caustics: {} casters, {} splats clamped", m_casterCount, clamped());
    if (const auto m = manifoldCounts(); m[0] > 0)
        OHAO_LOG(Info, render, "caustics' manifold pass: {} requests, {} triangles solved, {} roots, {} connections; "
                 "{} cluster, {} sub-cluster and {} triangle tests; through solids {} walks, {} paths, {} connections, {} at the trials' ceiling",
                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10]);
    if (const auto r = manifoldRoundCounts(); r[4] > 0)
        OHAO_LOG(Info, render, "caustics' manifold rounds: {} items lost, {} sums clamped, {} items carried, {} passes at "
                 "most; peak a batch and pass {} records, {} record items, {} groups, {} clusters, {} sub-clusters, {} "
                 "candidates, {} roots",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]);
    for (GpuBuffer* b : {&m_table, &m_splat, &m_stats}) m_allocator->retire(*b);
    m_splatFull = false;
    m_pixels = 0;
    m_uploaded.clear();
}

std::vector<glm::vec4> CausticPass::pack(const CausticCasters& c, glm::vec3 sceneCenter, float sceneRadius) {
    const auto lights = static_cast<uint32_t>(c.lightIrradiance.size());
    const auto disks = static_cast<uint32_t>(c.disks.size());
    std::vector<glm::vec4> rows;
    rows.reserve(2 + 3 * lights + 2 * disks);
    rows.emplace_back(u2f(lights), u2f(disks), sceneRadius, 0.0f);
    rows.emplace_back(sceneCenter, 0.0f);
    for (uint32_t l = 0; l < lights; ++l) {
        rows.emplace_back(c.lightDirection[l], c.lightIrradiance[l]);
        rows.emplace_back(c.lightRgb[l], c.lightCdf[l]);
        rows.emplace_back(c.lightArea[l], u2f(c.lightFirst[l]), u2f(c.lightFirst[l + 1] - c.lightFirst[l]), 0.0f);
    }
    for (uint32_t d = 0; d < disks; ++d) {
        rows.emplace_back(c.disks[d].center, c.disks[d].radius);
        rows.emplace_back(c.diskCdf[d], 0.0f, 0.0f, 0.0f);
    }
    return rows;
}

void CausticPass::setScene(const RenderScene& scene, float cut) {
    std::vector<CausticCasterInput> casters;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const RenderScene::Draw& d : scene.draws()) {
        if (d.emitter || d.viewModel) continue;
        // A draw's bounds are its mesh's own: to world space through its instance's transform (all
        // eight corners). Taken raw, a moved caster's rays were aimed where its mesh sits unmoved.
        glm::vec3 wlo(1e30f), whi(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? d.aabbMax.x : d.aabbMin.x, (c & 2) ? d.aabbMax.y : d.aabbMin.y,
                                   (c & 4) ? d.aabbMax.z : d.aabbMin.z);
            const glm::vec3 w = d.world ? glm::vec3(*d.world * glm::vec4(corner, 1.0f)) : corner;
            wlo = glm::min(wlo, w);
            whi = glm::max(whi, w);
        }
        lo = glm::min(lo, wlo);
        hi = glm::max(hi, whi);
        if (canBeSmooth(scene.materialSmoothness(scene.materialIndexFor(d.slot, d.materialSlot)), cut))
            casters.push_back({wlo, whi, d.slot});
    }
    std::vector<CausticLightInput> lights;
    for (const LightDesc& l : scene.lights()) {
        if (l.kind != LightKind::Directional || !(glm::dot(l.direction, l.direction) > 0.0f)) continue;
        const glm::vec3 e = l.color * std::max(l.intensity, 0.0f);
        lights.push_back({l.direction, 0.2126f * e.r + 0.7152f * e.g + 0.0722f * e.b, e});
    }
    m_casters = buildCausticCasters(casters, lights);
    std::vector<uint32_t> slots;
    for (const auto& c : casters) slots.push_back(c.slot);
    std::sort(slots.begin(), slots.end());
    m_casterCount = static_cast<uint32_t>(std::unique(slots.begin(), slots.end()) - slots.begin());
    const glm::vec3 center = hi.x >= lo.x ? 0.5f * (lo + hi) : glm::vec3(0.0f);
    const float radius = hi.x >= lo.x ? 0.5f * glm::length(hi - lo) : 1.0f;
    m_packed = pack(m_casters, center, radius);
    if (m_packed.size() > kRows) {
        // More casters than the table holds: one disk a light, the whole scene's (still exact: the
        // density is the disks', and every caster is inside it).
        std::vector<CausticCasterInput> whole{{lo, hi, 0u}};
        m_casters = buildCausticCasters(casters.empty() ? std::vector<CausticCasterInput>{} : whole, lights);
        m_packed = pack(m_casters, center, radius);
    }
}

bool CausticPass::ensure(uint32_t pixels) {
    if (m_device == VK_NULL_HANDLE) return false;
    if (m_splatFull && m_pixels >= pixels) return true;
    if (!allocate(m_splat, VkDeviceSize(pixels) * 3u * 4u, false)) {
        OHAO_LOG(Error, render, "CausticPass: a splat buffer for {} pixels could not be allocated; caustics stay off", pixels);
        allocate(m_splat, 16, false);
        m_splatFull = false;
        m_pixels = 0;
        return false;
    }
    m_splatFull = true;
    m_pixels = pixels;
    m_clearSplat = true;
    return true;
}

void CausticPass::recordBeginFrame(const Frame& frame) {
    const bool upload = m_packed != m_uploaded;
    if (!upload && !m_clearSplat && !m_clearCounts) return;
    Pass pass(frame, nullptr, Pass::Transfer);
    if (upload) pass.write(m_table, BufferUse::CopyDst);
    if (m_clearSplat) pass.write(m_splat, BufferUse::CopyDst);
    if (m_clearCounts) pass.write(m_stats, BufferUse::CopyDst);
    pass.begin();
    if (upload) {
        const VkDeviceSize bytes = m_packed.size() * sizeof(glm::vec4);   // at most 64 KiB (kRows)
        vkCmdUpdateBuffer(frame.cmd, m_table.buffer, 0, bytes, m_packed.data());
        m_uploaded = m_packed;
    }
    if (m_clearSplat) {
        vkCmdFillBuffer(frame.cmd, m_splat.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearSplat = false;
    }
    if (m_clearCounts) {
        vkCmdFillBuffer(frame.cmd, m_stats.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearCounts = false;
    }
}

void CausticPass::lightPass(Pass& pass) {
    pass.read(m_table, BufferUse::Storage).write(m_splat, BufferUse::Storage).write(m_stats, BufferUse::Storage);
}

void CausticPass::cameraPass(Pass& pass) {
    pass.write(m_splat, BufferUse::Storage);
}

void CausticPass::manifoldPass(Pass& pass) {
    pass.read(m_table, BufferUse::Storage).write(m_stats, BufferUse::Storage);
}

bool CausticPass::readStats(void* out, VkDeviceSize offset, VkDeviceSize bytes) const {
    if (!m_stats.getMappedData()) return false;
    GpuBuffer stats = m_stats;   // the handles: the host's view of the memory is brought up to date
    m_allocator->invalidateBuffer(stats);
    std::memcpy(out, static_cast<const uint8_t*>(m_stats.getMappedData()) + offset, bytes);
    return true;
}

std::array<uint32_t, 11> CausticPass::manifoldCounts() const {
    std::array<uint32_t, 11> v{};
    readStats(v.data(), 4, sizeof v);
    return v;
}

std::array<uint32_t, 11> CausticPass::manifoldRoundCounts() const {
    std::array<uint32_t, 11> v{};
    readStats(v.data(), 48, sizeof v);
    return v;
}

uint32_t CausticPass::clamped() const {
    uint32_t v = 0;
    readStats(&v, 0, sizeof v);
    return v;
}

} // namespace ohao::render
