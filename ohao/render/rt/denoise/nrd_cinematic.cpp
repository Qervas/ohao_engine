// Sub-plan 4.G: NrdCinematicPost — bloom + AgX + vignette + color grade.
//
// Replaces 4.F's NrdTonemap with a 3-pipeline chain:
//   1. bloom_extract  (1 dispatch, full→half-res RGBA16F)
//   2. bloom_blur     (2 dispatches: mip0→mip1, mip1→mip2; both downsample+blur)
//   3. composite      (1 dispatch, full-res RGBA8 output)
//
// All three pipelines share the same OHAO conventions: 8x8 workgroup, push
// constants for parameters, descriptor pools sized to total bindings.
//
// OHAO_NRD=OFF compiles to no-op stubs (same shape as nrd_tonemap.cpp).

#include "render/rt/denoise/nrd_cinematic.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

namespace ohao {

namespace {

// OHAO's shader CMake flattens directory paths with underscores. See
// shaders/CMakeLists.txt: `string(REPLACE "/" "_" SHADER_FLAT_NAME ...)`.
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

// Push-constant struct mirrors shaders/rt/cinematic_bloom_extract.comp.
// 8 + 8 + 4 + 4 + 4 + 4 = 32 bytes.
struct BloomExtractPC {
    float srcExtent[2];
    float dstExtent[2];
    float threshold;
    float knee;
    float _pad0;
    float _pad1;
};
static_assert(sizeof(BloomExtractPC) == 32, "BloomExtractPC must be 32B");

// Push-constant for cinematic_bloom_blur.comp.
// 8 + 8 = 16 bytes.
struct BloomBlurPC {
    float srcExtent[2];
    float dstExtent[2];
};
static_assert(sizeof(BloomBlurPC) == 16, "BloomBlurPC must be 16B");

// Push-constant for cinematic_composite.comp — matches §3.7 of the spec.
// 64 + 64 + 8 + 4*7 + 12 + 4 = 176 bytes. Under the 256-byte minimum guarantee.
struct CompositePC {
    float invView[16];
    float invProj[16];
    float extent[2];
    float envIntensity;
    float exposure;
    float bloomStrength;
    float vignetteStrength;
    float saturation;
    float contrast;
    float tint[3];
    float _pad;
};
static_assert(sizeof(CompositePC) == 176, "CompositePC must be 176B");

// Push-constant for cinematic_dof.comp — matches the GLSL DoFPushConstants
// struct. 4 floats × 4 bytes = 16 bytes (well under the 256-byte minimum).
struct DoFPC {
    float focusDistance;
    float aperture;
    float maxCoCPixels;
    float _pad;
};
static_assert(sizeof(DoFPC) == 16, "DoFPC must be 16B");

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
    return vkCreateSampler(device, &si, nullptr, &sampler) == VK_SUCCESS;
}

VkShaderModule loadShader(VkDevice device, const char* spvName) {
    auto spv = readShaderSpv(spvName);
    if (spv.empty()) {
        std::cerr << "[NRD cinematic] failed to load " << spvName << std::endl;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smInfo, nullptr, &sm) != VK_SUCCESS) {
        std::cerr << "[NRD cinematic] vkCreateShaderModule failed for " << spvName << std::endl;
        return VK_NULL_HANDLE;
    }
    return sm;
}

}  // anonymous namespace

struct NrdCinematicPost::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    uint32_t         bloomW0        = 0;  // mip 0 (half-res)
    uint32_t         bloomH0        = 0;
    uint32_t         bloomW1        = 0;  // mip 1 (quarter-res)
    uint32_t         bloomH1        = 0;
    uint32_t         bloomW2        = 0;  // mip 2 (eighth-res)
    uint32_t         bloomH2        = 0;

    // Bloom extract pipeline
    VkDescriptorSetLayout extractLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      extractPipeLayout= VK_NULL_HANDLE;
    VkPipeline            extractPipeline  = VK_NULL_HANDLE;
    VkDescriptorSet       extractSet       = VK_NULL_HANDLE;

    // Bloom blur pipeline (shared across mip levels)
    VkDescriptorSetLayout blurLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      blurPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            blurPipeline     = VK_NULL_HANDLE;
    // Two pre-allocated sets: slot 0 (mip0 → mip1), slot 1 (mip1 → mip2).
    VkDescriptorSet       blurSets[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Composite pipeline
    VkDescriptorSetLayout compositeLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      compositePipeLayout= VK_NULL_HANDLE;
    VkPipeline            compositePipeline  = VK_NULL_HANDLE;
    VkDescriptorSet       compositeSet       = VK_NULL_HANDLE;

    // 4.J: depth-of-field gather pipeline
    VkDescriptorSetLayout dofLayout          = VK_NULL_HANDLE;
    VkPipelineLayout      dofPipeLayout      = VK_NULL_HANDLE;
    VkPipeline            dofPipeline        = VK_NULL_HANDLE;
    VkDescriptorSet       dofSet             = VK_NULL_HANDLE;

    // Single descriptor pool shared by all 3 pipelines
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Fallback env (same purpose as NrdTonemap had: descriptors must be valid
    // even when no env is loaded — composite shader masks contribution via
    // envIntensity=0).
    VkImage        fallbackEnvImage  = VK_NULL_HANDLE;
    VkDeviceMemory fallbackEnvMemory = VK_NULL_HANDLE;
    VkImageView    fallbackEnvView   = VK_NULL_HANDLE;
    VkSampler      fallbackEnvSampler= VK_NULL_HANDLE;
    bool           fallbackEnvTransitioned = false;
};

NrdCinematicPost::NrdCinematicPost()  : m_impl(std::make_unique<Impl>()) {}
NrdCinematicPost::~NrdCinematicPost() { shutdown(); }

namespace {

bool createExtractPipeline(NrdCinematicPost::Impl& I) {
    // Bindings: 0 = HDR in (storage image r), 1 = bloom mip 0 out (storage image w)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(I.device, &li, nullptr, &I.extractLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(BloomExtractPC);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &I.extractLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(I.device, &pli, nullptr, &I.extractPipeLayout) != VK_SUCCESS) return false;

    VkShaderModule sm = loadShader(I.device, "rt_cinematic_bloom_extract.comp.spv");
    if (sm == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = sm;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = I.extractPipeLayout;
    VkResult r = vkCreateComputePipelines(I.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &I.extractPipeline);
    vkDestroyShaderModule(I.device, sm, nullptr);
    return r == VK_SUCCESS;
}

bool createBlurPipeline(NrdCinematicPost::Impl& I) {
    // Bindings: 0 = src mip (storage image r), 1 = dst mip (storage image w)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(I.device, &li, nullptr, &I.blurLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(BloomBlurPC);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &I.blurLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(I.device, &pli, nullptr, &I.blurPipeLayout) != VK_SUCCESS) return false;

    VkShaderModule sm = loadShader(I.device, "rt_cinematic_bloom_blur.comp.spv");
    if (sm == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = sm;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = I.blurPipeLayout;
    VkResult r = vkCreateComputePipelines(I.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &I.blurPipeline);
    vkDestroyShaderModule(I.device, sm, nullptr);
    return r == VK_SUCCESS;
}

bool createCompositePipeline(NrdCinematicPost::Impl& I) {
    // Bindings:
    //   0 = HDR in   (storage image r)
    //   1 = LDR out  (storage image w)
    //   2 = depth    (storage image r)
    //   3 = env      (combined image sampler)
    //   4 = bloom 0  (combined image sampler)
    //   5 = bloom 1  (combined image sampler)
    //   6 = bloom 2  (combined image sampler)
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(I.device, &li, nullptr, &I.compositeLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(CompositePC);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &I.compositeLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(I.device, &pli, nullptr, &I.compositePipeLayout) != VK_SUCCESS) return false;

    VkShaderModule sm = loadShader(I.device, "rt_cinematic_composite.comp.spv");
    if (sm == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = sm;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = I.compositePipeLayout;
    VkResult r = vkCreateComputePipelines(I.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &I.compositePipeline);
    vkDestroyShaderModule(I.device, sm, nullptr);
    return r == VK_SUCCESS;
}

bool createDoFPipeline(NrdCinematicPost::Impl& I) {
    // Bindings (matches shaders/rt/cinematic_dof.comp):
    //   0 = pre-DoF LDR  (storage image rgba8 read)
    //   1 = depth AOV    (storage image r32f  read)
    //   2 = final LDR    (storage image rgba8 write)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(I.device, &li, nullptr, &I.dofLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(DoFPC);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &I.dofLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(I.device, &pli, nullptr, &I.dofPipeLayout) != VK_SUCCESS) return false;

    VkShaderModule sm = loadShader(I.device, "rt_cinematic_dof.comp.spv");
    if (sm == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = sm;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = I.dofPipeLayout;
    VkResult r = vkCreateComputePipelines(I.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &I.dofPipeline);
    vkDestroyShaderModule(I.device, sm, nullptr);
    return r == VK_SUCCESS;
}

}  // anonymous namespace

bool NrdCinematicPost::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                   uint32_t width, uint32_t height) {
    auto& I = *m_impl;
    I.device         = device;
    I.physicalDevice = physicalDevice;
    I.width          = width;
    I.height         = height;
    I.bloomW0        = (width  + 1) / 2;
    I.bloomH0        = (height + 1) / 2;
    I.bloomW1        = (I.bloomW0 + 1) / 2;
    I.bloomH1        = (I.bloomH0 + 1) / 2;
    I.bloomW2        = (I.bloomW1 + 1) / 2;
    I.bloomH2        = (I.bloomH1 + 1) / 2;

    if (!createExtractPipeline(I)) {
        std::cerr << "[NRD cinematic] createExtractPipeline failed" << std::endl;
        return false;
    }
    if (!createBlurPipeline(I)) {
        std::cerr << "[NRD cinematic] createBlurPipeline failed" << std::endl;
        return false;
    }
    if (!createCompositePipeline(I)) {
        std::cerr << "[NRD cinematic] createCompositePipeline failed" << std::endl;
        return false;
    }
    if (!createDoFPipeline(I)) {
        std::cerr << "[NRD cinematic] createDoFPipeline failed" << std::endl;
        return false;
    }

    // Descriptor pool — sized for all sets:
    //   extract:    2 storage images, 1 set
    //   blur:       2 storage images per set × 2 sets = 4
    //   composite:  3 storage images + 4 combined image samplers, 1 set
    //   dof:        3 storage images, 1 set  (4.J)
    // total storage images = 2 + 4 + 3 + 3 = 12
    // total combined image samplers = 4
    // total sets = 1 + 2 + 1 + 1 = 5
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 12;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 4;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = 5;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &I.descriptorPool) != VK_SUCCESS) {
        std::cerr << "[NRD cinematic] vkCreateDescriptorPool failed" << std::endl;
        return false;
    }

    // Allocate the 5 sets.
    VkDescriptorSet sets[5] = {};
    VkDescriptorSetLayout layouts[5] = {
        I.extractLayout, I.blurLayout, I.blurLayout, I.compositeLayout, I.dofLayout
    };
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = I.descriptorPool;
    ai.descriptorSetCount = 5;
    ai.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS) {
        std::cerr << "[NRD cinematic] vkAllocateDescriptorSets failed" << std::endl;
        return false;
    }
    I.extractSet   = sets[0];
    I.blurSets[0]  = sets[1];
    I.blurSets[1]  = sets[2];
    I.compositeSet = sets[3];
    I.dofSet       = sets[4];

    // Fallback env (descriptor must be valid when no env loaded).
    if (!createFallbackEnv(device, physicalDevice,
                            I.fallbackEnvImage, I.fallbackEnvMemory,
                            I.fallbackEnvView, I.fallbackEnvSampler)) {
        std::cerr << "[NRD cinematic] createFallbackEnv failed" << std::endl;
        return false;
    }

    std::cout << "[NRD cinematic] pipeline ready @ " << width << "x" << height
              << "  bloom mips: " << I.bloomW0 << "x" << I.bloomH0
              << ", "             << I.bloomW1 << "x" << I.bloomH1
              << ", "             << I.bloomW2 << "x" << I.bloomH2
              << "  (4.G+4.J: bloom + grade + camera DoF)" << std::endl;
    return true;
}

void NrdCinematicPost::shutdown() {
    if (!m_impl || !m_impl->device) return;
    auto& I = *m_impl;
    VkDevice d = I.device;
    if (I.fallbackEnvSampler) { vkDestroySampler(d, I.fallbackEnvSampler, nullptr); I.fallbackEnvSampler = VK_NULL_HANDLE; }
    if (I.fallbackEnvView)    { vkDestroyImageView(d, I.fallbackEnvView, nullptr);   I.fallbackEnvView    = VK_NULL_HANDLE; }
    if (I.fallbackEnvImage)   { vkDestroyImage(d, I.fallbackEnvImage, nullptr);      I.fallbackEnvImage   = VK_NULL_HANDLE; }
    if (I.fallbackEnvMemory)  { vkFreeMemory(d, I.fallbackEnvMemory, nullptr);       I.fallbackEnvMemory  = VK_NULL_HANDLE; }
    if (I.descriptorPool)     { vkDestroyDescriptorPool(d, I.descriptorPool, nullptr); I.descriptorPool = VK_NULL_HANDLE; }
    if (I.dofPipeline)           { vkDestroyPipeline(d, I.dofPipeline, nullptr); I.dofPipeline = VK_NULL_HANDLE; }
    if (I.dofPipeLayout)         { vkDestroyPipelineLayout(d, I.dofPipeLayout, nullptr); I.dofPipeLayout = VK_NULL_HANDLE; }
    if (I.dofLayout)             { vkDestroyDescriptorSetLayout(d, I.dofLayout, nullptr); I.dofLayout = VK_NULL_HANDLE; }
    if (I.compositePipeline)     { vkDestroyPipeline(d, I.compositePipeline, nullptr); I.compositePipeline = VK_NULL_HANDLE; }
    if (I.compositePipeLayout)   { vkDestroyPipelineLayout(d, I.compositePipeLayout, nullptr); I.compositePipeLayout = VK_NULL_HANDLE; }
    if (I.compositeLayout)       { vkDestroyDescriptorSetLayout(d, I.compositeLayout, nullptr); I.compositeLayout = VK_NULL_HANDLE; }
    if (I.blurPipeline)          { vkDestroyPipeline(d, I.blurPipeline, nullptr); I.blurPipeline = VK_NULL_HANDLE; }
    if (I.blurPipeLayout)        { vkDestroyPipelineLayout(d, I.blurPipeLayout, nullptr); I.blurPipeLayout = VK_NULL_HANDLE; }
    if (I.blurLayout)            { vkDestroyDescriptorSetLayout(d, I.blurLayout, nullptr); I.blurLayout = VK_NULL_HANDLE; }
    if (I.extractPipeline)       { vkDestroyPipeline(d, I.extractPipeline, nullptr); I.extractPipeline = VK_NULL_HANDLE; }
    if (I.extractPipeLayout)     { vkDestroyPipelineLayout(d, I.extractPipeLayout, nullptr); I.extractPipeLayout = VK_NULL_HANDLE; }
    if (I.extractLayout)         { vkDestroyDescriptorSetLayout(d, I.extractLayout, nullptr); I.extractLayout = VK_NULL_HANDLE; }
    I.extractSet = VK_NULL_HANDLE;
    I.blurSets[0] = VK_NULL_HANDLE;
    I.blurSets[1] = VK_NULL_HANDLE;
    I.compositeSet = VK_NULL_HANDLE;
    I.dofSet = VK_NULL_HANDLE;
    I.device = VK_NULL_HANDLE;
}

void NrdCinematicPost::dispatchBloomExtract(VkCommandBuffer cmd,
                                             VkImageView composedHDR,
                                             VkImageView bloomMip0View,
                                             uint32_t srcW, uint32_t srcH,
                                             uint32_t dstW, uint32_t dstH) {
    auto& I = *m_impl;
    if (!I.extractPipeline) return;

    VkDescriptorImageInfo info0{};
    info0.imageView   = composedHDR;
    info0.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo info1{};
    info1.imageView   = bloomMip0View;
    info1.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = I.extractSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &info0;
    writes[1].pImageInfo = &info1;
    vkUpdateDescriptorSets(I.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.extractPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.extractPipeLayout, 0, 1,
                             &I.extractSet, 0, nullptr);

    BloomExtractPC pc{};
    pc.srcExtent[0] = static_cast<float>(srcW);
    pc.srcExtent[1] = static_cast<float>(srcH);
    pc.dstExtent[0] = static_cast<float>(dstW);
    pc.dstExtent[1] = static_cast<float>(dstH);
    pc.threshold    = 1.0f;
    pc.knee         = 0.5f;
    pc._pad0        = 0.0f;
    pc._pad1        = 0.0f;
    vkCmdPushConstants(cmd, I.extractPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 7u) / 8u;
    const uint32_t gy = (dstH + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

void NrdCinematicPost::dispatchBloomBlur(VkCommandBuffer cmd,
                                          uint32_t slot,
                                          VkImageView srcMipView, VkImageView dstMipView,
                                          uint32_t srcW, uint32_t srcH,
                                          uint32_t dstW, uint32_t dstH) {
    auto& I = *m_impl;
    if (!I.blurPipeline || slot >= 2) return;

    VkDescriptorImageInfo info0{};
    info0.imageView   = srcMipView;
    info0.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo info1{};
    info1.imageView   = dstMipView;
    info1.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = I.blurSets[slot];
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &info0;
    writes[1].pImageInfo = &info1;
    vkUpdateDescriptorSets(I.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.blurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.blurPipeLayout, 0, 1,
                             &I.blurSets[slot], 0, nullptr);

    BloomBlurPC pc{};
    pc.srcExtent[0] = static_cast<float>(srcW);
    pc.srcExtent[1] = static_cast<float>(srcH);
    pc.dstExtent[0] = static_cast<float>(dstW);
    pc.dstExtent[1] = static_cast<float>(dstH);
    vkCmdPushConstants(cmd, I.blurPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 7u) / 8u;
    const uint32_t gy = (dstH + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

void NrdCinematicPost::dispatchComposite(VkCommandBuffer cmd, const NrdCinematicInputs& inputs) {
    auto& I = *m_impl;
    if (!I.compositePipeline) return;

    // Storage images (0, 1, 2).
    VkDescriptorImageInfo siInfos[3] = {};
    siInfos[0].imageView   = inputs.composedHDR;
    siInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    siInfos[1].imageView   = inputs.tonemappedOut;
    siInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    siInfos[2].imageView   = inputs.depthAOV;
    siInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Env sampler (3) — fallback if no env loaded.
    const bool useFallback = (inputs.envMapView == VK_NULL_HANDLE || inputs.envMapSampler == VK_NULL_HANDLE);
    if (useFallback && !I.fallbackEnvTransitioned) {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask    = 0;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image            = I.fallbackEnvImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
        I.fallbackEnvTransitioned = true;
    }
    VkDescriptorImageInfo envInfo{};
    envInfo.imageView   = useFallback ? I.fallbackEnvView    : inputs.envMapView;
    envInfo.sampler     = useFallback ? I.fallbackEnvSampler : inputs.envMapSampler;
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Bloom mips (4, 5, 6) — sampled.
    VkDescriptorImageInfo bloomInfos[3] = {};
    bloomInfos[0].imageView   = inputs.bloomMip0View;
    bloomInfos[0].sampler     = inputs.bloomSampler;
    bloomInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bloomInfos[1].imageView   = inputs.bloomMip1View;
    bloomInfos[1].sampler     = inputs.bloomSampler;
    bloomInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bloomInfos[2].imageView   = inputs.bloomMip2View;
    bloomInfos[2].sampler     = inputs.bloomSampler;
    bloomInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = I.compositeSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &siInfos[i];
    }
    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = I.compositeSet;
    writes[3].dstBinding      = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo      = &envInfo;
    for (uint32_t i = 0; i < 3; ++i) {
        writes[4 + i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4 + i].dstSet          = I.compositeSet;
        writes[4 + i].dstBinding      = 4 + i;
        writes[4 + i].descriptorCount = 1;
        writes[4 + i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4 + i].pImageInfo      = &bloomInfos[i];
    }
    vkUpdateDescriptorSets(I.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.compositePipeLayout, 0, 1,
                             &I.compositeSet, 0, nullptr);

    CompositePC pc{};
    std::memcpy(pc.invView, inputs.invView.data(), sizeof(float) * 16);
    std::memcpy(pc.invProj, inputs.invProj.data(), sizeof(float) * 16);
    pc.extent[0]        = inputs.extent[0];
    pc.extent[1]        = inputs.extent[1];
    pc.envIntensity     = inputs.envIntensity;
    pc.exposure         = inputs.exposure;
    pc.bloomStrength    = inputs.bloomStrength;
    pc.vignetteStrength = inputs.vignetteStrength;
    pc.saturation       = inputs.saturation;
    pc.contrast         = inputs.contrast;
    pc.tint[0]          = inputs.tint[0];
    pc.tint[1]          = inputs.tint[1];
    pc.tint[2]          = inputs.tint[2];
    pc._pad             = 0.0f;
    vkCmdPushConstants(cmd, I.compositePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);

    const uint32_t gx = (I.width  + 7u) / 8u;
    const uint32_t gy = (I.height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

void NrdCinematicPost::dispatchDoF(VkCommandBuffer cmd, const NrdDoFInputs& inputs) {
    auto& I = *m_impl;
    if (!I.dofPipeline) return;

    // Three storage-image bindings, all GENERAL.
    VkDescriptorImageInfo infos[3] = {};
    infos[0].imageView   = inputs.preDofLdr;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[1].imageView   = inputs.depthAOV;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[2].imageView   = inputs.finalLdrOut;
    infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = I.dofSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(I.device, static_cast<uint32_t>(writes.size()),
                            writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.dofPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.dofPipeLayout,
                             0, 1, &I.dofSet, 0, nullptr);

    DoFPC pc{};
    pc.focusDistance = inputs.focusDistance;
    pc.aperture      = inputs.aperture;
    pc.maxCoCPixels  = inputs.maxCoCPixels;
    pc._pad          = 0.0f;
    vkCmdPushConstants(cmd, I.dofPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);

    const uint32_t gx = (I.width  + 7u) / 8u;
    const uint32_t gy = (I.height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED

namespace ohao {

struct NrdCinematicPost::Impl {};
NrdCinematicPost::NrdCinematicPost()  : m_impl(std::make_unique<Impl>()) {}
NrdCinematicPost::~NrdCinematicPost() = default;

bool NrdCinematicPost::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdCinematicPost::shutdown() {}
void NrdCinematicPost::dispatchBloomExtract(VkCommandBuffer, VkImageView, VkImageView,
                                             uint32_t, uint32_t, uint32_t, uint32_t) {}
void NrdCinematicPost::dispatchBloomBlur(VkCommandBuffer, uint32_t, VkImageView, VkImageView,
                                          uint32_t, uint32_t, uint32_t, uint32_t) {}
void NrdCinematicPost::dispatchComposite(VkCommandBuffer, const NrdCinematicInputs&) {}
void NrdCinematicPost::dispatchDoF(VkCommandBuffer, const NrdDoFInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
