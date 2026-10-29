#include "render/scene/render_scene.hpp"
#include "render/scene/instance_table.hpp"
#include "render/scene/light_table.hpp"
#include "render/scene/material_table.hpp"
#include "render/scene/mesh_registry.hpp"
#include "render/scene/skin_pass.hpp"
#include "render/frame/gpu_pass_timer.hpp"
#include "gpu/shared_layout.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
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
    std::vector<LightDesc> sceneLights;       // setLights' list
    std::vector<LightDesc> transientLights;   // setTransientLights': the effects' flashes this frame
    // The table's list: the scene's lights, then the transient ones.
    void setLightTable() {
        std::vector<LightDesc> all = sceneLights;
        all.insert(all.end(), transientLights.begin(), transientLights.end());
        lights.set(all);
    }

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
    std::set<uint32_t> alphaSlots;   // live, visible instances with an alpha material (FORCE_NO_OPAQUE)
    bool tlasStructural{false};

    RenderSceneStats stats;
    VfxFrame vfx;   // setVfx
    std::vector<Draw> draws;
    std::vector<uint32_t> maskDraws, transparentDraws;   // indices into draws
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
    std::vector<uint32_t> presentedSlots;   // setPresentedSpheres: the emitter sphere mesh, not an emitter
    MeshHandle emitterSphere{}, emitterQuad{};
    MaterialHandle emitterMaterial{};
    bool emitterAssetsReady{false};
    std::optional<glm::vec3> viewer;   // setViewerPosition: spot emitters shine toward it
    bool spotEmitters{false};          // a spot light has an emitter: they follow the viewer

    // Skinned instances' meshes (registerSkinnedInstance), by mesh index. A pose is skinned when
    // its palettes differ from what the mesh shows (`shown`: the last palettes skinned); a pose
    // whose previous palette equals its current one has no motion, and once skinned twice in a
    // row like that the mesh is left as it is (no skin, bounds or refit).
    struct Skinned {
        MeshHandle mesh;
        std::vector<glm::mat4> current, previous;
        bool pending{false};
        uint32_t refits{0};   // since the BLAS's last build
    };
    std::map<uint32_t, Skinned> skinned;
    SkinPass skinPass;
    bool skinPassTried{false};
    bool skinnedBlasChanged{false};   // a BLAS was refit or rebuilt this frame: the TLAS refits
    std::vector<SkinPass::Job> skinJobs;
    std::vector<uint32_t> skinJobMeshes;
    std::vector<RTAccelerationStructure::BlasUpdate> blasUpdates;   // the frame's BLAS refit batch
    std::vector<uint32_t> blasUpdateMeshes;                          // its skinned indices

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
        // Its materials' alpha modes: an instance with an alpha-tested or blended material is
        // non-opaque to every ray (the BLAS is per mesh and opaque), so ray queries see its
        // triangles as candidates and test them by their own material (rt/alpha_test.slang,
        // pt_anyhit); one whose every material is blended is seen by camera rays only.
        bool anyAlpha = false, allBlend = g.materialRefCount > 0;
        if ((g.flags & (kInstanceEmitter | kInstanceViewModel)) == 0)
            for (uint32_t k = 0; k < g.materialRefCount; ++k) {
                const AlphaMode a = materials.alphaMode(instances.materialIndexFor(slot, k));
                anyAlpha = anyAlpha || a != AlphaMode::Opaque;
                allBlend = allBlend && a == AlphaMode::Blend;
            }
        else
            allBlend = false;
        // A light's emitter mesh and a first-person gun are seen by the path tracer's camera rays
        // and its near-mirrors' first specular rays only (kRayMaskCamera).
        vi.mask = !visible ? 0u
                : (g.flags & kInstanceEmitter) != 0   ? gpu::shared::kTlasMaskEmitter
                : (g.flags & kInstanceViewModel) != 0 ? gpu::shared::kTlasMaskViewModel
                : allBlend                            ? gpu::shared::kTlasMaskTransparent
                                                      : gpu::shared::kTlasMaskScene;
        vi.instanceShaderBindingTableRecordOffset = 0;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                   (anyAlpha ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0u);

        // The path tracer forces opacity on its rays while no instance has an alpha material.
        if (anyAlpha && visible) alphaSlots.insert(slot);
        else alphaSlots.erase(slot);
        lights.setAlphaRays(!alphaSlots.empty());

        VkAccelerationStructureInstanceKHR& cur = tlasHost[slot];
        if (structural || cur.mask != vi.mask || cur.flags != vi.flags ||
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
        maskDraws.clear();
        transparentDraws.clear();
        for (uint32_t slot = 0; slot < instances.highWater(); ++slot) {
            if (!instances.live(slot)) continue;
            const InstanceGpu& g = instances.gpu(slot);
            if ((g.flags & kInstanceVisible) == 0) continue;
            const MeshInfo* mesh = meshes.info(MeshHandle{g.mesh, 0});
            if (!mesh) continue;
            for (std::size_t s = 0; s < mesh->submeshes.size(); ++s) {
                const SubmeshDesc& sub = mesh->submeshes[s];
                const uint32_t material = instances.materialIndexFor(slot, sub.materialSlot);
                const bool oneSided = !materials.doubleSided(material);
                // A light's emitter and a first-person gun stay opaque whatever their material.
                const AlphaMode alpha = (g.flags & (kInstanceEmitter | kInstanceViewModel)) != 0
                                            ? AlphaMode::Opaque
                                            : materials.alphaMode(material);
                const std::array<uint32_t, 2> ml = s < mesh->submeshMeshlets.size() ? mesh->submeshMeshlets[s]
                                                                                   : std::array<uint32_t, 2>{0u, 0u};
                draws.push_back(Draw{slot, mesh->indexOffset + sub.firstIndex, sub.indexCount,
                                     static_cast<int32_t>(mesh->vertexOffset), sub.materialSlot,
                                     mesh->aabbMin, mesh->aabbMax, &g.world, oneSided, ml[0], ml[1],
                                     (g.flags & kInstanceEmitter) != 0, (g.flags & kInstanceViewModel) != 0,
                                     mesh->skinned, alpha});
                const auto index = static_cast<uint32_t>(draws.size() - 1);
                if (alpha == AlphaMode::Mask) maskDraws.push_back(index);
                if (alpha == AlphaMode::Blend) transparentDraws.push_back(index);
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
                      (d.oneSided ? gpu::shared::kDrawOneSided : 0u) | (d.emitter ? gpu::shared::kDrawEmitter : 0u) |
                          (d.viewModel ? gpu::shared::kDrawViewModel : 0u) | (d.skinned ? gpu::shared::kDrawSkinned : 0u) |
                          (d.alpha == AlphaMode::Mask ? gpu::shared::kDrawMask : 0u) |
                          (d.alpha == AlphaMode::Blend ? gpu::shared::kDrawTransparent : 0u),
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
        std::vector<glm::mat4> motion;   // each emitter's light's lastFrameMotion
        std::vector<uint32_t> rtIndex;   // its light's index in the path tracer's light buffer
        spotEmitters = false;
        uint32_t rt = 0;   // the RT light buffer holds every light but emissive proxies, in this order
        if (emittersEnabled)
            for (const LightDesc& d : lights.descs()) {
                if (d.kind == LightKind::EmissiveProxy) continue;
                if (const auto e = lightEmitter(d, viewer)) {
                    want.push_back(*e);
                    motion.push_back(d.lastFrameMotion);
                    rtIndex.push_back(rt);
                    spotEmitters = spotEmitters || d.kind == LightKind::Spot;
                }
                ++rt;
            }
        if (!want.empty() && !ensureEmitterAssets()) {
            want.clear();
            rtIndex.clear();
        }
        lights.setEmitterLights(rtIndex);
        const MaterialHandle mats[1] = {emitterMaterial};
        for (std::size_t i = 0; i < want.size(); ++i) {
            InstanceDesc d{};
            d.mesh = want[i].quad ? emitterQuad : emitterSphere;
            d.materials = mats;
            d.world = want[i].world;
            d.prevWorld = motion[i] * want[i].world;   // where the light was: its motion vectors
            d.emitter = true;
            // rgb its radiance (what the camera sees), w its light (rt/path_tracer.slang's MIS).
            d.giAlbedoPacked = glm::vec4(want[i].radiance, static_cast<float>(rtIndex[i]));
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
    m->skinPass.shutdown();
    m->skinPassTried = false;
    m->skinned.clear();
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
    m->presentedSlots.clear();
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
    m->skinned.erase(h.index);
    m->stats.skinning.instances = static_cast<uint32_t>(m->skinned.size());
}

std::vector<Vertex> RenderScene::readMeshVertices(MeshHandle h) {
    if (!m->initialized) return {};
    return m->meshes.readVertices(h);
}

MeshHandle RenderScene::registerSkinnedInstance(MeshHandle base) {
    if (!m->initialized) return {};
    if (!m->skinPassTried) {
        m->skinPassTried = true;
        if (!m->skinPass.initialize(m->info.device, m->info.physical)) m->skinPass.shutdown();
    }
    std::vector<GpuBuffer> retired;
    uint32_t grown = 0;
    const MeshHandle h = m->meshes.registerSkinnedCopy(base, retired, grown);
    m->stats.arenaGrowths += grown;
    m->retire(retired);
    if (!h.valid()) {
        OHAO_LOG(Error, render, "RenderScene: registerSkinnedInstance: no mesh for base {}", base.index);
        return {};
    }
    m->skinned[h.index] = Impl::Skinned{h, {}, {}, false, 0};
    m->stats.skinning.instances = static_cast<uint32_t>(m->skinned.size());
    return h;
}

void RenderScene::setSkinPalette(MeshHandle mesh, std::span<const glm::mat4> current,
                                 std::span<const glm::mat4> previous, const glm::vec3& boundsMin,
                                 const glm::vec3& boundsMax) {
    if (!m->initialized) return;
    const auto it = m->skinned.find(mesh.index);
    if (it == m->skinned.end() || !m->meshes.info(mesh) || current.size() != previous.size()) return;
    Impl::Skinned& s = it->second;
    const auto same = [](const std::vector<glm::mat4>& a, std::span<const glm::mat4> b) {
        return a.size() == b.size() && (b.empty() || std::memcmp(a.data(), b.data(), b.size_bytes()) == 0);
    };
    if (!same(s.current, current) || !same(s.previous, previous)) {
        s.current.assign(current.begin(), current.end());
        s.previous.assign(previous.begin(), previous.end());
        s.pending = true;
    }
    if (m->meshes.setBounds(mesh, boundsMin, boundsMax)) m->drawsDirty = true;
}

void RenderScene::recordSkinning(VkCommandBuffer cmd, uint32_t frameSlot, GpuPassTimer* timer) {
    auto& st = m->stats.skinning;
    st.skinned = 0;
    st.vertices = 0;
    if (!m->initialized || cmd == VK_NULL_HANDLE || frameSlot >= MAX_FRAMES_IN_FLIGHT || !m->skinPass.ready()) return;
    m->skinJobs.clear();
    m->skinJobMeshes.clear();
    for (auto& [index, s] : m->skinned) {
        if (!s.pending || s.current.empty()) continue;
        const MeshInfo* mesh = m->meshes.info(s.mesh);
        if (!mesh) continue;
        SkinPass::Job j;
        j.baseVertex = mesh->baseVertexOffset;
        j.vertex = mesh->vertexOffset;
        j.vertexCount = mesh->vertexCount;
        j.firstMeshlet = mesh->firstMeshlet;
        j.meshletCount = mesh->meshletCount;
        j.current = s.current;
        j.previous = s.previous;
        m->skinJobs.push_back(j);
        m->skinJobMeshes.push_back(index);
    }
    if (m->skinJobs.empty() ||
        !m->skinPass.prepare(frameSlot, m->skinJobs, m->meshes.vertexArena(), m->meshes.meshletArena(),
                             m->meshes.meshletVertexArena()))
        return;

    if (timer) timer->begin(cmd, "skin");
    m->skinPass.recordSkin(cmd, frameSlot, m->skinJobs);
    if (timer) timer->end(cmd);
    if (timer) timer->begin(cmd, "skin.bounds");
    m->skinPass.recordBounds(cmd, frameSlot, m->skinJobs);
    if (timer) timer->end(cmd);

    if (m->rt()) {
        if (timer) timer->begin(cmd, "rt.blas.refit");
        // Every BLAS the frame updates in one build call: side by side on the GPU, not one after
        // another behind a barrier each.
        m->blasUpdates.clear();
        m->blasUpdateMeshes.clear();
        for (const uint32_t index : m->skinJobMeshes) {
            const Impl::Skinned& s = m->skinned[index];
            const MeshInfo* mesh = m->meshes.info(s.mesh);
            if (!mesh || mesh->blas == INVALID_BLAS) continue;
            // A refit keeps the build's tree and moves its boxes: after many, the boxes of a
            // pose far from the built one overlap and traversal slows (OPEN ruling 12).
            m->blasUpdates.push_back({mesh->blas, s.refits >= kBlasRefitsPerRebuild, false});
            m->blasUpdateMeshes.push_back(index);
        }
        const bool any = m->rtAccel->recordBlasUpdates(cmd, m->blasUpdates, m->meshes.vertexArena(),
                                                       m->meshes.indexArena()) > 0;
        for (std::size_t k = 0; k < m->blasUpdates.size(); ++k) {
            if (!m->blasUpdates[k].recorded) continue;
            Impl::Skinned& s = m->skinned[m->blasUpdateMeshes[k]];
            if (m->blasUpdates[k].rebuild) {
                s.refits = 0;
                ++st.blasRebuilds;
            } else {
                ++s.refits;
                ++st.blasRefits;
            }
        }
        if (any) {
            ++st.blasUpdateCalls;
            // The BLASes to the TLAS build (recordUpdates) and the ray-query and trace readers.
            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            after.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &after, 0, nullptr, 0,
                                 nullptr);
            m->skinnedBlasChanged = true;
        }
        if (timer) timer->end(cmd);
    }
    for (const uint32_t index : m->skinJobMeshes) {
        Impl::Skinned& s = m->skinned[index];
        s.pending = false;
        ++st.skinned;
        if (const MeshInfo* mesh = m->meshes.info(s.mesh)) st.vertices += mesh->vertexCount;
    }
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
    m->sceneLights.assign(lights.begin(), lights.end());
    m->setLightTable();
    m->syncEmitters();
}

void RenderScene::setPresentedSpheres(std::span<const PresentedSphere> spheres) {
    if (!m->initialized || (spheres.empty() && m->presentedSlots.empty())) return;
    if (!spheres.empty() && !m->ensureEmitterAssets()) return;
    const MaterialHandle mats[1] = {m->emitterMaterial};
    for (std::size_t i = 0; i < spheres.size(); ++i) {
        InstanceDesc d{};
        d.mesh = m->emitterSphere;
        d.materials = mats;
        d.world = glm::scale(glm::translate(glm::mat4(1.0f), spheres[i].centre), glm::vec3(spheres[i].radius));
        d.prevWorld = d.world;   // the frame draws it where the step left it: no motion of its own
        if (i < m->presentedSlots.size()) {
            m->instances.update(m->presentedSlots[i], d, m->materials.count());
            m->stageTlasInstance(m->presentedSlots[i], false);
        } else {
            const uint32_t slot = m->instances.create(d, m->materials.count());
            m->stageTlasInstance(slot, true);
            m->presentedSlots.push_back(slot);
        }
    }
    while (m->presentedSlots.size() > spheres.size()) {
        m->instances.destroy(m->presentedSlots.back());
        m->stageTlasInstance(m->presentedSlots.back(), true);
        m->presentedSlots.pop_back();
    }
}

void RenderScene::setTransientLights(std::span<const LightDesc> lights) {
    if (!m->initialized || (m->transientLights.empty() && lights.empty())) return;
    m->transientLights.assign(lights.begin(), lights.end());
    m->setLightTable();
    m->syncEmitters();
}

bool RenderScene::hasDirectionalLight() const {
    if (!m->initialized) return false;
    const auto descs = m->lights.descs();
    return std::any_of(descs.begin(), descs.end(),
                       [](const LightDesc& l) { return l.kind == LightKind::Directional; });
}

void RenderScene::setLightEmitters(bool enabled) {
    if (!m->initialized || enabled == m->emittersEnabled) return;
    m->emittersEnabled = enabled;
    m->syncEmitters();
}

void RenderScene::setViewerPosition(const glm::vec3& viewer) {
    if (!m->initialized || (m->viewer && *m->viewer == viewer)) return;
    m->viewer = viewer;
    if (m->spotEmitters) m->syncEmitters();
}

std::span<const uint32_t> RenderScene::lightEmitterSlots() const { return m->emitterSlots; }

const InstanceGpu& RenderScene::instance(uint32_t slot) const { return m->instances.gpu(slot); }

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

    // A refit or rebuilt BLAS (recordSkinning, before this) moves its instances' bounds: the TLAS
    // refits even when no instance changed.
    const bool tlasChanged = !m->tlasDirty.empty() || m->skinnedBlasChanged;
    m->skinnedBlasChanged = false;
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

void RenderScene::setVfx(VfxFrame frame) { m->vfx = std::move(frame); }
const VfxFrame& RenderScene::vfx() const noexcept { return m->vfx; }

std::span<const RenderScene::Draw> RenderScene::draws() const {
    if (m->instances.consumeDrawsDirty() || m->drawsDirty) m->rebuildDraws();
    return m->draws;
}
std::span<const uint32_t> RenderScene::maskDraws() const {
    (void)draws();
    return m->maskDraws;
}
std::span<const uint32_t> RenderScene::transparentDraws() const {
    (void)draws();
    return m->transparentDraws;
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
