#include "fft_ocean_sim.hpp"
#include <array>
#include <iostream>
#include <cmath>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool FFTOceanSim::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createTextures()) {
        std::cerr << "FFTOceanSim: texture creation failed" << std::endl;
        return false;
    }
    if (!createSpectrumInitPipeline()) {
        std::cerr << "FFTOceanSim: spectrum_init pipeline failed" << std::endl;
        return false;
    }
    if (!createSpectrumHktPipeline()) {
        std::cerr << "FFTOceanSim: spectrum_hkt pipeline failed" << std::endl;
        return false;
    }
    if (!createButterflyPipeline()) {
        std::cerr << "FFTOceanSim: fft_butterfly pipeline failed" << std::endl;
        return false;
    }
    if (!createNormalPipeline()) {
        std::cerr << "FFTOceanSim: fft_normal pipeline failed" << std::endl;
        return false;
    }

    std::cout << "FFTOceanSim: OK (" << N << "x" << N << ")" << std::endl;
    return true;
}

void FFTOceanSim::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    // Pipelines
    safeDestroy(m_initPipeline);    safeDestroy(m_initPL);
    safeDestroy(m_initPool);        safeDestroy(m_initDSL);
    safeDestroy(m_hktPipeline);     safeDestroy(m_hktPL);
    safeDestroy(m_hktPool);         safeDestroy(m_hktDSL);
    safeDestroy(m_bflyPipeline);    safeDestroy(m_bflyPL);
    safeDestroy(m_bflyPool);        safeDestroy(m_bflyDSL);
    safeDestroy(m_normPipeline);    safeDestroy(m_normPL);
    safeDestroy(m_normPool);        safeDestroy(m_normDSL);

    // Textures
    safeDestroy(m_h0Sampler);
    safeDestroy(m_h0View);
    safeDestroy(m_h0Image);
    safeFree(m_h0Memory);

    safeDestroy(m_hktSampler);
    safeDestroy(m_hktView);
    safeDestroy(m_hktImage);
    safeFree(m_hktMemory);

    safeDestroy(m_normalSampler);
    safeDestroy(m_normalView);
    safeDestroy(m_normalImage);
    safeFree(m_normalMemory);

    m_h0Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    m_hktLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    m_normalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Wind direction
// ---------------------------------------------------------------------------

void FFTOceanSim::setWindDirection(float x, float z) {
    float len = std::sqrt(x * x + z * z);
    if (len > 1e-4f) { x /= len; z /= len; }
    else { x = 1.0f; z = 0.0f; }
    m_windDirX = x;
    m_windDirZ = z;
    m_spectrumDirty = true;
}

// ---------------------------------------------------------------------------
// simulate()
// ---------------------------------------------------------------------------

void FFTOceanSim::simulate(VkCommandBuffer cmd, float time, float /*dt*/) {
    // ── 1. Spectrum init (once, or when wind changes) ──────────────────────
    if (m_spectrumDirty) {
        transitionTo(cmd, m_h0Image, m_h0Layout, VK_IMAGE_LAYOUT_GENERAL);

        struct SpectrumInitPC {
            float windX, windZ, patchSize, amplitude;
            int   N;
            float gravity, suppression, pad;
        } spc{
            m_windDirX * m_windSpeed,
            m_windDirZ * m_windSpeed,
            m_patchSize,
            m_amplitude,
            static_cast<int>(N),
            9.81f, 0.07f, 0.0f
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_initPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_initPL, 0, 1, &m_initDS, 0, nullptr);
        vkCmdPushConstants(cmd, m_initPL, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(spc), &spc);
        vkCmdDispatch(cmd, N / 16, N / 16, 1);

        computeBarrier(cmd);
        transitionTo(cmd, m_h0Image, m_h0Layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_spectrumDirty = false;
    }

    // ── 2. Per-frame h(k,t) ────────────────────────────────────────────────
    transitionTo(cmd, m_hktImage, m_hktLayout, VK_IMAGE_LAYOUT_GENERAL);

    struct HktPC { float time, patchSize; int N; float pad; } hkt{
        time, m_patchSize, static_cast<int>(N), 0.0f
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hktPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_hktPL, 0, 1, &m_hktDS, 0, nullptr);
    vkCmdPushConstants(cmd, m_hktPL, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(hkt), &hkt);
    vkCmdDispatch(cmd, N / 16, N / 16, 1);
    computeBarrier(cmd);

    // ── 3. IFFT horizontal ─────────────────────────────────────────────────
    struct ButterflyPC { int N, direction, pad0, pad1; };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bflyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_bflyPL, 0, 1, &m_bflyDS, 0, nullptr);

    ButterflyPC bpcH{ static_cast<int>(N), 0, 0, 0 };
    vkCmdPushConstants(cmd, m_bflyPL, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(bpcH), &bpcH);
    vkCmdDispatch(cmd, 1, N, 1);
    computeBarrier(cmd);

    // ── 4. IFFT vertical ───────────────────────────────────────────────────
    ButterflyPC bpcV{ static_cast<int>(N), 1, 0, 0 };
    vkCmdPushConstants(cmd, m_bflyPL, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(bpcV), &bpcV);
    vkCmdDispatch(cmd, N, 1, 1);
    computeBarrier(cmd);

    // ── 5. Transition hkt for sampling in fft_normal ───────────────────────
    transitionTo(cmd, m_hktImage, m_hktLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionTo(cmd, m_normalImage, m_normalLayout, VK_IMAGE_LAYOUT_GENERAL);

    struct NormalPC { float patchSize; int N; float normalStrength, foamThreshold; } npc{
        m_patchSize, static_cast<int>(N), m_normalStrength, m_foamThreshold
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_normPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_normPL, 0, 1, &m_normDS, 0, nullptr);
    vkCmdPushConstants(cmd, m_normPL, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(npc), &npc);
    vkCmdDispatch(cmd, N / 16, N / 16, 1);

    // ── 6. Final transitions → SHADER_READ_ONLY for WaterPass vertex shader ─
    // Include VK_PIPELINE_STAGE_VERTEX_SHADER_BIT in dst so the water vertex
    // shader can safely sample these images.
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // Image layout transitions for hkt and normal (GENERAL → SHADER_READ_ONLY)
    {
        std::array<VkImageMemoryBarrier, 2> barriers{};
        auto fill = [&](VkImageMemoryBarrier& b, VkImage img, VkImageLayout& cur, VkImageLayout dst) {
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout           = cur;
            b.newLayout           = dst;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = img;
            b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            cur = dst;
        };
        // hkt was already transitioned to SHADER_READ_ONLY above, but we need
        // the memory access barrier to cover vertex shader stage.
        // normalImg is still GENERAL after compute write.
        fill(barriers[0], m_normalImage, m_normalLayout,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Use hktImage barrier to also cover vertex reads (already in correct layout).
        fill(barriers[1], m_hktImage, m_hktLayout,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void FFTOceanSim::transitionTo(VkCommandBuffer cmd, VkImage image,
                                VkImageLayout& current, VkImageLayout newLayout) {
    if (current == newLayout) return;

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = current;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Conservative access masks
    if (current == VK_IMAGE_LAYOUT_UNDEFINED) {
        b.srcAccessMask = 0;
    } else if (current == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else {
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    }

    if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else {
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);

    current = newLayout;
}

void FFTOceanSim::computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Texture creation
// ---------------------------------------------------------------------------

bool FFTOceanSim::createTextures() {
    // Helper: create image + memory + view
    auto makeImage = [&](VkFormat fmt, VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = fmt;
        ci.extent        = {N, N, 1};
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_STORAGE_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ci, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, img, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
        if (vkBindImageMemory(m_device, img, mem, 0) != VK_SUCCESS) return false;

        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = img;
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = fmt;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 1;
        return vkCreateImageView(m_device, &vi, nullptr, &view) == VK_SUCCESS;
    };

    // Helper: linear clamp sampler
    auto makeSampler = [&](VkSampler& s) -> bool {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        return vkCreateSampler(m_device, &si, nullptr, &s) == VK_SUCCESS;
    };

    if (!makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, m_h0Image,     m_h0Memory,     m_h0View))     return false;
    if (!makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, m_hktImage,    m_hktMemory,    m_hktView))    return false;
    if (!makeImage(VK_FORMAT_R16G16B16A16_SFLOAT, m_normalImage, m_normalMemory, m_normalView)) return false;

    if (!makeSampler(m_h0Sampler))     return false;
    if (!makeSampler(m_hktSampler))    return false;
    if (!makeSampler(m_normalSampler)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Pipeline helpers
// ---------------------------------------------------------------------------

namespace {

// Create a compute pipeline with a single descriptor-set layout and push constants.
// Returns false on failure; caller destroys partial resources.
bool buildComputePipeline(
    VkDevice device,
    const std::string& spvPath,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    uint32_t pushConstantSize,
    VkDescriptorSetLayout& outDSL,
    VkDescriptorPool&      outPool,
    VkDescriptorSet&       outDS,
    VkPipelineLayout&      outPL,
    VkPipeline&            outPipeline)
{
    // Descriptor set layout
    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = static_cast<uint32_t>(bindings.size());
    dslCI.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &outDSL) != VK_SUCCESS)
        return false;

    // Descriptor pool
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (auto& b : bindings) {
        bool found = false;
        for (auto& ps : poolSizes)
            if (ps.type == b.descriptorType) { ps.descriptorCount++; found = true; break; }
        if (!found) poolSizes.push_back({b.descriptorType, 1});
    }
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &outPool) != VK_SUCCESS)
        return false;

    // Descriptor set
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = outPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &outDSL;
    if (vkAllocateDescriptorSets(device, &dsAlloc, &outDS) != VK_SUCCESS)
        return false;

    // Pipeline layout
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = pushConstantSize;

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &outDSL;
    plCI.pushConstantRangeCount = pushConstantSize > 0 ? 1 : 0;
    plCI.pPushConstantRanges    = pushConstantSize > 0 ? &pcRange : nullptr;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &outPL) != VK_SUCCESS)
        return false;

    // Load SPIR-V
    VkShaderModule mod = VK_NULL_HANDLE;
    {
        FILE* f = nullptr;
        std::string fullPath = RenderPassBase::getShaderBasePath() + spvPath;
        f = fopen(fullPath.c_str(), "rb");
        if (!f) {
            std::cerr << "FFTOceanSim: cannot open shader " << fullPath << std::endl;
            return false;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        std::vector<uint32_t> code(sz / 4);
        fread(code.data(), 1, sz, f);
        fclose(f);

        VkShaderModuleCreateInfo smCI{};
        smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCI.codeSize = sz;
        smCI.pCode    = code.data();
        if (vkCreateShaderModule(device, &smCI, nullptr, &mod) != VK_SUCCESS)
            return false;
    }

    VkComputePipelineCreateInfo cpCI{};
    cpCI.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCI.layout       = outPL;
    cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module = mod;
    cpCI.stage.pName  = "main";

    VkResult res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &outPipeline);
    vkDestroyShaderModule(device, mod, nullptr);
    return res == VK_SUCCESS;
}

} // anonymous namespace

// ── spectrum_init: binding 0 = STORAGE_IMAGE (h0Img write) ─────────────────
bool FFTOceanSim::createSpectrumInitPipeline() {
    VkDescriptorSetLayoutBinding b0{};
    b0.binding         = 0;
    b0.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b0.descriptorCount = 1;
    b0.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    if (!buildComputePipeline(m_device,
            "water_waves_spectrum_init.comp.spv",
            {b0}, 32,
            m_initDSL, m_initPool, m_initDS, m_initPL, m_initPipeline))
        return false;

    // Write descriptor: h0Img as STORAGE_IMAGE (GENERAL)
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = m_h0View;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = m_initDS;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.descriptorCount = 1;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    return true;
}

// ── spectrum_hkt: binding 0 = SAMPLER h0Tex, binding 1 = STORAGE_IMAGE hktImg
bool FFTOceanSim::createSpectrumHktPipeline() {
    std::vector<VkDescriptorSetLayoutBinding> bindings(2);
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    if (!buildComputePipeline(m_device,
            "water_waves_spectrum_hkt.comp.spv",
            bindings, 16,
            m_hktDSL, m_hktPool, m_hktDS, m_hktPL, m_hktPipeline))
        return false;

    std::array<VkDescriptorImageInfo, 2> infos{};
    infos[0] = {m_h0Sampler,  m_h0View,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[1] = {VK_NULL_HANDLE, m_hktView, VK_IMAGE_LAYOUT_GENERAL};

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_hktDS;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    vkUpdateDescriptorSets(m_device, 2, writes.data(), 0, nullptr);
    return true;
}

// ── fft_butterfly: binding 0 = STORAGE_IMAGE hktImg (in-place) ─────────────
bool FFTOceanSim::createButterflyPipeline() {
    VkDescriptorSetLayoutBinding b0{};
    b0.binding         = 0;
    b0.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b0.descriptorCount = 1;
    b0.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    if (!buildComputePipeline(m_device,
            "water_waves_fft_butterfly.comp.spv",
            {b0}, 16,
            m_bflyDSL, m_bflyPool, m_bflyDS, m_bflyPL, m_bflyPipeline))
        return false;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = m_hktView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = m_bflyDS;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.descriptorCount = 1;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    return true;
}

// ── fft_normal: binding 0 = SAMPLER hktImg, binding 1 = STORAGE_IMAGE normalImg
bool FFTOceanSim::createNormalPipeline() {
    std::vector<VkDescriptorSetLayoutBinding> bindings(2);
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    if (!buildComputePipeline(m_device,
            "water_waves_fft_normal.comp.spv",
            bindings, 16,
            m_normDSL, m_normPool, m_normDS, m_normPL, m_normPipeline))
        return false;

    std::array<VkDescriptorImageInfo, 2> infos{};
    infos[0] = {m_hktSampler,   m_hktView,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[1] = {VK_NULL_HANDLE, m_normalView, VK_IMAGE_LAYOUT_GENERAL};

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_normDS;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    vkUpdateDescriptorSets(m_device, 2, writes.data(), 0, nullptr);
    return true;
}

} // namespace ohao
