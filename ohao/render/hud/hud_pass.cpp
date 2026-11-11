#include "render/hud/hud_pass.hpp"
#include "render/shader_bindings/hud_hud.hpp"

#include "core/log.hpp"
#include "render/graph/pipeline.hpp"

#include <algorithm>
#include <cstring>

namespace ohao {

namespace {

struct HudPush {
    float invSize[2];
    float pad[2];
};
OHAO_CHECK_PUSH(HudPush, hud_hud)   // hud/hud.slang's

constexpr VkDeviceSize kItemBytes = sizeof(render::HudItem) * render::kHudMaxItems;
constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;   // the final image's

} // namespace

HudPass::~HudPass() { cleanup(); }

bool HudPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    if (!m_allocator) return false;
    m_device = device;
    m_physicalDevice = physicalDevice;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {shader::layoutBinding(shader::hud_hud::items, stages),
                                                     shader::layoutBinding(shader::hud_hud::font, stages)};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    // The font, once; the items, a buffer a frame slot (written when its frame is recorded).
    const auto rows = render::hudFontRows();
    m_font = m_allocator->createBuffer(rows.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::CpuToGpu, true);
    if (!m_font.isValid() || !m_font.getMappedData()) return false;
    std::memcpy(m_font.getMappedData(), rows.data(), rows.size_bytes());
    m_allocator->flushBuffer(m_font);
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        m_itemBuffers[s] = m_allocator->createBuffer(kItemBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::CpuToGpu, true);
        if (!m_itemBuffers[s].isValid() || !m_itemBuffers[s].getMappedData()) return false;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &m_sets[s]) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo items{m_itemBuffers[s].buffer, 0, kItemBytes};
        const VkDescriptorBufferInfo font{m_font.buffer, 0, rows.size_bytes()};
        VkWriteDescriptorSet writes[2] = {shader::write(shader::hud_hud::items, m_sets[s]),
                                          shader::write(shader::hud_hud::font, m_sets[s])};
        writes[0].pBufferInfo = &items;
        writes[1].pBufferInfo = &font;
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    }
    if (!createPipeline()) {
        OHAO_LOG(Error, render, "HudPass: pipeline failed");
        return false;
    }
    return true;
}

bool HudPass::createPipeline() {
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(HudPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) return false;

    // Over the image by alpha; the image's alpha kept.
    const render::Blend blend{true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ZERO,
                              VK_BLEND_FACTOR_ONE};
    render::GraphicsPipelineDesc desc{};
    desc.name = "hud";
    desc.vertex = "hud_hud.slang.vs.spv";
    desc.fragment = "hud_hud.slang.fs.spv";
    desc.vertexEntry = "vs";   // Slang keeps the entry points' names (-fvk-use-entrypoint-name)
    desc.fragmentEntry = "fs";
    desc.layout = m_layout;
    desc.colorFormats = {&kTargetFormat, 1};
    desc.blends = {&blend, 1};
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

void HudPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    safeDestroy(m_pipeline);
    safeDestroy(m_layout);
    safeDestroy(m_pool);   // frees the sets
    safeDestroy(m_setLayout);
    for (auto& b : m_itemBuffers) m_allocator->retire(b);
    m_allocator->retire(m_font);
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

void HudPass::setItems(std::span<const render::HudItem> items) {
    m_items.assign(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(std::min(items.size(), render::kHudMaxItems)));
}

void HudPass::record(const render::Frame& frame, gpu::Image& target) {
    if (m_items.empty() || m_pipeline == VK_NULL_HANDLE || !target || target.desc.format != kTargetFormat) return;
    const uint32_t slot = frame.slot % MAX_FRAMES_IN_FLIGHT;
    std::memcpy(m_itemBuffers[slot].getMappedData(), m_items.data(), m_items.size() * sizeof(render::HudItem));
    m_allocator->flushBuffer(m_itemBuffers[slot], 0, m_items.size() * sizeof(render::HudItem));

    render::Pass pass(frame, "hud", render::Pass::Raster);
    pass.read(m_itemBuffers[slot], render::BufferUse::Storage).read(m_font, render::BufferUse::Storage);
    pass.color(target, render::Load::Keep);
    pass.begin();
    const VkCommandBuffer cmd = frame.cmd;
    const VkExtent2D size = pass.area();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
    const HudPush pc{{1.0f / static_cast<float>(size.width), 1.0f / static_cast<float>(size.height)}, {0.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(m_items.size()), 0, 0);
}

} // namespace ohao
