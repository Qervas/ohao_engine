// Extracted from path_tracer.cpp — descriptor set layout + pool + set creation.
// Kept as a member of class PathTracer; no behavior change.

#include "path_tracer.hpp"
#include "render/shader_bindings/rt_path_tracer.hpp"

#include <vector>
#include "core/log.hpp"

namespace ohao {

bool PathTracer::createDescriptorResources() {
    // Binding 12 holds the whole texture table (bindless index = slot). Clamp to what
    // the device allows for an update-after-bind combined-image-sampler array in one
    // set and one stage, leaving room for this set's other resources.
    {
        VkPhysicalDeviceDescriptorIndexingProperties indexing{};
        indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &indexing;
        vkGetPhysicalDeviceProperties2(m_physicalDevice, &props);
        constexpr uint32_t kOtherResources = 64;   // the set's 29 other bindings, with margin
        const uint32_t perStageResources = indexing.maxPerStageUpdateAfterBindResources > kOtherResources
                                               ? indexing.maxPerStageUpdateAfterBindResources - kOtherResources
                                               : 1u;
        const uint32_t limit = std::min({indexing.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         indexing.maxPerStageDescriptorUpdateAfterBindSamplers,
                                         indexing.maxDescriptorSetUpdateAfterBindSampledImages,
                                         indexing.maxDescriptorSetUpdateAfterBindSamplers, perStageResources});
        if (limit > 0 && m_bindlessCapacity > limit) {
            OHAO_LOG(Warn, render, "PathTracer: bindless texture capacity {}"
                                   " exceeds the device's update-after-bind limit; clamped to {}",
                     m_bindlessCapacity, limit);
            m_bindlessCapacity = limit;
        }
    }

    constexpr VkShaderStageFlags RGEN = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    constexpr VkShaderStageFlags HIT = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    constexpr VkShaderStageFlags MISS = VK_SHADER_STAGE_MISS_BIT_KHR;

    namespace P = shader::rt_path_tracer;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> bindingFlags;
    const auto add = [&](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages,
                         uint32_t count = 1, VkDescriptorBindingFlags flags = 0) {
        // Every binding is also the compute kernels' (sub-project 25: the path as compute with ray queries).
        bindings.push_back(VkDescriptorSetLayoutBinding{binding, type, count, stages | VK_SHADER_STAGE_COMPUTE_BIT,
                                                        nullptr});
        bindingFlags.push_back(flags);
    };
    constexpr VkDescriptorType IMAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType BUFFER = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    add(P::scene.binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, RGEN);  // TLAS (RenderScene)
    add(P::accumBuffer.binding, IMAGE, RGEN);                                           // accumulation, this frame's (RGBA32F)
    add(P::outputImage.binding, IMAGE, RGEN);                                           // output (RGBA8)
    add(P::accumHistory.binding, IMAGE, RGEN);                                           // accumulation, the previous frame's (read)
    // RenderScene scene data, read by the hit shaders (closestHit / anyHit) and by manifoldConnect's glass walk
    // (caustics step 2b: the solid's triangles):
    add(P::vertices.binding, BUFFER, RGEN | HIT);    // vertex arena (Vertex = 25 floats)
    add(P::indices.binding, BUFFER, RGEN | HIT);    // index arena (mesh-local indices)
    add(P::albedoAOV.binding, IMAGE, RGEN);    // albedo AOV (RGBA32F)
    add(P::normalAOV.binding, IMAGE, RGEN);    // normal AOV (RGBA32F)
    add(P::meshes.binding, BUFFER, RGEN | HIT);    // mesh table (MeshGpu)
    add(P::triangleSlots.binding, BUFFER, HIT);    // triangle material slots
    add(P::materials.binding, BUFFER, RGEN | HIT);   // material table (kMaterialRows float4s a material; the raygen reads emissive lights')
    add(P::lightData.binding, BUFFER, RGEN | MISS);   // RT lights: header {count, envTexIdx, envIntensity} + GPULight[]
    // Bindless textures (closest-hit, any-hit, miss for the env map), the table's whole
    // capacity. Not a variable count: that flag is only legal on a layout's highest binding
    // (VUID 03004), and every set is allocated at the full capacity anyway.
    add(P::textures.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RGEN | HIT | MISS, m_bindlessCapacity,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    add(P::emissionAOV.binding, IMAGE, RGEN);   // the camera ray's emission (RGBA16F): nrd_compose adds it back
    add(P::envCdf.binding, BUFFER, RGEN);  // the environment's 2D CDF (render/rt/env_cdf.hpp)
    add(P::motionVector.binding, IMAGE, RGEN);   // motion vector AOV (RG16F) — Sub-plan 3.A
    add(P::depthAOV.binding, IMAGE, RGEN);   // depth AOV (R32F) — Sub-plan 3.B
    add(P::roughnessAOV.binding, IMAGE, RGEN);   // roughness AOV — Sub-plan 3.B
    add(P::diffuseRadiance.binding, IMAGE, RGEN);   // diffuse radiance (RGBA32F) — Sub-plan 3.C
    add(P::specularRadiance.binding, IMAGE, RGEN);   // specular radiance (RGBA32F) — Sub-plan 3.C
    add(P::diffuseAlbedoAOV.binding, IMAGE, RGEN);   // diffuse albedo AOV (RGBA8) — Sub-plan 3.C.6
    add(P::specColorAOV.binding, IMAGE, RGEN);   // specular color AOV (RGBA8) — Sub-plan 3.C.6
    add(P::normalRoughnessAOV.binding, IMAGE, RGEN);   // normal+roughness packed AOV — Sub-plan 4.B
    add(P::instances.binding, BUFFER, RGEN | HIT);   // instances (InstanceGpu, indexed by the TLAS custom index = slot)
    add(P::materialRefs.binding, BUFFER, HIT);   // instance material refs
    add(P::openpbrTables.binding, BUFFER, RGEN | HIT);  // OpenPBR's energy tables (rt/path_tracer.slang; anyHit: a pane's transmission)
    add(P::cacheKeys.binding, BUFFER, RGEN);  // the radiance cache: keys,
    add(P::cacheAccum.binding, BUFFER, RGEN);  // this frame's sums,
    add(P::cacheResolved.binding, BUFFER, RGEN);  // resolved radiance (render/rt/radiance_cache.hpp)
    add(P::emissiveLights.binding, BUFFER, RGEN);  // the emissive triangle lights (render/rt/emissive_lights.hpp),
    add(P::emissivePower.binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RGEN);   // their power image (every level),
    add(P::emissiveRanges.binding, BUFFER, RGEN);  // their header and instance ranges,
    add(P::emissiveFirst.binding, BUFFER, HIT);   // each instance slot's first light (a bounce that meets one)
    add(P::guideStates.binding, BUFFER, RGEN);  // path guiding: the grid's states (render/rt/path_guiding.hpp),
    add(P::guideProposals.binding, BUFFER, RGEN);  // this frame's proposals,
    add(P::guidePixel.binding, BUFFER, RGEN);  // each pixel's sample radiance and proposal count
    add(P::causticCasters.binding, BUFFER, RGEN);  // caustics: the lights' disks (render/rt/caustic_pass.hpp),
    add(P::causticSplat.binding, BUFFER, RGEN);  // the light pass's splats,
    add(P::causticStats.binding, BUFFER, RGEN);  // its clamped count (and the manifold pass's counters)
    add(P::manifoldTris.binding, BUFFER, RGEN);  // caustics step 2: the owned casters' triangles (render/rt/manifold_casters.hpp),
    add(P::manifoldNodes.binding, BUFFER, RGEN);  // their clusters and groups,
    add(P::manifoldDraws.binding, BUFFER, RGEN);  // the owned draws and a word a material,
    add(P::manifoldRequests.binding, BUFFER, RGEN);  // the camera side's requests

    if (bindings.size() != P::kSet0.size()) {   // a binding the shader declares and this layout lacks, or the reverse
        OHAO_LOG(Error, render, "PathTracer: the layout has {} bindings, rt/path_tracer.slang's set 0 {}", bindings.size(),
                 P::kSet0.size());
        return false;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool: one set per frame in flight (each frame rewrites only its own set).
    constexpr uint32_t kSets = MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * kSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 14 * kSets},  // 1-3, 6, 7, 13, 19-26
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 26 * kSets}, // scene data 4,5,8,9,10,36,37 + lights 11 + the env CDF 17 + OpenPBR tables 38 + the cache 39-41 + emissive 42,44,45 + guiding 46,49,50 + caustics 51-53 + manifold 54-57
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 * kSets},   // the emissive lights' power 43
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_bindlessCapacity * kSets},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = kSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorSetLayout, kSets> layouts;
    layouts.fill(m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = m_descriptorPool;
    setInfo.descriptorSetCount = kSets;
    setInfo.pSetLayouts = layouts.data();

    return vkAllocateDescriptorSets(m_device, &setInfo, m_descriptorSets.data()) == VK_SUCCESS;
}

} // namespace ohao
