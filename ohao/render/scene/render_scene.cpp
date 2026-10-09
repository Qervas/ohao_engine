#include "render/scene/render_scene.hpp"
#include "render/scene/instance_table.hpp"
#include "render/scene/light_table.hpp"
#include "render/scene/material_table.hpp"
#include "render/scene/mesh_registry.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace ohao::render {

namespace {
constexpr VkDeviceSize kMinStagingBytes = 256u * 1024u;
}

struct RenderScene::Impl {
    InitInfo info{};
    bool initialized{false};

    GpuAllocator allocator;
    MeshRegistry meshes;
    MaterialTable materials;
    InstanceTable instances;
    LightTable lights;

    // One host-visible staging buffer per frame slot: the slot's fence guarantees
    // the GPU finished reading it before recordUpdates rewrites it.
    std::array<GpuBuffer, MAX_FRAMES_IN_FLIGHT> staging{};
    std::array<VkDeviceSize, MAX_FRAMES_IN_FLIGHT> stagingCapacity{};

    struct Retired { uint64_t retireAt; GpuBuffer buf; };
    std::vector<Retired> retireQueue;
    uint64_t frameCounter{0};

    RenderSceneStats stats;
    std::vector<Draw> draws;
    bool drawsDirty{true};
    bool tlasNeedsRebuild{true};   // consumed by the TLAS builder (Task 5)

    [[nodiscard]] bool rt() const noexcept { return info.rt != nullptr && info.rt->isSupported(); }

    void retire(std::vector<GpuBuffer>& bufs) {
        for (GpuBuffer& b : bufs)
            retireQueue.push_back(Retired{frameCounter + MAX_FRAMES_IN_FLIGHT, b});
        bufs.clear();
    }

    [[nodiscard]] bool ensureStaging(uint32_t slot, VkDeviceSize bytes) {
        if (stagingCapacity[slot] >= bytes) return true;
        allocator.destroyBuffer(staging[slot]);   // the slot's fence was waited: unused
        const VkDeviceSize cap = std::max({bytes, stagingCapacity[slot] * 2, kMinStagingBytes});
        staging[slot] = allocator.createBuffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               AllocationUsage::CpuToGpu, true);
        if (!staging[slot].isValid() || !staging[slot].getMappedData()) {
            allocator.destroyBuffer(staging[slot]);
            staging[slot] = GpuBuffer{};
            stagingCapacity[slot] = 0;
            std::cerr << "[RenderScene] staging allocation failed (" << cap << " bytes)\n";
            return false;
        }
        stagingCapacity[slot] = cap;
        return true;
    }

    void rebuildDraws() {
        draws.clear();
        for (uint32_t slot = 0; slot < instances.highWater(); ++slot) {
            if (!instances.live(slot)) continue;
            const InstanceGpu& g = instances.gpu(slot);
            if ((g.flags & kInstanceVisible) == 0) continue;
            const MeshInfo* mesh = meshes.info(MeshHandle{g.mesh, 0});
            if (!mesh) continue;
            for (const SubmeshDesc& sub : mesh->submeshes) {
                draws.push_back(Draw{slot, mesh->indexOffset + sub.firstIndex, sub.indexCount,
                                     static_cast<int32_t>(mesh->vertexOffset), sub.materialSlot,
                                     mesh->aabbMin, mesh->aabbMax, &g.world});
            }
        }
        drawsDirty = false;
    }
};

RenderScene::RenderScene() : m(std::make_unique<Impl>()) {}
RenderScene::~RenderScene() { shutdown(); }

bool RenderScene::initialize(const InitInfo& info) {
    if (m->initialized) return true;
    m->info = info;
    if (!m->allocator.initialize(info.instance, info.physical, info.device)) {
        std::cerr << "[RenderScene] GpuAllocator initialization failed\n";
        return false;
    }
    const MeshRegistry::Context ctx{&m->allocator, info.device, info.queue, info.commandPool,
                                    m->rt() ? info.rt : nullptr};
    m->initialized = true;   // so shutdown() releases whatever was created below
    if (!m->meshes.initialize(ctx) || !m->materials.initialize(m->allocator) ||
        !m->instances.initialize(m->allocator) || !m->lights.initialize(m->allocator)) {
        shutdown();
        return false;
    }
    return true;
}

void RenderScene::shutdown() {
    if (!m || !m->initialized) return;
    vkDeviceWaitIdle(m->info.device);
    for (auto& r : m->retireQueue) m->allocator.destroyBuffer(r.buf);
    m->retireQueue.clear();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m->allocator.destroyBuffer(m->staging[i]);
        m->staging[i] = GpuBuffer{};
        m->stagingCapacity[i] = 0;
    }
    m->lights.shutdown();
    m->instances.shutdown();
    m->materials.shutdown();
    m->meshes.shutdown();
    m->allocator.shutdown();
    m->draws.clear();
    m->initialized = false;
}

MeshHandle RenderScene::registerMesh(const MeshData& data) {
    if (!m->initialized) return {};
    std::vector<GpuBuffer> retired;
    uint32_t grown = 0;
    const MeshHandle h = m->meshes.registerMesh(data, retired, grown);
    if (grown > 0) {
        m->stats.arenaGrowths += grown;
        m->tlasNeedsRebuild = true;   // every BLAS was rebuilt against the new arenas
    }
    m->retire(retired);
    return h;
}

MaterialHandle RenderScene::registerMaterial(const MaterialDesc& desc) {
    if (!m->initialized) return {};
    return m->materials.add(desc);
}

uint32_t RenderScene::createInstance(const InstanceDesc& desc) {
    if (!m->initialized) return kInvalidIndex;
    if (!m->meshes.info(desc.mesh)) {
        std::cerr << "[RenderScene] createInstance: unknown mesh handle\n";
        return kInvalidIndex;
    }
    return m->instances.create(desc, m->materials.count());
}

void RenderScene::updateInstance(uint32_t slot, const InstanceDesc& desc) {
    if (!m->initialized) return;
    if (!m->meshes.info(desc.mesh)) {
        std::cerr << "[RenderScene] updateInstance: unknown mesh handle\n";
        return;
    }
    m->instances.update(slot, desc, m->materials.count());
}

void RenderScene::destroyInstance(uint32_t slot) {
    if (!m->initialized) return;
    m->instances.destroy(slot);
}

void RenderScene::setLights(std::span<const LightDesc> lights) {
    if (!m->initialized) return;
    m->lights.set(lights);
}

void RenderScene::setEnvMapTextureIndex(uint32_t texIdx) {
    if (!m->initialized) return;
    m->lights.setEnvMapTextureIndex(texIdx);
}

void RenderScene::recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot) {
    if (!m->initialized || cmd == VK_NULL_HANDLE || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    const auto t0 = std::chrono::steady_clock::now();
    m->stats.instanceBytesUploaded = 0;
    m->stats.lightBytesUploaded = 0;

    const VkDeviceSize need = m->materials.pendingBytes() + m->instances.pendingBytes() +
                              m->lights.pendingBytes();
    if (need > 0 || m->instances.needsGpuGrowth()) {
        if (need == 0 || m->ensureStaging(frameSlot, need)) {
            detail::StagingBatch batch{};
            batch.cmd = cmd;
            batch.buffer = m->staging[frameSlot].buffer;
            batch.mapped = static_cast<std::byte*>(m->staging[frameSlot].getMappedData());
            batch.capacity = m->stagingCapacity[frameSlot];

            VkPipelineStageFlags readers = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            if (m->rt()) {
                readers |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                           VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            }

            // Earlier frames read these tables (and earlier copies wrote them): order
            // this frame's copies after both.
            VkMemoryBarrier before{};
            before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, readers | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &before, 0, nullptr, 0, nullptr);

            std::vector<GpuBuffer> retired;
            m->materials.recordUploads(batch);
            m->stats.instanceBytesUploaded = m->instances.recordUploads(batch, retired);
            m->stats.lightBytesUploaded = m->lights.recordUploads(batch);
            if (batch.used > 0) m->allocator.flushBuffer(m->staging[frameSlot], 0, batch.used);

            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, readers,
                                 0, 1, &after, 0, nullptr, 0, nullptr);
            m->retire(retired);
        }
    }

    m->stats.recordCpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void RenderScene::onFrameRetired() {
    if (!m->initialized) return;
    ++m->frameCounter;
    auto& q = m->retireQueue;
    auto keep = q.begin();
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->retireAt <= m->frameCounter) m->allocator.destroyBuffer(it->buf);
        else *keep++ = *it;
    }
    q.erase(keep, q.end());
}

const MeshInfo* RenderScene::meshInfo(MeshHandle h) const { return m->meshes.info(h); }

uint32_t RenderScene::materialIndexFor(uint32_t slot, uint32_t materialSlot) const {
    return m->instances.materialIndexFor(slot, materialSlot);
}

bool RenderScene::rtSupported() const noexcept { return m->rt(); }

VkBuffer RenderScene::vertexArena() const { return m->meshes.vertexArena(); }
VkBuffer RenderScene::indexArena() const { return m->meshes.indexArena(); }
VkBuffer RenderScene::triangleSlotBuffer() const { return m->meshes.triangleSlotBuffer(); }
VkBuffer RenderScene::meshTable() const { return m->meshes.meshTable(); }
VkBuffer RenderScene::materialBuffer() const { return m->materials.buffer(); }
VkBuffer RenderScene::materialRefBuffer() const { return m->instances.materialRefBuffer(); }
VkBuffer RenderScene::instanceBuffer() const { return m->instances.instanceBuffer(); }
std::span<const LightData> RenderScene::rasterLights() const { return m->lights.raster(); }
VkBuffer RenderScene::rtLightBuffer() const { return m->lights.rtBuffer(); }
uint32_t RenderScene::rtLightCount() const { return m->lights.rtCount(); }

std::span<const RenderScene::Draw> RenderScene::draws() const {
    if (m->instances.consumeDrawsDirty() || m->drawsDirty) m->rebuildDraws();
    return m->draws;
}

const RenderSceneStats& RenderScene::stats() const noexcept { return m->stats; }
VkAccelerationStructureKHR RenderScene::tlas() const { return VK_NULL_HANDLE; }
bool RenderScene::empty() const noexcept { return m->instances.liveCount() == 0; }

} // namespace ohao::render
