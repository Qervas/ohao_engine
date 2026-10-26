#include "render/scene/render_scene.hpp"
#include "render/scene/instance_table.hpp"
#include "render/scene/light_table.hpp"
#include "render/scene/material_table.hpp"
#include "render/scene/mesh_registry.hpp"
#include "gpu/shared_layout.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <set>

#include "core/log.hpp"

namespace ohao::render {

namespace {
constexpr VkDeviceSize kMinStagingBytes = 256u * 1024u;
constexpr VkDeviceSize kTlasInstanceBytes = sizeof(VkAccelerationStructureInstanceKHR);
static_assert(kTlasInstanceBytes == 64, "staging runs stay 16-byte aligned");
}

struct RenderScene::Impl {
    InitInfo info{};
    bool initialized{false};

    GpuAllocator allocator;
    std::unique_ptr<RTAccelerationStructure> rtAccel;   // own instance; see InitInfo::rayTracing
    MeshRegistry meshes;
    MaterialTable materials;
    InstanceTable instances;
    LightTable lights;

    // One host-visible staging buffer per frame slot: the slot's fence guarantees
    // the GPU finished reading it before recordUpdates rewrites it.
    std::array<GpuBuffer, MAX_FRAMES_IN_FLIGHT> staging{};
    std::array<VkDeviceSize, MAX_FRAMES_IN_FLIGHT> stagingCapacity{};

    // A retired entry holds a buffer, TLAS storage / scratch, or a released mesh's BLAS
    // (RT only).
    struct Retired { uint64_t retireAt; GpuBuffer buf; RetiredTlas tlas; BlasHandle blas{INVALID_BLAS}; };
    std::vector<Retired> retireQueue;
    uint64_t frameCounter{0};

    // TLAS input: one VkAccelerationStructureInstanceKHR per slot, device-local,
    // written only by staged copies. `tlasHost` mirrors it; `tlasDirty` slots are
    // uploaded next recordUpdates. `tlasStructural`: a create, destroy, mesh or
    // visibility change since the last build -- the next build must not refit.
    GpuBuffer tlasInstances;
    uint32_t tlasCapacity{0};
    std::vector<VkAccelerationStructureInstanceKHR> tlasHost;
    std::set<uint32_t> tlasDirty;
    bool tlasStructural{false};

    RenderSceneStats stats;
    std::vector<Draw> draws;
    bool drawsDirty{true};
    uint64_t drawsVersion{0};
    // The draw list on the GPU (gpu::shared::DrawRecord each), rewritten whole when the
    // list changes; `uploadedVersion` is the version it holds.
    GpuBuffer drawTable;
    uint32_t drawTableCapacity{0};
    uint64_t uploadedVersion{~0ull};
    std::vector<gpu::shared::DrawRecord> drawRecords;
    uint32_t meshletEntries{0};   // the uploaded table's meshlets, all draws

    // The lights' emitter instances (lightEmitter()), one per light that has one, in light
    // order; their meshes and material are registered by the first light that needs them.
    bool emittersEnabled{true};
    std::vector<uint32_t> emitterSlots;
    MeshHandle emitterSphere{}, emitterQuad{};
    MaterialHandle emitterMaterial{};
    bool emitterAssetsReady{false};

    [[nodiscard]] bool rt() const noexcept { return rtAccel != nullptr && rtAccel->isSupported(); }

    void retire(std::vector<GpuBuffer>& bufs) {
        for (GpuBuffer& b : bufs)
            retireQueue.push_back(Retired{frameCounter + MAX_FRAMES_IN_FLIGHT, b, {}});
        bufs.clear();
    }

    void destroyRetired(Retired& r) {
        allocator.destroyBuffer(r.buf);
        if (rtAccel && (r.tlas.as != VK_NULL_HANDLE || r.tlas.buffer != VK_NULL_HANDLE))
            rtAccel->destroyRetired(r.tlas);
        if (rtAccel && r.blas != INVALID_BLAS) rtAccel->destroyBLAS(r.blas);
    }

    [[nodiscard]] GpuBuffer createTlasInstanceBuffer(uint32_t capacity) {
        return allocator.createBuffer(VkDeviceSize{capacity} * kTlasInstanceBytes,
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      AllocationUsage::GpuOnly);
    }

    // Recomputes the slot's TLAS instance from the instance table. `structural`:
    // the slot was created or destroyed (a reused slot never refits). A changed
    // mask (visibility) or BLAS reference (mesh) is structural too; a changed
    // transform alone is a refit.
    void stageTlasInstance(uint32_t slot, bool structural) {
        if (!rt()) return;
        if (slot >= tlasHost.size())
            tlasHost.resize(std::max<std::size_t>(slot + 1, tlasHost.size() * 2), VkAccelerationStructureInstanceKHR{});
        const InstanceGpu& g = instances.gpu(slot);
        const MeshInfo* mesh = meshes.info(MeshHandle{g.mesh, 0});

        VkAccelerationStructureInstanceKHR vi{};
        const glm::mat4 rowMajor = glm::transpose(g.world);   // rows 0..2 = the 3x4 matrix
        std::memcpy(&vi.transform, &rowMajor, sizeof(VkTransformMatrixKHR));
        vi.instanceCustomIndex = slot & 0xFFFFFFu;   // 24 bits
        // A reference of 0 makes the instance inactive (no BLAS: failed build). A
        // destroyed slot gets 0 too: its mesh may be released and the BLAS destroyed
        // while the slot sits unused (releaseMesh).
        const bool live = instances.live(slot);
        vi.accelerationStructureReference = mesh && live ? rtAccel->getBlasAddress(mesh->blas) : 0;
        const bool visible = live && (g.flags & kInstanceVisible) != 0;
        // A light's emitter mesh is seen by the path tracer's camera rays only (kRayMaskCamera).
        vi.mask = !visible ? 0u
                : (g.flags & kInstanceEmitter) != 0 ? gpu::shared::kTlasMaskEmitter : gpu::shared::kTlasMaskScene;
        vi.instanceShaderBindingTableRecordOffset = 0;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

        VkAccelerationStructureInstanceKHR& cur = tlasHost[slot];
        if (structural || cur.mask != vi.mask ||
            cur.accelerationStructureReference != vi.accelerationStructureReference)
            tlasStructural = true;
        if (structural || std::memcmp(&cur, &vi, sizeof(vi)) != 0) {
            cur = vi;
            tlasDirty.insert(slot);
        }
    }

    [[nodiscard]] bool tlasNeedsGrowth() const noexcept {
        return rt() && instances.highWater() > tlasCapacity;
    }

    // Grows the device TLAS instance buffer (old -> new copy, old retired), then
    // records the dirty slots' copies. Returns false (dirty kept) on allocation failure.
    bool recordTlasUploads(detail::StagingBatch& batch, std::vector<GpuBuffer>& retired) {
        if (tlasNeedsGrowth()) {
            uint32_t cap = std::max(tlasCapacity, InstanceTable::kInitialCapacity);
            while (cap < instances.highWater()) cap *= 2;
            GpuBuffer bigger = createTlasInstanceBuffer(cap);
            if (!bigger.isValid()) {
                OHAO_LOG(Error, render, "RenderScene: TLAS instance buffer allocation failed ({} slots)", cap);
                return false;
            }
            const VkBufferCopy copy{0, 0, VkDeviceSize{tlasCapacity} * kTlasInstanceBytes};
            vkCmdCopyBuffer(batch.cmd, tlasInstances.buffer, bigger.buffer, 1, &copy);
            VkMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &b, 0, nullptr, 0, nullptr);
            retired.push_back(tlasInstances);
            tlasInstances = bigger;
            tlasCapacity = cap;
        }
        for (auto it = tlasDirty.begin(); it != tlasDirty.end();) {
            const uint32_t first = *it;
            uint32_t last = first;
            for (++it; it != tlasDirty.end() && *it == last + 1; ++it) last = *it;
            batch.upload(tlasInstances.buffer, VkDeviceSize{first} * kTlasInstanceBytes, &tlasHost[first],
                         VkDeviceSize{last - first + 1} * kTlasInstanceBytes);
        }
        tlasDirty.clear();
        return true;
    }

    // After the copies and their barrier: Build on a structural change (or no TLAS
    // yet), Refit on transform-only changes, else nothing.
    void recordTlas(VkCommandBuffer cmd, bool changed) {
        stats.lastTlasOp = RenderSceneStats::TlasOp::None;
        const uint32_t count = instances.highWater();
        if (!rt() || count == 0 || count > tlasCapacity) return;
        const bool build = tlasStructural || rtAccel->getTLAS() == VK_NULL_HANDLE;
        if (!build && !changed) return;
        VkBufferDeviceAddressInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        ai.buffer = tlasInstances.buffer;
        const bool recorded = rtAccel->buildTLASFromDevice(cmd, vkGetBufferDeviceAddress(info.device, &ai), count, build);
        for (const RetiredTlas& r : rtAccel->takeRetired())
            retireQueue.push_back(Retired{frameCounter + MAX_FRAMES_IN_FLIGHT, GpuBuffer{}, r});
        if (!recorded) {             // allocation failed: the input is uploaded, so a
            tlasStructural = true;   // full build next frame covers this frame's changes
            return;
        }
        tlasStructural = false;
        stats.lastTlasOp = build ? RenderSceneStats::TlasOp::Build : RenderSceneStats::TlasOp::Refit;
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
            OHAO_LOG(Error, render, "RenderScene: staging allocation failed ({} bytes)", cap);
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
            for (std::size_t s = 0; s < mesh->submeshes.size(); ++s) {
                const SubmeshDesc& sub = mesh->submeshes[s];
                const bool oneSided = !materials.doubleSided(instances.materialIndexFor(slot, sub.materialSlot));
                const std::array<uint32_t, 2> ml = s < mesh->submeshMeshlets.size() ? mesh->submeshMeshlets[s]
                                                                                   : std::array<uint32_t, 2>{0u, 0u};
                draws.push_back(Draw{slot, mesh->indexOffset + sub.firstIndex, sub.indexCount,
                                     static_cast<int32_t>(mesh->vertexOffset), sub.materialSlot,
                                     mesh->aabbMin, mesh->aabbMax, &g.world, oneSided, ml[0], ml[1],
                                     (g.flags & kInstanceEmitter) != 0});
            }
        }
        drawsDirty = false;
        ++drawsVersion;
    }

    // Grows the device draw table if needed (the old one retired), then records the whole
    // list's upload. False (version kept stale) on allocation failure.
    bool recordDrawTable(detail::StagingBatch& batch, std::vector<GpuBuffer>& retired) {
        const auto n = static_cast<uint32_t>(draws.size());
        if (n > drawTableCapacity) {
            uint32_t cap = std::max(drawTableCapacity, InstanceTable::kInitialCapacity);
            while (cap < n) cap *= 2;
            GpuBuffer bigger = allocator.createBuffer(VkDeviceSize{cap} * sizeof(gpu::shared::DrawRecord),
                                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                      AllocationUsage::GpuOnly);
            if (!bigger.isValid()) {
                OHAO_LOG(Error, render, "RenderScene: draw table allocation failed ({} draws)", cap);
                return false;
            }
            if (drawTable.isValid()) retired.push_back(drawTable);
            drawTable = bigger;
            drawTableCapacity = cap;
        }
        drawRecords.resize(n);
        uint32_t entries = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const Draw& d = draws[i];
            gpu::shared::DrawRecord& r = drawRecords[i];
            r.draw = {d.firstIndex, d.indexCount, static_cast<uint32_t>(d.vertexOffset), d.slot};
            r.info = {d.materialSlot,
                      (d.oneSided ? gpu::shared::kDrawOneSided : 0u) | (d.emitter ? gpu::shared::kDrawEmitter : 0u),
                      d.firstMeshlet, d.meshletCount};
            r.aabbMin = {d.aabbMin.x, d.aabbMin.y, d.aabbMin.z, std::bit_cast<float>(entries)};
            r.aabbMax = {d.aabbMax.x, d.aabbMax.y, d.aabbMax.z, 0.0f};
            entries += d.meshletCount;
        }
        meshletEntries = entries;
        batch.upload(drawTable.buffer, 0, drawRecords.data(), VkDeviceSize{n} * sizeof(gpu::shared::DrawRecord));
        uploadedVersion = drawsVersion;
        return true;
    }

    [[nodiscard]] bool drawTableStale() const noexcept { return uploadedVersion != drawsVersion && !draws.empty(); }

    // The emitter meshes (the unit sphere, 24 x 12, and the unit quad, both counter-clockwise seen
    // from outside, the side their normals face) and their material (black, lit by nothing:
    // the shaders read an emitter's radiance from its instance).
    bool ensureEmitterAssets() {
        if (emitterAssetsReady) return true;
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        constexpr int kSectors = 24, kStacks = 12;
        for (int i = 0; i <= kStacks; ++i) {
            const float theta = glm::pi<float>() * static_cast<float>(i) / kStacks;
            for (int j = 0; j <= kSectors; ++j) {
                const float phi = glm::two_pi<float>() * static_cast<float>(j) / kSectors;
                Vertex x{};
                x.position = {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                x.normal = x.position;
                x.color = glm::vec3(1.0f);
                x.texCoord = {static_cast<float>(j) / kSectors, static_cast<float>(i) / kStacks};
                v.push_back(x);
            }
        }
        const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            const glm::vec3 pa = v[a].position, pb = v[b].position, pc = v[c].position;
            if (glm::dot(glm::cross(pb - pa, pc - pa), pa + pb + pc) < 0.0f) std::swap(b, c);   // outward
            idx.insert(idx.end(), {a, b, c});
        };
        for (int i = 0; i < kStacks; ++i)
            for (int j = 0; j < kSectors; ++j) {
                const uint32_t a = static_cast<uint32_t>(i * (kSectors + 1) + j), b = a + kSectors + 1;
                if (i != 0) tri(a, b, a + 1);                   // the poles' quads are triangles
                if (i != kStacks - 1) tri(a + 1, b, b + 1);
            }
        MeshData sphere{v, idx, {}, {}, "light emitter sphere"};
        std::vector<GpuBuffer> retired;
        uint32_t grown = 0;
        emitterSphere = meshes.registerMesh(sphere, retired, grown);

        std::vector<Vertex> q(4);
        const glm::vec3 corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        for (int i = 0; i < 4; ++i) {
            q[i].position = corners[i];
            q[i].normal = {0.0f, 0.0f, 1.0f};
            q[i].color = glm::vec3(1.0f);
            q[i].texCoord = glm::vec2(corners[i]);
        }
        const std::vector<uint32_t> qi = {0, 1, 2, 0, 2, 3};
        MeshData quad{q, qi, {}, {}, "light emitter quad"};
        emitterQuad = meshes.registerMesh(quad, retired, grown);
        stats.arenaGrowths += grown;
        retire(retired);

        MaterialDesc black;
        black.baseColor = glm::vec3(0.0f);
        black.roughness = 1.0f;
        emitterMaterial = materials.add(black);   // double-sided: an area light is black behind
        emitterAssetsReady = meshes.info(emitterSphere) && meshes.info(emitterQuad);
        if (!emitterAssetsReady) OHAO_LOG(Error, render, "RenderScene: the light emitter meshes could not be built");
        return emitterAssetsReady;
    }

    // One emitter instance per light that has one, in light order, slots reused in that order:
    // an unchanged light rewrites identical bytes and uploads nothing.
    void syncEmitters() {
        std::vector<LightEmitter> want;
        if (emittersEnabled)
            for (const LightDesc& d : lights.descs())
                if (const auto e = lightEmitter(d)) want.push_back(*e);
        if (!want.empty() && !ensureEmitterAssets()) want.clear();
        const MaterialHandle mats[1] = {emitterMaterial};
        for (std::size_t i = 0; i < want.size(); ++i) {
            InstanceDesc d{};
            d.mesh = want[i].quad ? emitterQuad : emitterSphere;
            d.materials = mats;
            d.world = d.prevWorld = want[i].world;
            d.emitter = true;
            d.giAlbedoPacked = glm::vec4(want[i].radiance, 1.0f);
            if (i < emitterSlots.size()) {
                instances.update(emitterSlots[i], d, materials.count());
                stageTlasInstance(emitterSlots[i], false);
            } else {
                const uint32_t slot = instances.create(d, materials.count());
                stageTlasInstance(slot, true);
                emitterSlots.push_back(slot);
            }
        }
        while (emitterSlots.size() > want.size()) {
            instances.destroy(emitterSlots.back());
            stageTlasInstance(emitterSlots.back(), true);
            emitterSlots.pop_back();
        }
    }
};

RenderScene::RenderScene() : m(std::make_unique<Impl>()) {}
RenderScene::~RenderScene() { shutdown(); }

bool RenderScene::initialize(const InitInfo& info) {
    if (m->initialized) return true;
    m->info = info;
    if (!m->allocator.initialize(info.instance, info.physical, info.device)) {
        OHAO_LOG(Error, render, "RenderScene: GpuAllocator initialization failed");
        return false;
    }
    if (info.rayTracing) {
        // TLAS readers: ray tracing pipeline shaders, and -- only when rayQuery is
        // enabled, else naming them with ACCELERATION_STRUCTURE_READ is invalid
        // (VUID-vkCmdPipelineBarrier-dstAccessMask-06257) -- compute and fragment
        // shaders doing inline ray queries.
        VkPipelineStageFlags consumers = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        if (info.rayQuery) consumers |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        m->rtAccel = std::make_unique<RTAccelerationStructure>();
        if (!m->rtAccel->init(info.device, info.physical, info.queue, info.queueFamily, info.commandPool,
                              consumers)) {
            OHAO_LOG(Warn, render, "RenderScene: ray tracing unavailable: meshes get no BLAS");
            m->rtAccel.reset();
        }
    }
    const MeshRegistry::Context ctx{&m->allocator, info.device, info.queue, info.commandPool,
                                    m->rt() ? m->rtAccel.get() : nullptr};
    m->initialized = true;   // so shutdown() releases whatever was created below
    if (!m->meshes.initialize(ctx) || !m->materials.initialize(m->allocator) ||
        !m->instances.initialize(m->allocator) || !m->lights.initialize(m->allocator)) {
        shutdown();
        return false;
    }
    if (m->rt()) {
        m->tlasInstances = m->createTlasInstanceBuffer(InstanceTable::kInitialCapacity);
        if (!m->tlasInstances.isValid()) {
            OHAO_LOG(Error, render, "RenderScene: TLAS instance buffer allocation failed");
            shutdown();
            return false;
        }
        m->tlasCapacity = InstanceTable::kInitialCapacity;
    }
    return true;
}

void RenderScene::shutdown() {
    if (!m || !m->initialized) return;
    vkDeviceWaitIdle(m->info.device);
    for (auto& r : m->retireQueue) m->destroyRetired(r);
    m->retireQueue.clear();
    m->allocator.destroyBuffer(m->tlasInstances);
    m->tlasInstances = GpuBuffer{};
    m->allocator.destroyBuffer(m->drawTable);
    m->drawTable = GpuBuffer{};
    m->drawTableCapacity = 0;
    m->uploadedVersion = ~0ull;
    m->tlasCapacity = 0;
    m->tlasHost.clear();
    m->tlasDirty.clear();
    m->tlasStructural = false;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m->allocator.destroyBuffer(m->staging[i]);
        m->staging[i] = GpuBuffer{};
        m->stagingCapacity[i] = 0;
    }
    m->lights.shutdown();
    m->instances.shutdown();
    m->materials.shutdown();
    m->meshes.shutdown();
    if (m->rtAccel) {
        m->rtAccel->destroy();
        m->rtAccel.reset();
    }
    m->allocator.shutdown();
    m->draws.clear();
    m->emitterSlots.clear();
    m->emitterAssetsReady = false;
    m->initialized = false;
}

MeshHandle RenderScene::registerMesh(const MeshData& data) {
    if (!m->initialized) return {};
    std::vector<GpuBuffer> retired;
    uint32_t grown = 0;
    const MeshHandle h = m->meshes.registerMesh(data, retired, grown);
    m->stats.arenaGrowths += grown;
    m->retire(retired);
    return h;
}

void RenderScene::releaseMesh(MeshHandle h) {
    if (!m->initialized) return;
    // Frames in flight may still trace the BLAS: it is destroyed once they retire.
    const BlasHandle blas = m->meshes.release(h);
    if (blas != INVALID_BLAS)
        m->retireQueue.push_back(Impl::Retired{m->frameCounter + MAX_FRAMES_IN_FLIGHT, GpuBuffer{}, {}, blas});
}

MaterialHandle RenderScene::registerMaterial(const MaterialDesc& desc) {
    if (!m->initialized) return {};
    return m->materials.add(desc);
}

uint32_t RenderScene::createInstance(const InstanceDesc& desc) {
    if (!m->initialized) return kInvalidIndex;
    if (!m->meshes.info(desc.mesh)) {
        OHAO_LOG(Error, render, "RenderScene: createInstance: unknown mesh handle");
        return kInvalidIndex;
    }
    const uint32_t slot = m->instances.create(desc, m->materials.count());
    m->stageTlasInstance(slot, true);
    return slot;
}

void RenderScene::updateInstance(uint32_t slot, const InstanceDesc& desc) {
    if (!m->initialized) return;
    if (!m->meshes.info(desc.mesh)) {
        OHAO_LOG(Error, render, "RenderScene: updateInstance: unknown mesh handle");
        return;
    }
    if (!m->instances.live(slot)) {
        OHAO_LOG(Warn, render, "RenderScene: updateInstance on dead slot {} ignored", slot);
        return;
    }
    m->instances.update(slot, desc, m->materials.count());
    m->stageTlasInstance(slot, false);
}

void RenderScene::destroyInstance(uint32_t slot) {
    if (!m->initialized || !m->instances.live(slot)) return;
    m->instances.destroy(slot);
    m->stageTlasInstance(slot, true);
}

void RenderScene::setLights(std::span<const LightDesc> lights) {
    if (!m->initialized) return;
    m->lights.set(lights);
    m->syncEmitters();
}

void RenderScene::setLightEmitters(bool enabled) {
    if (!m->initialized || enabled == m->emittersEnabled) return;
    m->emittersEnabled = enabled;
    m->syncEmitters();
}

std::span<const uint32_t> RenderScene::lightEmitterSlots() const { return m->emitterSlots; }

void RenderScene::setEnvMapTextureIndex(uint32_t texIdx) {
    if (!m->initialized) return;
    m->lights.setEnvMapTextureIndex(texIdx);
}

void RenderScene::setEnvIntensity(float scale) {
    if (!m->initialized) return;
    m->lights.setEnvIntensity(scale);
}

void RenderScene::recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot) {
    if (!m->initialized || cmd == VK_NULL_HANDLE || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    const auto t0 = std::chrono::steady_clock::now();
    m->stats.instanceBytesUploaded = 0;
    m->stats.lightBytesUploaded = 0;

    const bool tlasChanged = !m->tlasDirty.empty();
    bool tlasUploaded = !tlasChanged;
    if (m->instances.consumeDrawsDirty() || m->drawsDirty) m->rebuildDraws();
    const bool drawUpload = m->drawTableStale();
    const VkDeviceSize need = m->materials.pendingBytes() + m->instances.pendingBytes() +
                              m->lights.pendingBytes() + m->tlasDirty.size() * kTlasInstanceBytes +
                              (drawUpload ? m->draws.size() * sizeof(gpu::shared::DrawRecord) : 0u);
    if (need > 0 || m->instances.needsGpuGrowth() || m->tlasNeedsGrowth()) {
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
            m->stats.instanceTableGrowths = m->instances.gpuGrowths();
            if (m->rt()) tlasUploaded = m->recordTlasUploads(batch, retired);
            if (drawUpload) (void)m->recordDrawTable(batch, retired);
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

    // A failed upload keeps the TLAS input dirty: no build until it lands.
    m->stats.lastTlasOp = RenderSceneStats::TlasOp::None;
    if (tlasUploaded) m->recordTlas(cmd, tlasChanged);

    m->stats.recordCpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void RenderScene::onFrameRetired() {
    if (!m->initialized) return;
    ++m->frameCounter;
    auto& q = m->retireQueue;
    auto keep = q.begin();
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->retireAt <= m->frameCounter) m->destroyRetired(*it);
        else *keep++ = *it;
    }
    q.erase(keep, q.end());
}

const MeshInfo* RenderScene::meshInfo(MeshHandle h) const { return m->meshes.info(h); }

uint32_t RenderScene::materialIndexFor(uint32_t slot, uint32_t materialSlot) const {
    return m->instances.materialIndexFor(slot, materialSlot);
}

bool RenderScene::rtSupported() const noexcept { return m->rt(); }
RTAccelerationStructure* RenderScene::rtAccel() const noexcept { return m->rt() ? m->rtAccel.get() : nullptr; }

VkBuffer RenderScene::vertexArena() const { return m->meshes.vertexArena(); }
VkBuffer RenderScene::indexArena() const { return m->meshes.indexArena(); }
VkBuffer RenderScene::triangleSlotBuffer() const { return m->meshes.triangleSlotBuffer(); }
VkBuffer RenderScene::meshTable() const { return m->meshes.meshTable(); }
VkBuffer RenderScene::materialBuffer() const { return m->materials.buffer(); }
VkBuffer RenderScene::materialRefBuffer() const { return m->instances.materialRefBuffer(); }
VkBuffer RenderScene::instanceBuffer() const { return m->instances.instanceBuffer(); }
std::span<const LightDesc> RenderScene::lights() const { return m->lights.descs(); }
std::span<const LightData> RenderScene::rasterLights() const { return m->lights.raster(); }
uint64_t RenderScene::rasterLightsVersion() const { return m->lights.rasterVersion(); }
uint32_t RenderScene::shadowCastersWithoutChannel() const { return m->lights.shadowCastersWithoutChannel(); }
std::span<const LightDesc> RenderScene::shadowLights() const { return m->lights.shadowLights(); }
VkBuffer RenderScene::rtLightBuffer() const { return m->lights.rtBuffer(); }
uint32_t RenderScene::rtLightCount() const { return m->lights.rtCount(); }

std::span<const RenderScene::Draw> RenderScene::draws() const {
    if (m->instances.consumeDrawsDirty() || m->drawsDirty) m->rebuildDraws();
    return m->draws;
}

VkBuffer RenderScene::drawTable() const { return m->drawTable.buffer; }
// The uploaded list's length: zero until recordUpdates has uploaded the current version.
uint32_t RenderScene::drawCount() const {
    return m->uploadedVersion == m->drawsVersion ? static_cast<uint32_t>(m->draws.size()) : 0u;
}
uint64_t RenderScene::drawsVersion() const { return m->drawsVersion; }
uint32_t RenderScene::meshletEntryCount() const {
    return m->uploadedVersion == m->drawsVersion ? m->meshletEntries : 0u;
}
VkBuffer RenderScene::meshletArena() const { return m->meshes.meshletArena(); }
VkBuffer RenderScene::meshletVertexArena() const { return m->meshes.meshletVertexArena(); }
VkBuffer RenderScene::meshletTriangleArena() const { return m->meshes.meshletTriangleArena(); }

const RenderSceneStats& RenderScene::stats() const noexcept { return m->stats; }
VkAccelerationStructureKHR RenderScene::tlas() const { return m->rt() ? m->rtAccel->getTLAS() : VK_NULL_HANDLE; }

glm::mat4 RenderScene::tlasTransform(uint32_t slot) const {
    glm::mat4 out(1.0f);
    if (slot >= m->tlasHost.size()) return out;
    const VkTransformMatrixKHR& t = m->tlasHost[slot].transform;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) out[col][row] = t.matrix[row][col];
    return out;
}
bool RenderScene::empty() const noexcept { return m->instances.liveCount() == 0; }

} // namespace ohao::render
