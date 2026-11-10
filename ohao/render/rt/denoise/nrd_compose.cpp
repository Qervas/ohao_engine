// Sub-plan 4.D: NrdCompositor — Vulkan compute pipeline that remodulates
// NRD's denoised radiance with demod albedo+F0.

#include "render/rt/denoise/nrd_compose.hpp"
#include "render/shader_bindings/rt_nrd_compose.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <fstream>
#include <vector>

#include "core/log.hpp"
#include "render/frame/frame_resources.hpp"   // MAX_FRAMES_IN_FLIGHT

namespace ohao {

namespace {

// Matches PathTracer's readFile helper: tries the raw path first, then
// common build-output locations.
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

struct NrdCompositor::Impl {
    VkDevice              device         = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice = VK_NULL_HANDLE;
    uint32_t              width          = 0;
    uint32_t              height         = 0;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    // One set per frame in flight: dispatch() writes its slot's set at record time,
    // while an earlier frame may still read its own (VUID 03047).
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets{};
    uint32_t              slot           = 0;
};

NrdCompositor::NrdCompositor()  : m_impl(std::make_unique<Impl>()) {}
NrdCompositor::~NrdCompositor() { shutdown(); }

bool NrdCompositor::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // 1. Descriptor set layout: rt/nrd_compose.slang's set 0.
    const auto bindings = shader::layoutBindings(shader::rt_nrd_compose::kSet0, VK_SHADER_STAGE_COMPUTE_BIT);

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout); r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "NRD compose: vkCreateDescriptorSetLayout failed: {}", static_cast<int>(r));
        return false;
    }

    // 2. Pipeline layout: the display transform's exposure and curve as push constants.
    const VkPushConstantRange displayRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_impl->setLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &displayRange;
    if (VkResult r = vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout); r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "NRD compose: vkCreatePipelineLayout failed: {}", static_cast<int>(r));
        return false;
    }

    // 3. Load SPV and create shader module.
    // shaders/rt/nrd_compose.slang (one entry point) → rt_nrd_compose.slang.spv.
    auto spv = readShaderSpv("rt_nrd_compose.slang.spv");
    if (spv.empty()) {
        OHAO_LOG(Error, render, "NRD compose: failed to load rt_nrd_compose.slang.spv");
        return false;
    }
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule); r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "NRD compose: vkCreateShaderModule failed: {}", static_cast<int>(r));
        return false;
    }

    // 4. Compute pipeline.
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
        OHAO_LOG(Error, render, "NRD compose: vkCreateComputePipelines failed: {}", static_cast<int>(pipeResult));
        return false;
    }

    // 5. Descriptor pool (one set of 5 storage images per frame in flight).
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 7 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
    if (VkResult r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool); r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "NRD compose: vkCreateDescriptorPool failed: {}", static_cast<int>(r));
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(m_impl->setLayout);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsai.pSetLayouts        = layouts.data();
    if (VkResult r = vkAllocateDescriptorSets(device, &dsai, m_impl->descriptorSets.data()); r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "NRD compose: vkAllocateDescriptorSets failed: {}", static_cast<int>(r));
        return false;
    }

    OHAO_LOG(Info, render, "NRD compose: pipeline ready @ {}x{}", width, height);
    return true;
}

void NrdCompositor::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    if (m_impl->descriptorPool) { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)       { vkDestroyPipeline(d, m_impl->pipeline, nullptr);              m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout) { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr);  m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)      { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr);  m_impl->setLayout      = VK_NULL_HANDLE; }
    m_impl->descriptorSets = {};
    m_impl->device         = VK_NULL_HANDLE;
}

void NrdCompositor::setFrameSlot(uint32_t slot) {
    if (slot < MAX_FRAMES_IN_FLIGHT) m_impl->slot = slot;
}

void NrdCompositor::dispatch(VkCommandBuffer cmd, const NrdComposeInputs& inputs) {
    if (!m_impl->pipeline) return;

    // Bind 7 image views into the descriptor set.
    std::array<VkDescriptorImageInfo, 7> imageInfos{};
    imageInfos[0].imageView   = inputs.diffRadiance;
    imageInfos[1].imageView   = inputs.specRadiance;
    imageInfos[2].imageView   = inputs.diffAlbedo;
    imageInfos[3].imageView   = inputs.specColor;
    imageInfos[4].imageView   = inputs.composedOut;
    imageInfos[5].imageView   = inputs.emission;
    imageInfos[6].imageView   = inputs.outputLdr;
    for (auto& ii : imageInfos) ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, shader::rt_nrd_compose::kSet0.size()> writes{};
    static_assert(writes.size() == decltype(imageInfos){}.size());
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = shader::write(shader::rt_nrd_compose::kSet0[i], m_impl->descriptorSets[m_impl->slot]);
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_impl->device, static_cast<uint32_t>(writes.size()), writes.data(),
                            0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipelineLayout, 0, 1,
                             &m_impl->descriptorSets[m_impl->slot], 0, nullptr);

    const shader::rt_nrd_compose::PushConstants display{inputs.exposure, inputs.curve};
    vkCmdPushConstants(cmd, m_impl->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(display), &display);

    const uint32_t gx = (m_impl->width  + 7u) / 8u;
    const uint32_t gy = (m_impl->height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED

namespace ohao {

struct NrdCompositor::Impl {};
NrdCompositor::NrdCompositor()  : m_impl(std::make_unique<Impl>()) {}
NrdCompositor::~NrdCompositor() = default;

bool NrdCompositor::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdCompositor::shutdown() {}
void NrdCompositor::setFrameSlot(uint32_t) {}
void NrdCompositor::dispatch(VkCommandBuffer, const NrdComposeInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
