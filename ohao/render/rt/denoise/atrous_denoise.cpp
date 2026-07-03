// AtrousDenoiser — full SVGF (Spatiotemporal Variance-Guided Filtering,
// Schied et al. 2017) for the RT beauty image (DenoiseMode::Atrous).
//
// Two compute passes + persistent history:
//   Pass 1 (rt_svgf_temporal.comp): reproject + temporally accumulate the
//           fresh per-frame beauty, estimate per-pixel luminance variance.
//   Pass 2 (rt_svgf_atrous.comp):   5-iteration variance-guided à-trous over
//           the accumulated color; final iteration writes the RGBA8 beauty.
//
// The first à-trous iteration's output is fed back as next frame's temporal
// history color (SVGF standard), so it doubles as the ping-pong buffer B for
// iteration 0. Because SVGF denoises the final (correct) LDR PBR image — never
// demodulates diffuse/specular like NRD — metals/emissive stay physically
// correct (no black metals, no magenta cast).

#include "render/rt/denoise/atrous_denoise.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace ohao {

namespace {

// Number of à-trous iterations. Step sizes double each pass: 1,2,4,8,16.
constexpr uint32_t kNumIterations = 5;

// Variance-guided edge-stopping sensitivities. Tuned for the LDR RGBA16F
// accumulated color ([0,1] luminance), world normals, and linear view-space
// depth (world units). sigmaL is deliberately small: the temporal pass already
// removes most noise, so the spatial filter only needs to clean the residual
// while aggressively preserving high-frequency detail — a larger sigmaL
// over-blurs (measured: sigmaL=4 -> |lap| 3.25 < plain a-trous; sigmaL=0.4 ->
// |lap| 5.70 > plain 4.38 AND OIDN 4.98, at the same ~0.6 flat-metal noise).
constexpr float kSigmaL      = 0.4f;   // luminance weight (scaled by sqrt(variance))
constexpr float kSigmaNormal = 0.30f;  // normal weight
constexpr float kSigmaDepth  = 2.0f;   // depth weight

struct SvgfTemporalPush {
    int32_t width;
    int32_t height;
    int32_t reset;   // 1 => discard history
    int32_t pad;
};

struct SvgfAtrousPush {
    int32_t stepSize;
    float   sigmaL;
    float   sigmaNormal;
    float   sigmaDepth;
    int32_t isFinal;   // 1 => write RGBA8 beauty
};

// Matches PathTracer / NrdCompositor readFile helper.
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

}  // anonymous namespace

struct AtrousDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;

    // Pass 1 (temporal accumulation)
    VkDescriptorSetLayout temporalSetLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      temporalPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            temporalPipeline       = VK_NULL_HANDLE;
    VkDescriptorSet       temporalSet            = VK_NULL_HANDLE;

    // Pass 2 (variance-guided à-trous)
    VkDescriptorSetLayout atrousSetLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      atrousPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            atrousPipeline       = VK_NULL_HANDLE;
    VkDescriptorSet       atrousSets[kNumIterations] = {};

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // --- Persistent history (survives across frames), ping-ponged prev/cur ---
    VkImage        histColorImg[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // RGBA16F denoised color
    VkDeviceMemory histColorMem[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    histColorView[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage        histMomentImg[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // RGBA16F (m1,m2,len,_)
    VkDeviceMemory histMomentMem[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    histMomentView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage        histGeomImg[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE}; // RGBA16F (viewZ,nx,ny,nz)
    VkDeviceMemory histGeomMem[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    histGeomView[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // --- Per-frame scratch (fully overwritten each frame) ---
    VkImage        colorImg[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // RGBA16F à-trous ping-pong
    VkDeviceMemory colorMem[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    colorView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage        varImg[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // R16F variance ping-pong
    VkDeviceMemory varMem[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    varView[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    bool     imagesInitialized = false;  // first-frame UNDEFINED->GENERAL latch (all owned images)
    uint32_t parity            = 0;      // history ping-pong: cur=parity, prev=1-parity

    // Edge-stopping sensitivities (env-overridable for tuning sweeps).
    float sigmaL      = kSigmaL;
    float sigmaNormal = kSigmaNormal;
    float sigmaDepth  = kSigmaDepth;

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool createImage(VkFormat format, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = format;
        imageInfo.extent        = {width, height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imageInfo, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, img, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, img, mem, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = img;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) return false;
        return true;
    }

    // Create a compute pipeline from an SPV filename.
    bool createComputePipeline(const std::string& spvName, VkPipelineLayout layout, VkPipeline& outPipe) {
        auto spv = readShaderSpv(spvName);
        if (spv.empty()) {
            std::cerr << "[svgf] failed to load " << spvName << "\n";
            return false;
        }
        VkShaderModuleCreateInfo smInfo{};
        smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = spv.size();
        smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            std::cerr << "[svgf] vkCreateShaderModule failed for " << spvName << "\n";
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
        pipeInfo.layout = layout;
        VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &outPipe);
        vkDestroyShaderModule(device, shaderModule, nullptr);
        if (r != VK_SUCCESS) {
            std::cerr << "[svgf] vkCreateComputePipelines failed for " << spvName << "\n";
            return false;
        }
        return true;
    }
};

AtrousDenoiser::AtrousDenoiser()  : m_impl(std::make_unique<Impl>()) {}
AtrousDenoiser::~AtrousDenoiser() { shutdown(); }

bool AtrousDenoiser::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                uint32_t width, uint32_t height) {
    Impl& I = *m_impl;
    I.device         = device;
    I.physicalDevice = physicalDevice;
    I.width          = width;
    I.height         = height;

    constexpr VkFormat kRGBA16F = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat kR16F    = VK_FORMAT_R16_SFLOAT;

    auto makeSetLayout = [&](uint32_t count, VkDescriptorSetLayout& out) -> bool {
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);
        for (uint32_t i = 0; i < count; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = count;
        li.pBindings    = bindings.data();
        return vkCreateDescriptorSetLayout(device, &li, nullptr, &out) == VK_SUCCESS;
    };

    // --- Pass 1 (temporal): 11 storage-image bindings ---
    if (!makeSetLayout(11, I.temporalSetLayout)) {
        std::cerr << "[svgf] temporal set layout failed\n"; return false;
    }
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.size       = sizeof(SvgfTemporalPush);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount         = 1;
        pl.pSetLayouts            = &I.temporalSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &pc;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &I.temporalPipelineLayout) != VK_SUCCESS) {
            std::cerr << "[svgf] temporal pipeline layout failed\n"; return false;
        }
    }
    if (!I.createComputePipeline("rt_rt_svgf_temporal.comp.spv", I.temporalPipelineLayout, I.temporalPipeline))
        return false;

    // --- Pass 2 (à-trous): 7 storage-image bindings ---
    if (!makeSetLayout(7, I.atrousSetLayout)) {
        std::cerr << "[svgf] atrous set layout failed\n"; return false;
    }
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.size       = sizeof(SvgfAtrousPush);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount         = 1;
        pl.pSetLayouts            = &I.atrousSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &pc;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &I.atrousPipelineLayout) != VK_SUCCESS) {
            std::cerr << "[svgf] atrous pipeline layout failed\n"; return false;
        }
    }
    if (!I.createComputePipeline("rt_rt_svgf_atrous.comp.spv", I.atrousPipelineLayout, I.atrousPipeline))
        return false;

    // --- Descriptor pool: 1 temporal set (11) + 5 atrous sets (7 each) ---
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 11 + kNumIterations * 7;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = 1 + kNumIterations;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &I.descriptorPool) != VK_SUCCESS) {
            std::cerr << "[svgf] descriptor pool failed\n"; return false;
        }
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = I.descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &I.temporalSetLayout;
        if (vkAllocateDescriptorSets(device, &ai, &I.temporalSet) != VK_SUCCESS) {
            std::cerr << "[svgf] temporal set alloc failed\n"; return false;
        }
        std::array<VkDescriptorSetLayout, kNumIterations> layouts;
        layouts.fill(I.atrousSetLayout);
        VkDescriptorSetAllocateInfo ai2{};
        ai2.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai2.descriptorPool     = I.descriptorPool;
        ai2.descriptorSetCount = kNumIterations;
        ai2.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(device, &ai2, I.atrousSets) != VK_SUCCESS) {
            std::cerr << "[svgf] atrous set alloc failed\n"; return false;
        }
    }

    // --- Images: persistent history (RGBA16F) + scratch (RGBA16F / R16F) ---
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        ok = ok && I.createImage(kRGBA16F, I.histColorImg[i],  I.histColorMem[i],  I.histColorView[i]);
        ok = ok && I.createImage(kRGBA16F, I.histMomentImg[i], I.histMomentMem[i], I.histMomentView[i]);
        ok = ok && I.createImage(kRGBA16F, I.histGeomImg[i],   I.histGeomMem[i],   I.histGeomView[i]);
        ok = ok && I.createImage(kRGBA16F, I.colorImg[i],      I.colorMem[i],      I.colorView[i]);
        ok = ok && I.createImage(kR16F,    I.varImg[i],        I.varMem[i],        I.varView[i]);
    }
    if (!ok) { std::cerr << "[svgf] image allocation failed\n"; return false; }

    I.imagesInitialized = false;
    I.parity            = 0;

    // Tuning overrides (optional): OHAO_SVGF_SIGMAL / _SIGMAN / _SIGMAD.
    if (const char* e = std::getenv("OHAO_SVGF_SIGMAL")) I.sigmaL      = static_cast<float>(std::atof(e));
    if (const char* e = std::getenv("OHAO_SVGF_SIGMAN")) I.sigmaNormal = static_cast<float>(std::atof(e));
    if (const char* e = std::getenv("OHAO_SVGF_SIGMAD")) I.sigmaDepth  = static_cast<float>(std::atof(e));

    std::cout << "[svgf] pipeline ready @ " << width << "x" << height
              << "  sigmaL=" << I.sigmaL << " sigmaN=" << I.sigmaNormal << " sigmaD=" << I.sigmaDepth
              << " (temporal + " << kNumIterations << "-tap variance-guided a-trous)\n";
    return true;
}

void AtrousDenoiser::shutdown() {
    if (!m_impl || !m_impl->device) return;
    Impl& I = *m_impl;
    VkDevice d = I.device;

    auto destroyImg = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        if (view) { vkDestroyImageView(d, view, nullptr); view = VK_NULL_HANDLE; }
        if (img)  { vkDestroyImage(d, img, nullptr);       img  = VK_NULL_HANDLE; }
        if (mem)  { vkFreeMemory(d, mem, nullptr);         mem  = VK_NULL_HANDLE; }
    };
    for (int i = 0; i < 2; ++i) {
        destroyImg(I.histColorImg[i],  I.histColorMem[i],  I.histColorView[i]);
        destroyImg(I.histMomentImg[i], I.histMomentMem[i], I.histMomentView[i]);
        destroyImg(I.histGeomImg[i],   I.histGeomMem[i],   I.histGeomView[i]);
        destroyImg(I.colorImg[i],      I.colorMem[i],      I.colorView[i]);
        destroyImg(I.varImg[i],        I.varMem[i],        I.varView[i]);
    }
    if (I.descriptorPool)         { vkDestroyDescriptorPool(d, I.descriptorPool, nullptr);         I.descriptorPool = VK_NULL_HANDLE; }
    if (I.temporalPipeline)       { vkDestroyPipeline(d, I.temporalPipeline, nullptr);             I.temporalPipeline = VK_NULL_HANDLE; }
    if (I.atrousPipeline)         { vkDestroyPipeline(d, I.atrousPipeline, nullptr);               I.atrousPipeline = VK_NULL_HANDLE; }
    if (I.temporalPipelineLayout) { vkDestroyPipelineLayout(d, I.temporalPipelineLayout, nullptr); I.temporalPipelineLayout = VK_NULL_HANDLE; }
    if (I.atrousPipelineLayout)   { vkDestroyPipelineLayout(d, I.atrousPipelineLayout, nullptr);   I.atrousPipelineLayout = VK_NULL_HANDLE; }
    if (I.temporalSetLayout)      { vkDestroyDescriptorSetLayout(d, I.temporalSetLayout, nullptr); I.temporalSetLayout = VK_NULL_HANDLE; }
    if (I.atrousSetLayout)        { vkDestroyDescriptorSetLayout(d, I.atrousSetLayout, nullptr);   I.atrousSetLayout = VK_NULL_HANDLE; }
    I.temporalSet = VK_NULL_HANDLE;
    for (auto& s : I.atrousSets) s = VK_NULL_HANDLE;
    I.device = VK_NULL_HANDLE;
}

namespace {
// Fill a STORAGE_IMAGE descriptor write.
inline VkWriteDescriptorSet imgWrite(VkDescriptorSet set, uint32_t binding,
                                     VkDescriptorImageInfo* info) {
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = set;
    w.dstBinding      = binding;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo      = info;
    return w;
}
// Full memory barrier COMPUTE->COMPUTE (publish writes to next reads/writes).
inline void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb, 0, nullptr, 0, nullptr);
}
}  // anonymous namespace

void AtrousDenoiser::dispatch(VkCommandBuffer cmd, const AtrousInputs& inputs) {
    if (!m_impl->temporalPipeline || !m_impl->atrousPipeline) return;
    Impl& I = *m_impl;

    const uint32_t cur  = I.parity;
    const uint32_t prev = 1u - I.parity;

    // --- First-frame layout latch: all owned images UNDEFINED->GENERAL. After
    //     this they remain GENERAL for the lifetime of the denoiser (history is
    //     read next frame across submit boundaries; scratch is fully rewritten
    //     each frame). ---
    if (!I.imagesInitialized) {
        VkImage all[] = {
            I.histColorImg[0],  I.histColorImg[1],
            I.histMomentImg[0], I.histMomentImg[1],
            I.histGeomImg[0],   I.histGeomImg[1],
            I.colorImg[0],      I.colorImg[1],
            I.varImg[0],        I.varImg[1],
        };
        constexpr uint32_t n = sizeof(all) / sizeof(all[0]);
        VkImageMemoryBarrier b[n] = {};
        for (uint32_t i = 0; i < n; ++i) {
            b[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].srcAccessMask    = 0;
            b[i].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b[i].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image            = all[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, n, b);
        I.imagesInitialized = true;
    }

    const bool reset = inputs.resetHistory;

    // ================= Pass 1: temporal accumulation =================
    {
        VkDescriptorImageInfo ii[11]{};
        VkImageView views[11] = {
            inputs.beautyView,        // 0 current beauty (RGBA8)
            inputs.motionView,        // 1 motion vectors (RG16F)
            inputs.depthView,         // 2 depth (R32F)
            inputs.normalView,        // 3 normal (RGBA32F)
            I.histColorView[prev],    // 4 prev color history
            I.histMomentView[prev],   // 5 prev moments
            I.histGeomView[prev],     // 6 prev geometry
            I.colorView[0],           // 7 out accumColor -> à-trous input (scratch A)
            I.histMomentView[cur],    // 8 out moments (next frame)
            I.varView[0],             // 9 out variance (scratch varA)
            I.histGeomView[cur],      // 10 out geometry (next frame)
        };
        VkWriteDescriptorSet w[11];
        for (uint32_t k = 0; k < 11; ++k) {
            ii[k].imageView   = views[k];
            ii[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[k] = imgWrite(I.temporalSet, k, &ii[k]);
        }
        vkUpdateDescriptorSets(I.device, 11, w, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.temporalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.temporalPipelineLayout,
                                0, 1, &I.temporalSet, 0, nullptr);
        SvgfTemporalPush tp{};
        tp.width  = static_cast<int32_t>(I.width);
        tp.height = static_cast<int32_t>(I.height);
        tp.reset  = reset ? 1 : 0;
        vkCmdPushConstants(cmd, I.temporalPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(tp), &tp);
        const uint32_t gx = (I.width + 15u) / 16u;
        const uint32_t gy = (I.height + 15u) / 16u;
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    // Publish temporal writes (colorA, varA, moments[cur], geom[cur]) before the
    // à-trous reads.
    computeBarrier(cmd);

    // ================= Pass 2: variance-guided à-trous =================
    // Ping-pong schedule. Iteration 0's color output is histColor[cur] — it
    // becomes next frame's temporal history AND doubles as ping-pong buffer B
    // (SVGF feedback). Final iteration writes the RGBA8 beauty.
    const VkImageView A  = I.colorView[0];      // temporal accumColor / scratch
    const VkImageView B  = I.colorView[1];      // scratch
    const VkImageView HC = I.histColorView[cur];// iter-0 output == next-frame history
    const VkImageView vA = I.varView[0];
    const VkImageView vB = I.varView[1];

    const VkImageView inColor[kNumIterations]  = { A,  HC, B,  A,  B  };
    const VkImageView outColor[kNumIterations] = { HC, B,  A,  B,  A  };  // iter4 out16 unused (isFinal)
    const VkImageView inVar[kNumIterations]    = { vA, vB, vA, vB, vA };
    const VkImageView outVar[kNumIterations]   = { vB, vA, vB, vA, vB };

    for (uint32_t it = 0; it < kNumIterations; ++it) {
        const bool isFinal = (it == kNumIterations - 1);

        VkDescriptorImageInfo ii[7]{};
        VkImageView views[7] = {
            inColor[it],        // 0 inColor
            outColor[it],       // 1 outColor16
            inputs.normalView,  // 2 normal
            inputs.depthView,   // 3 depth
            inVar[it],          // 4 inVar
            outVar[it],         // 5 outVar
            inputs.beautyView,  // 6 outColorLDR (written only when isFinal)
        };
        VkWriteDescriptorSet w[7];
        for (uint32_t k = 0; k < 7; ++k) {
            ii[k].imageView   = views[k];
            ii[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[k] = imgWrite(I.atrousSets[it], k, &ii[k]);
        }
        vkUpdateDescriptorSets(I.device, 7, w, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.atrousPipeline);
    const uint32_t gx = (I.width + 15u) / 16u;
    const uint32_t gy = (I.height + 15u) / 16u;

    for (uint32_t it = 0; it < kNumIterations; ++it) {
        // Order previous pass's writes before this pass's reads (and the WAR on
        // the buffer about to be reused).
        computeBarrier(cmd);

        SvgfAtrousPush ap{};
        ap.stepSize    = 1 << it;   // 1,2,4,8,16
        ap.sigmaL      = I.sigmaL;
        ap.sigmaNormal = I.sigmaNormal;
        ap.sigmaDepth  = I.sigmaDepth;
        ap.isFinal     = (it == kNumIterations - 1) ? 1 : 0;
        vkCmdPushConstants(cmd, I.atrousPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(ap), &ap);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.atrousPipelineLayout,
                                0, 1, &I.atrousSets[it], 0, nullptr);
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    // Final beauty now lives in inputs.beautyImage (GENERAL, last written by
    // COMPUTE). histColor[cur]/histMoment[cur]/histGeom[cur] hold next frame's
    // temporal history.
    I.parity = 1u - I.parity;
}

}  // namespace ohao
