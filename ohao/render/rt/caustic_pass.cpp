#include "render/rt/caustic_pass.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace ohao::render {

namespace {

constexpr VkPipelineStageFlags kTrace = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
             VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

float u2f(uint32_t u) { return std::bit_cast<float>(u); }

} // namespace

bool CausticPass::allocate(Buffer& b, VkDeviceSize size, bool hostVisible) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    // The counter is read on the host (host-cached, AGENTS: uncached host reads are slow); the rest
    // is read at random by the trace: device-local.
    const VkMemoryPropertyFlags props =
        hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    alloc.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX && hostVisible)
        alloc.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, b.buffer, nullptr);
        b = Buffer{};
        return false;
    }
    b.size = size;
    if (vkBindBufferMemory(m_device, b.buffer, b.memory, 0) != VK_SUCCESS) return false;
    if (hostVisible && vkMapMemory(m_device, b.memory, 0, size, 0, &b.mapped) != VK_SUCCESS) return false;
    return true;
}

void CausticPass::release(Buffer& b) {
    if (b.mapped) vkUnmapMemory(m_device, b.memory);
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = Buffer{};
}

bool CausticPass::create(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    if (!allocate(m_table, kRows * sizeof(glm::vec4), false) || !allocate(m_splat, 16, false) ||
        !allocate(m_stats, 16, true)) {
        OHAO_LOG(Error, render, "CausticPass: its buffers could not be created");
        destroy();
        return false;
    }
    std::memset(m_stats.mapped, 0, 16);
    m_clearSplat = true;
    m_clearCounts = true;
    return true;
}

void CausticPass::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_splatFull) OHAO_LOG(Info, render, "caustics: {} casters, {} splats clamped", m_casterCount, clamped());
    release(m_table);
    release(m_splat);
    release(m_stats);
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
    // The placeholder or a smaller buffer may be in flight: nothing is replaced under the GPU.
    vkDeviceWaitIdle(m_device);
    release(m_splat);
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

void CausticPass::recordBeginFrame(VkCommandBuffer cmd) {
    // The last frame's trace may still read the table and write the buffers (raygenCaustics' clears):
    // ordered before this frame's copies and made visible to this frame's atomics.
    barrier(cmd, kTrace, kRW, kTransfer | kTrace, VK_ACCESS_TRANSFER_WRITE_BIT | kRW);
    if (m_packed != m_uploaded) {
        const VkDeviceSize bytes = m_packed.size() * sizeof(glm::vec4);   // at most 64 KiB (kRows)
        vkCmdUpdateBuffer(cmd, m_table.buffer, 0, bytes, m_packed.data());
        m_uploaded = m_packed;
    }
    if (m_clearSplat) {
        vkCmdFillBuffer(cmd, m_splat.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearSplat = false;
    }
    if (m_clearCounts) {
        vkCmdFillBuffer(cmd, m_stats.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearCounts = false;
    }
    barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTrace, kRW);
}

void CausticPass::recordAfterTrace(VkCommandBuffer cmd) {
    // causticTrace's splats before raygenCaustics reads (and clears) them.
    barrier(cmd, kTrace, VK_ACCESS_SHADER_WRITE_BIT, kTrace, kRW);
}

uint32_t CausticPass::clamped() const {
    if (!m_stats.mapped) return 0;
    uint32_t v = 0;
    std::memcpy(&v, m_stats.mapped, sizeof v);
    return v;
}

} // namespace ohao::render
