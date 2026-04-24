// Sub-plan 4.E T1: NrdTonemap — Vulkan compute pipeline that applies ACES +
// sRGB gamma to NRD's composed HDR output.
// Sub-plan 4.F T1: extended to 4-binding layout with push constants so the
// tonemap pass can composite an HDR environment background for miss-ray
// pixels (where depth AOV == sky sentinel 1e30).

#include "render/rt/denoise/nrd_tonemap.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

namespace ohao {

namespace {

// OHAO's shader CMake flattens directory paths with underscores.
std::vector<char> readShaderSpv(const std::string& name) {
    const std::string searchPaths[] = {
        name,
        "build/shaders/" + name,
        "build/Release/bin/shaders/" + name,
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = static_cast<size_t>(file.tellg());
            std::vector<char> buf(size);
            file.seekg(0);
            file.read(buf.data(), size);
            return buf;
        }
    }
    return {};
}

// Sub-plan 4.F T1: push-constant struct mirrors shaders/rt/nrd_tonemap.comp.
// Layout: mat4 invView (64) + mat4 invProj (64) + vec2 extent (8) +
// float envIntensity (4) + float _pad (4) = 144 bytes total. vec2 sits on a
// 16-byte boundary (offset 128) per std430; the 2 trailing floats pad it to
// 144. Verified against spirv-cross dump of rt_nrd_tonemap.comp.spv.
struct TonemapPushConstants {
    float invView[16];
    float invProj[16];
    float extent[2];
    float envIntensity;
    float _pad;
};
static_assert(sizeof(TonemapPushConstants) == 144, "Tonemap PC must be 144B");

}  // anonymous namespace

struct NrdTonemap::Impl {
    VkDevice              device         = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice = VK_NULL_HANDLE;
    uint32_t              width          = 0;
    uint32_t              height         = 0;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet  = VK_NULL_HANDLE;

    // Sub-plan 4.F T1: fallback 1x1 black env texture + sampler used when the
    // caller has no env map loaded. Descriptor must still be valid (Vulkan
    // disallows null combined-image-samplers), so we bind this stub and set
    // envIntensity=0 in the push constants. First dispatch that binds it
    // transitions it UNDEFINED → SHADER_READ_ONLY_OPTIMAL (no data upload —
    // envIntensity=0 means its contents never actually reach the final pixel).
    VkImage        fallbackEnvImage  = VK_NULL_HANDLE;
    VkDeviceMemory fallbackEnvMemory = VK_NULL_HANDLE;
    VkImageView    fallbackEnvView   = VK_NULL_HANDLE;
    VkSampler      fallbackEnvSampler = VK_NULL_HANDLE;
    bool           fallbackEnvTransitioned = false;
};

NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() { shutdown(); }

namespace {

// Sub-plan 4.F T1: build a 1x1 RGBA32F black texture + linear-clamp sampler.
// Bound as the env descriptor when the caller passes VK_NULL_HANDLE — keeps
// the descriptor set valid under all validation-layer configurations.
bool createFallbackEnv(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkImage& image, VkDeviceMemory& memory,
                       VkImageView& view, VkSampler& sampler) {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
    imgInfo.extent        = {1, 1, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imgInfo, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, image, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i; break;
        }
    }
    if (memTypeIndex == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = memTypeIndex;
    if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod       = 1.0f;
    if (vkCreateSampler(device, &si, nullptr, &sampler) != VK_SUCCESS) return false;

    return true;
}

}  // anonymous namespace

bool NrdTonemap::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                             uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // Sub-plan 4.F T1: 3 storage-image bindings (HDR in, LDR out, depth) +
    // 1 combined-image-sampler (env map). All at COMPUTE stage.
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorSetLayout failed: " << int(r) << std::endl;
        return false;
    }

    // Sub-plan 4.F T1: push-constant range for camera inv matrices + extent + envIntensity.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(TonemapPushConstants);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_impl->setLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (VkResult r = vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreatePipelineLayout failed: " << int(r) << std::endl;
        return false;
    }

    auto spv = readShaderSpv("rt_nrd_tonemap.comp.spv");
    if (spv.empty()) {
        std::cerr << "[NRD tonemap] failed to load rt_nrd_tonemap.comp.spv" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateShaderModule failed: " << int(r) << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName  = "main";

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage  = stage;
    pipeInfo.layout = m_impl->pipelineLayout;
    VkResult pipeResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                                    &m_impl->pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (pipeResult != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateComputePipelines failed: " << int(pipeResult) << std::endl;
        return false;
    }

    // Sub-plan 4.F T1: descriptor pool now holds 3 STORAGE_IMAGE + 1 COMBINED_IMAGE_SAMPLER.
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = 1;
    if (VkResult r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorPool failed: " << int(r) << std::endl;
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_impl->setLayout;
    if (VkResult r = vkAllocateDescriptorSets(device, &dsai, &m_impl->descriptorSet); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkAllocateDescriptorSets failed: " << int(r) << std::endl;
        return false;
    }

    // Fallback env (1x1 black, never sampled meaningfully — envIntensity=0).
    if (!createFallbackEnv(device, physicalDevice,
                            m_impl->fallbackEnvImage, m_impl->fallbackEnvMemory,
                            m_impl->fallbackEnvView, m_impl->fallbackEnvSampler)) {
        std::cerr << "[NRD tonemap] createFallbackEnv failed" << std::endl;
        return false;
    }

    std::cout << "[NRD tonemap] pipeline ready @ " << width << "x" << height
              << " (4.F T1: env composite enabled)" << std::endl;
    return true;
}

void NrdTonemap::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    if (m_impl->fallbackEnvSampler) { vkDestroySampler(d, m_impl->fallbackEnvSampler, nullptr); m_impl->fallbackEnvSampler = VK_NULL_HANDLE; }
    if (m_impl->fallbackEnvView)    { vkDestroyImageView(d, m_impl->fallbackEnvView, nullptr);   m_impl->fallbackEnvView    = VK_NULL_HANDLE; }
    if (m_impl->fallbackEnvImage)   { vkDestroyImage(d, m_impl->fallbackEnvImage, nullptr);      m_impl->fallbackEnvImage   = VK_NULL_HANDLE; }
    if (m_impl->fallbackEnvMemory)  { vkFreeMemory(d, m_impl->fallbackEnvMemory, nullptr);       m_impl->fallbackEnvMemory  = VK_NULL_HANDLE; }
    if (m_impl->descriptorPool)     { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)           { vkDestroyPipeline(d, m_impl->pipeline, nullptr);              m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout)     { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr);  m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)          { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr);  m_impl->setLayout      = VK_NULL_HANDLE; }
    m_impl->descriptorSet = VK_NULL_HANDLE;
    m_impl->device        = VK_NULL_HANDLE;
}

void NrdTonemap::dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs) {
    if (!m_impl->pipeline) return;

    // Storage-image writes (bindings 0/1/2).
    std::array<VkDescriptorImageInfo, 3> storageInfos{};
    storageInfos[0].imageView   = inputs.composedHDR;
    storageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    storageInfos[1].imageView   = inputs.tonemappedOut;
    storageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    storageInfos[2].imageView   = inputs.depthAOV;
    storageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Combined-image-sampler for env (binding 3). Fall back to stub view/sampler
    // when caller didn't supply one — envIntensity=0 ensures the sky path stays
    // black in that case (matches pre-4.F behavior for no-env scenes).
    const bool useFallback = (inputs.envMapView == VK_NULL_HANDLE || inputs.envMapSampler == VK_NULL_HANDLE);
    if (useFallback && !m_impl->fallbackEnvTransitioned) {
        // One-shot UNDEFINED → SHADER_READ_ONLY_OPTIMAL transition. Runs on
        // `cmd` so no separate submit needed. No data upload: shader masks
        // the sample to zero via envIntensity=0 anyway.
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask    = 0;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image            = m_impl->fallbackEnvImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
        m_impl->fallbackEnvTransitioned = true;
    }
    VkDescriptorImageInfo envInfo{};
    envInfo.imageView   = useFallback ? m_impl->fallbackEnvView    : inputs.envMapView;
    envInfo.sampler     = useFallback ? m_impl->fallbackEnvSampler : inputs.envMapSampler;
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_impl->descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &storageInfos[i];
    }
    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = m_impl->descriptorSet;
    writes[3].dstBinding      = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo      = &envInfo;
    vkUpdateDescriptorSets(m_impl->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipelineLayout, 0, 1,
                             &m_impl->descriptorSet, 0, nullptr);

    // Push constants — pack matrices + extent + envIntensity.
    TonemapPushConstants pc{};
    std::memcpy(pc.invView, inputs.invView.data(), sizeof(float) * 16);
    std::memcpy(pc.invProj, inputs.invProj.data(), sizeof(float) * 16);
    pc.extent[0]     = inputs.extent[0];
    pc.extent[1]     = inputs.extent[1];
    pc.envIntensity  = inputs.envIntensity;
    pc._pad          = 0.0f;
    vkCmdPushConstants(cmd, m_impl->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);

    const uint32_t gx = (m_impl->width  + 7u) / 8u;
    const uint32_t gy = (m_impl->height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED

namespace ohao {

struct NrdTonemap::Impl {};
NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() = default;

bool NrdTonemap::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdTonemap::shutdown() {}
void NrdTonemap::dispatch(VkCommandBuffer, const NrdTonemapInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
