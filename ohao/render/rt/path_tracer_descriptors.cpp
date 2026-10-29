// Extracted from path_tracer.cpp — descriptor set layout + pool + set creation.
// Kept as a member of class PathTracer; no behavior change.

#include "path_tracer.hpp"

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
        constexpr uint32_t kOtherResources = 64;   // the set's 36 other bindings, with margin
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

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> bindingFlags;
    const auto add = [&](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages,
                         uint32_t count = 1, VkDescriptorBindingFlags flags = 0) {
        bindings.push_back(VkDescriptorSetLayoutBinding{binding, type, count, stages, nullptr});
        bindingFlags.push_back(flags);
    };
    constexpr VkDescriptorType IMAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType BUFFER = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    add(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, RGEN);  // TLAS (RenderScene)
    add(1, IMAGE, RGEN);                                           // accumulation, this frame's (RGBA32F)
    add(2, IMAGE, RGEN);                                           // output (RGBA8)
    add(3, IMAGE, RGEN);                                           // accumulation, the previous frame's (read)
    // RenderScene scene data, read by the hit shaders (pt_closesthit / pt_anyhit):
    add(4, BUFFER, HIT);    // vertex arena (Vertex = 25 floats)
    add(5, BUFFER, HIT);    // index arena (mesh-local indices)
    add(6, IMAGE, RGEN);    // albedo AOV (RGBA32F)
    add(7, IMAGE, RGEN);    // normal AOV (RGBA32F)
    add(8, BUFFER, HIT);    // mesh table (MeshGpu)
    add(9, BUFFER, HIT);    // triangle material slots
    add(10, BUFFER, HIT);   // material table (3 vec4s per material)
    add(11, BUFFER, RGEN | MISS);   // RT lights: header {count, envTexIdx, envIntensity} + GPULight[]
    // Bindless textures (closest-hit, any-hit, miss for the env map), the table's whole
    // capacity. Not a variable count: that flag is only legal on a layout's highest binding
    // (VUID 03004), and every set is allocated at the full capacity anyway.
    add(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RGEN | HIT | MISS, m_bindlessCapacity,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    add(13, IMAGE, RGEN);   // previous-frame surface history
    add(14, IMAGE, RGEN);   // current-frame surface history
    add(15, IMAGE, RGEN);   // previous-frame shading history
    add(16, IMAGE, RGEN);   // current-frame shading history
    add(17, BUFFER, RGEN | MISS);   // env marginal CDF
    add(18, BUFFER, RGEN | MISS);   // env conditional CDF
    add(19, IMAGE, RGEN);   // motion vector AOV (RG16F) — Sub-plan 3.A
    add(20, IMAGE, RGEN);   // depth AOV (R32F) — Sub-plan 3.B
    add(21, IMAGE, RGEN);   // roughness AOV — Sub-plan 3.B
    add(22, IMAGE, RGEN);   // diffuse radiance (RGBA32F) — Sub-plan 3.C
    add(23, IMAGE, RGEN);   // specular radiance (RGBA32F) — Sub-plan 3.C
    add(24, IMAGE, RGEN);   // diffuse albedo AOV (RGBA8) — Sub-plan 3.C.6
    add(25, IMAGE, RGEN);   // specular color AOV (RGBA8) — Sub-plan 3.C.6
    add(26, IMAGE, RGEN);   // normal+roughness packed AOV — Sub-plan 4.B
    add(27, IMAGE, RGEN);   // NRD denoised diffuse — Sub-plan 4.C (raygen flag for layout parity)
    add(28, IMAGE, RGEN);   // NRD denoised specular — Sub-plan 4.C
    // 29-31: ReSTIR GI prev reservoir planes 0-2 (read); 32-34: curr planes 0-2 (write)
    for (uint32_t b = 29; b <= 34; ++b) add(b, IMAGE, RGEN);
    add(35, IMAGE, RGEN);   // DLSS-RR specular hit-distance guide (R32F)
    add(36, BUFFER, HIT);   // instances (InstanceGpu, indexed by the TLAS custom index = slot)
    add(37, BUFFER, HIT);   // instance material refs
    add(38, BUFFER, RGEN);  // OpenPBR's energy tables (rt/path_tracer.slang)

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
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 26 * kSets},  // 1-3,6,7,13-16,19-35
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * kSets}, // scene data 4,5,8,9,10,36,37 + lights 11 + env CDFs 17,18 + OpenPBR tables 38
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_bindlessCapacity * kSets},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = kSets;
    poolInfo.poolSizeCount = 4;
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
