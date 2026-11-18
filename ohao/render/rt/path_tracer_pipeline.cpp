// The path tracer's ray tracing pipelines and their shader binding tables. Two programs share the
// layout, the miss and the hit group: the default one (raygen, then the radiance cache's update
// raygen) and caustics' (sub-project 24: raygenCaustics, then causticTrace), made the first time a
// frame needs it, so the default raygen carries none of caustics' code and compiles as before.

#include "path_tracer.hpp"
#include "pipeline_disk_cache.hpp"
#include "render/rt/rt_compute.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "core/log.hpp"

namespace ohao {

namespace {
static std::vector<char> readFile(const std::string& path) {
    // Try multiple search paths for shader SPVs
    std::vector<std::string> searchPaths = {
        path,
        "build/shaders/" + path.substr(path.find_last_of("/\\") + 1),
        "build/Release/bin/shaders/" + path.substr(path.find_last_of("/\\") + 1),
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = (size_t)file.tellg();
            std::vector<char> buffer(size);
            file.seekg(0);
            file.read(buffer.data(), size);
            return buffer;
        }
    }
    return {};
}

// vkCreateRayTracingPipelinesKHR through a deferred operation (VK_KHR_deferred_host_operations), joined by this
// thread and up to `workers` more: the driver compiles a program's stages on the joined threads. Since every OpenPBR
// call site was made single (sub-project 25, W1c) a program's two raygens are about the same size, so they compile
// side by side; with one 1.1-million-instruction raygen, 1 and 5 threads measured the same (209 s, 212 s). Without
// the extension's entry points, an ordinary create.
VkResult createRtPipelineJoined(PFN_vkCreateRayTracingPipelinesKHR createPipelines, VkDevice device, VkPipelineCache cache,
                                const VkRayTracingPipelineCreateInfoKHR& info, VkPipeline& pipeline, uint32_t workers,
                                uint32_t& threadsUsed) {
    const auto create = reinterpret_cast<PFN_vkCreateDeferredOperationKHR>(
        vkGetDeviceProcAddr(device, "vkCreateDeferredOperationKHR"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyDeferredOperationKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyDeferredOperationKHR"));
    const auto concurrency = reinterpret_cast<PFN_vkGetDeferredOperationMaxConcurrencyKHR>(
        vkGetDeviceProcAddr(device, "vkGetDeferredOperationMaxConcurrencyKHR"));
    const auto join = reinterpret_cast<PFN_vkDeferredOperationJoinKHR>(
        vkGetDeviceProcAddr(device, "vkDeferredOperationJoinKHR"));
    const auto result = reinterpret_cast<PFN_vkGetDeferredOperationResultKHR>(
        vkGetDeviceProcAddr(device, "vkGetDeferredOperationResultKHR"));
    threadsUsed = 1;
    VkDeferredOperationKHR op = VK_NULL_HANDLE;
    if (!create || !destroy || !concurrency || !join || !result || create(device, nullptr, &op) != VK_SUCCESS)
        return createPipelines(device, VK_NULL_HANDLE, cache, 1, &info, nullptr, &pipeline);
    VkResult r = createPipelines(device, op, cache, 1, &info, nullptr, &pipeline);
    if (r == VK_OPERATION_DEFERRED_KHR) {
        // Each joined thread works until the driver has nothing left for it; VK_THREAD_IDLE_KHR asks it to try again.
        const auto work = [&] {
            for (;;) {
                const VkResult j = join(device, op);
                if (j != VK_THREAD_IDLE_KHR) return;
                std::this_thread::yield();
            }
        };
        const uint32_t useful = concurrency(device, op);
        const uint32_t extra = std::min(workers, useful > 1u ? useful - 1u : 0u);
        std::vector<std::thread> pool;
        for (uint32_t i = 0; i < extra; ++i) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        threadsUsed = 1u + extra;
        r = result(device, op);
    } else if (r == VK_OPERATION_NOT_DEFERRED_KHR) {
        r = VK_SUCCESS;
    }
    destroy(device, op, nullptr);
    return r;
}
}  // namespace

bool PathTracer::createRTPipeline() {
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = kPushStages;
    pushRange.offset = 0;
    pushRange.size = sizeof(PTPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to create pipeline layout");
        return false;
    }
    const Raygen cacheUpdate[] = {{m_shaderSet.cacheUpdateSpv, m_shaderSet.cacheUpdateEntry}};
    m_rtPipeline = buildRtPipeline(m_shaderSet.raygenSpv, m_shaderSet.raygenEntry, cacheUpdate, "path_tracer");
    if (!m_rtPipeline) return false;
    OHAO_LOG(Debug, render, "PathTracer: RT pipeline created");
    return true;
}

VkPipeline PathTracer::buildRtPipeline(const char* rgenSpv, const char* rgenEntry, std::span<const Raygen> extras,
                                       const char* cacheName) {
    auto rgenCode = readFile(rgenSpv);
    auto rmissCode = readFile(m_shaderSet.missSpv);
    auto rchitCode = readFile(m_shaderSet.closestHitSpv);
    auto rahitCode = readFile(m_shaderSet.anyHitSpv);
    std::vector<std::vector<char>> extraCode;
    for (const Raygen& r : extras) extraCode.push_back(readFile(r.spv));

    const bool extraMissing = std::any_of(extraCode.begin(), extraCode.end(), [](const auto& c) { return c.empty(); });
    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty() || extraMissing) {
        OHAO_LOG(Error, render, "PathTracer: failed to load RT shader SPVs ({})", cacheName);
        return VK_NULL_HANDLE;
    }
    bool hasAnyHit = !rahitCode.empty();

    auto createModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vkCreateShaderModule(m_device, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule rgenModule = createModule(rgenCode);
    VkShaderModule rmissModule = createModule(rmissCode);
    VkShaderModule rchitModule = createModule(rchitCode);
    VkShaderModule rahitModule = hasAnyHit ? createModule(rahitCode) : VK_NULL_HANDLE;
    std::vector<VkShaderModule> extraModules;
    for (const auto& c : extraCode) extraModules.push_back(createModule(c));

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit, 3=any-hit (optional), then the extra raygens.
    const uint32_t extraStage = hasAnyHit ? 4 : 3;
    const uint32_t stageCount = extraStage + uint32_t(extras.size());
    std::vector<VkPipelineShaderStageCreateInfo> stages(stageCount);

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = rgenEntry;

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = m_shaderSet.missEntry;

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = m_shaderSet.closestHitEntry;

    if (hasAnyHit) {
        stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        stages[3].module = rahitModule;
        stages[3].pName = m_shaderSet.anyHitEntry;
    }
    for (uint32_t i = 0; i < extras.size(); ++i) {
        stages[extraStage + i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[extraStage + i].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stages[extraStage + i].module = extraModules[i];
        stages[extraStage + i].pName = extras[i].entry;
    }

    // Shader groups: raygen, miss, the hit group (closest-hit + any-hit), the extra raygens
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups(3 + extras.size());

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: Hit group — closest-hit + any-hit
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2;
    groups[2].anyHitShader = hasAnyHit ? 3 : VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    for (uint32_t i = 0; i < extras.size(); ++i) {
        VkRayTracingShaderGroupCreateInfoKHR& g = groups[3 + i];
        g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        g.generalShader = extraStage + i;
        g.closestHitShader = VK_SHADER_UNUSED_KHR;
        g.anyHitShader = VK_SHADER_UNUSED_KHR;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
    }

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages.data();
    pipelineInfo.groupCount = uint32_t(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = 2;  // path tracing needs bounce recursion
    pipelineInfo.layout = m_pipelineLayout;
    if (rtc::pipelineStats()) pipelineInfo.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

    // Minutes cold: kept in a file so the next process (a headless host beside a window) loads it.
    PipelineDiskCache cache;
    // Keyed by every module (a program without an extra raygen keyed with an empty one, as it always was).
    std::vector<std::vector<char>> keyed{rgenCode, rmissCode, rchitCode, rahitCode};
    for (const auto& c : extraCode) keyed.push_back(c);
    if (extraCode.empty()) keyed.emplace_back();
    cache.open(m_device, m_physicalDevice, cacheName, pipelineContentKey(keyed));
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Timed: seconds cold (sub-project 25, W1c-W1d), on 4 threads (a program has at most 5 stages; cold, the default
    // program took 13.7 s on 1 thread and 8.5 s on 4, caustics' 18.3 s and 10.0 s).
    const auto compileStart = std::chrono::steady_clock::now();
    uint32_t threads = 1;
    // OHAO_RT_COMPILE_THREADS (1-8, default 4): the threads joining the compile.
    uint32_t joined = 4;
    if (const char* e = std::getenv("OHAO_RT_COMPILE_THREADS")) joined = std::clamp<uint32_t>(std::atoi(e), 1u, 8u);
    VkResult result = createRtPipelineJoined(vkCreateRayTracingPipelinesKHR, m_device, cache.handle(), pipelineInfo,
                                             pipeline, joined - 1u, threads);
    OHAO_LOG(Info, render, "PathTracer: {} pipeline in {:.1f} s ({} threads)", cacheName,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count(), threads);
    if (result == VK_SUCCESS) cache.save();

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);
    if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
    for (VkShaderModule m : extraModules) vkDestroyShaderModule(m_device, m, nullptr);

    if (result != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to create RT pipeline {} (err={})", cacheName, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    if (rtc::pipelineStats()) {
        // The driver may report no executables for a ray tracing pipeline (NVIDIA's does not); the
        // raygen group's stack is what raygen's live state spills across each trace.
        rtc::logPipelineStats(m_device, pipeline, cacheName);
        const auto stackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupStackSizeKHR"));
        const VkDeviceSize raygenStack =
            stackSize ? stackSize(m_device, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        const VkDeviceSize extraStack =
            stackSize && !extras.empty() ? stackSize(m_device, pipeline, 3, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        OHAO_LOG(Info, render, "pipeline stats {}: raygen stack {} bytes, second raygen {} bytes", cacheName,
                 raygenStack, extraStack);
    }
    return pipeline;
}

bool PathTracer::ensureCausticsPipeline() {
    if (m_causticsPipeline) return true;
    if (m_causticsPipelineFailed) return false;
    const Raygen causticTrace[] = {{m_shaderSet.causticTraceSpv, m_shaderSet.causticTraceEntry}};
    m_causticsPipeline = buildRtPipeline(m_shaderSet.raygenCausticsSpv, m_shaderSet.raygenCausticsEntry, causticTrace,
                                         "path_tracer_caustics");
    if (!m_causticsPipeline ||
        !buildShaderBindingTable(m_causticsPipeline, m_causticsSbt, m_causticsRgenRegion, m_causticsMissRegion,
                                 m_causticsHitRegion, std::array{&m_causticTraceRegion})) {
        OHAO_LOG(Error, render, "PathTracer: caustics' pipeline unavailable: caustics stay off");
        m_causticsPipelineFailed = true;
        return false;
    }
    return true;
}

// Caustics step 2's program: manifoldSetup, then a raygen a round (a program of its own, so neither caustics' program
// nor the default one carries their code), and manifoldResolve as compute on the tracer's layout (the same set and push
// constants), made the first time a frame runs the manifold pass. The walk and the solve are raygens, not compute: from
// the same SPIR-V NVIDIA's driver (620.70) lowers a vector's dot with itself (OpDot, inside length and normalize) as
// fma(z, z, fma(y, y, x x)) in a raygen and fma(z, z, fma(x, x, y y)) in compute (each on all of 55,296 inputs,
// 2026-11-17), and Newton's test against a tolerance of 1e-6 is at fp32's noise there: as compute it lost 2.1% of the
// roots a double-precision Newton finds (caustics, 160x90), as a raygen 0.25%, step 2a's own count.
bool PathTracer::ensureManifoldPipeline() {
    if (m_manifoldPipelineFailed) return false;
    if (m_manifoldPipeline) return true;
    static constexpr const char* kRounds[6] = {"manifoldWalkRecords", "manifoldWalkGroups", "manifoldWalkClusters",
                                               "manifoldWalkSubs",    "manifoldSolvePlain", "manifoldConnect"};
    std::array<std::string, 6> spv;
    std::array<Raygen, 6> rounds;
    std::array<VkStridedDeviceAddressRegionKHR*, 6> regions;
    for (uint32_t i = 0; i < 6; ++i) {
        spv[i] = std::string("bin/shaders/rt_path_tracer.slang.") + kRounds[i] + ".spv";
        rounds[i] = {spv[i].c_str(), kRounds[i]};
        regions[i] = &m_manifoldRoundRegions[i];
    }
    m_manifoldPipeline = buildRtPipeline(m_shaderSet.manifoldSpv, m_shaderSet.manifoldEntry, rounds,
                                         "path_tracer_manifold");
    bool ok = m_manifoldPipeline && buildShaderBindingTable(m_manifoldPipeline, m_manifoldSbt, m_manifoldRgenRegion,
                                                            m_manifoldMissRegion, m_manifoldHitRegion, regions);
    // The resolve, as compute on the tracer's layout.
    const auto compute = [&](const char* entry, VkPipeline& pipe) {
        const std::string file = std::string("rt_path_tracer.slang.") + entry + ".spv";
        const VkShaderModule module = rtc::loadShaderModule(m_device, file.c_str());
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = entry;
        ci.layout = m_pipelineLayout;
        const bool made = module != VK_NULL_HANDLE &&
                          vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
        if (module) vkDestroyShaderModule(m_device, module, nullptr);
        return made;
    };
    ok = ok && compute(m_shaderSet.manifoldResolveEntry, m_manifoldResolvePipe);
    if (!ok) {
        OHAO_LOG(Error, render, "PathTracer: the manifold pass's pipelines unavailable: caustics' step 2 stays off");
        m_manifoldPipelineFailed = true;
        return false;
    }
    return true;
}

// Path guiding's programs: the default raygen or caustics' with guiding compiled in (tracePixel's GUIDING), made the
// first time a frame guides, so neither default raygen carries guiding's code (it cost 18 ms of the helmet's realtime
// trace while off). The caustics one carries causticTrace as caustics' program does.
bool PathTracer::ensureGuidedProgram(bool caustics) {
    GuidedProgram& g = m_guided[caustics ? 1 : 0];
    if (g.pipeline) return true;
    if (g.failed) return false;
    const Raygen causticTrace[] = {{m_shaderSet.causticTraceSpv, m_shaderSet.causticTraceEntry}};
    g.pipeline = caustics ? buildRtPipeline(m_shaderSet.raygenCausticsGuidedSpv, m_shaderSet.raygenCausticsGuidedEntry,
                                            causticTrace, "path_tracer_caustics_guided")
                          : buildRtPipeline(m_shaderSet.raygenGuidedSpv, m_shaderSet.raygenGuidedEntry, {},
                                            "path_tracer_guided");
    VkStridedDeviceAddressRegionKHR* const extra[] = {&g.extra};
    if (!g.pipeline || !buildShaderBindingTable(g.pipeline, g.sbt, g.rgen, g.miss, g.hit,
                                                std::span(extra, caustics ? 1u : 0u))) {
        OHAO_LOG(Error, render, "PathTracer: path guiding's {} pipeline unavailable: guiding stays off",
                 caustics ? "caustics" : "default");
        g.failed = true;
        return false;
    }
    return true;
}

// ─── Shader Binding Table ────────────────────────────────────────────

bool PathTracer::createShaderBindingTable() {
    return buildShaderBindingTable(m_rtPipeline, m_sbt, m_rgenRegion, m_missRegion, m_hitRegion,
                                   std::array{&m_cacheRgenRegion});
}

bool PathTracer::buildShaderBindingTable(VkPipeline pipeline, GpuBuffer& sbt,
                                         VkStridedDeviceAddressRegionKHR& rgenRegion,
                                         VkStridedDeviceAddressRegionKHR& missRegion,
                                         VkStridedDeviceAddressRegionKHR& hitRegion,
                                         std::span<VkStridedDeviceAddressRegionKHR* const> extraRegions) {
    // Query RT pipeline properties for handle size/alignment
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    uint32_t handleSize = rtProps.shaderGroupHandleSize;
    uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;

    // Aligned handle size (each entry in the SBT must be aligned)
    uint32_t handleSizeAligned = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);

    // The groups: rgen, miss, closest-hit, then the extra raygens.
    const uint32_t extras = uint32_t(extraRegions.size());
    const uint32_t groupCount = 3 + extras;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, pipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to get shader group handles");
        return false;
    }

    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize + extras * rgenSize;   // the extra raygens last

    // The table: host-written once, read by every trace through its device address.
    sbt = m_allocator->createBuffer(totalSize,
                                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    AllocationUsage::CpuToGpu, true);
    if (!sbt.isValid() || !sbt.getMappedData()) return false;

    uint8_t* dst = static_cast<uint8_t*>(sbt.getMappedData());
    memset(dst, 0, totalSize);
    // Rgen at offset 0
    memcpy(dst, handles.data() + 0 * handleSize, handleSize);
    // Miss at offset rgenSize
    memcpy(dst + rgenSize, handles.data() + 1 * handleSize, handleSize);
    // Closest-hit at offset rgenSize + missSize
    memcpy(dst + rgenSize + missSize, handles.data() + 2 * handleSize, handleSize);
    // The extra raygens after the hit group, a region each
    for (uint32_t i = 0; i < extras; ++i)
        memcpy(dst + rgenSize + missSize + hitSize + i * rgenSize, handles.data() + (3 + i) * handleSize, handleSize);
    m_allocator->flushBuffer(sbt);

    // Set up strided device address regions
    VkDeviceAddress sbtAddr = getBufferDeviceAddress(sbt.buffer);

    rgenRegion.deviceAddress = sbtAddr;
    // A raygen region holds exactly one record, and the spec requires its size
    // to equal its stride (VUID-vkCmdTraceRaysKHR-size-04023).
    rgenRegion.stride = rgenSize;
    rgenRegion.size = rgenSize;

    missRegion.deviceAddress = sbtAddr + rgenSize;
    missRegion.stride = handleSizeAligned;
    missRegion.size = missSize;

    hitRegion.deviceAddress = sbtAddr + rgenSize + missSize;
    hitRegion.stride = handleSizeAligned;
    hitRegion.size = hitSize;

    for (uint32_t i = 0; i < extras; ++i) {
        extraRegions[i]->deviceAddress = sbtAddr + rgenSize + missSize + hitSize + i * rgenSize;
        extraRegions[i]->stride = rgenSize;
        extraRegions[i]->size = rgenSize;
    }

    m_callRegion = {};  // not used

    OHAO_LOG(Debug, render, "PathTracer: SBT created (handleSize={}, aligned={})",
             handleSize, handleSizeAligned);
    return true;
}

} // namespace ohao
