#include "render_graph.hpp"
#include "renderer/memory/gpu_allocator.hpp"
#include <iostream>
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace ohao {

// PassBuilder implementation
PassBuilder::PassBuilder(RenderGraph& graph, uint32_t passIndex)
    : m_graph(graph), m_passIndex(passIndex) {}

TextureHandle PassBuilder::createColorAttachment(const std::string& name, uint32_t width,
                                                   uint32_t height, VkFormat format) {
    TextureDesc desc = TextureDesc::colorTarget(name, width, height, format);
    TextureHandle handle = m_graph.createTexture(desc);

    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::ColorAttachment;
    access.stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    access.accessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    m_graph.addPassWrite(m_passIndex, access);

    m_graph.m_passes[m_passIndex].colorAttachments.push_back(handle);
    m_graph.m_passes[m_passIndex].viewportWidth = width;
    m_graph.m_passes[m_passIndex].viewportHeight = height;

    return handle;
}

TextureHandle PassBuilder::createHdrColorAttachment(const std::string& name,
                                                      uint32_t width, uint32_t height) {
    return createColorAttachment(name, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
}

TextureHandle PassBuilder::createDepthAttachment(const std::string& name, uint32_t width,
                                                   uint32_t height, VkFormat format) {
    TextureDesc desc = TextureDesc::depthTarget(name, width, height, format);
    TextureHandle handle = m_graph.createTexture(desc);

    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::DepthAttachment;
    access.stageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    access.accessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    m_graph.addPassWrite(m_passIndex, access);

    m_graph.m_passes[m_passIndex].depthAttachment = handle;
    if (m_graph.m_passes[m_passIndex].viewportWidth == 0) {
        m_graph.m_passes[m_passIndex].viewportWidth = width;
        m_graph.m_passes[m_passIndex].viewportHeight = height;
    }

    return handle;
}

TextureHandle PassBuilder::createShadowMap(const std::string& name, uint32_t size) {
    return createDepthAttachment(name, size, size, VK_FORMAT_D32_SFLOAT);
}

TextureHandle PassBuilder::createGBufferAttachment(const std::string& name, uint32_t width,
                                                     uint32_t height, VkFormat format) {
    return createColorAttachment(name, width, height, format);
}

void PassBuilder::readTexture(TextureHandle handle, VkPipelineStageFlags stage) {
    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::ShaderRead;
    access.stageMask = stage;
    access.accessMask = VK_ACCESS_SHADER_READ_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_graph.addPassRead(m_passIndex, access);
}

void PassBuilder::sampleTexture(TextureHandle handle) {
    readTexture(handle, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void PassBuilder::writeStorageTexture(TextureHandle handle) {
    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::Storage;
    access.stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    access.accessMask = VK_ACCESS_SHADER_WRITE_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    m_graph.addPassWrite(m_passIndex, access);
}

void PassBuilder::useColorAttachment(TextureHandle handle) {
    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::ColorAttachment;
    access.stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    access.accessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    m_graph.addPassWrite(m_passIndex, access);

    m_graph.m_passes[m_passIndex].colorAttachments.push_back(handle);
}

void PassBuilder::useDepthAttachment(TextureHandle handle) {
    ResourceAccess access{};
    access.texture = handle;
    access.textureUsage = TextureUsage::DepthAttachment;
    access.stageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    access.accessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    access.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    m_graph.addPassWrite(m_passIndex, access);

    m_graph.m_passes[m_passIndex].depthAttachment = handle;
}

BufferHandle PassBuilder::createBuffer(const std::string& name, VkDeviceSize size, BufferUsage usage) {
    BufferDesc desc{name, size, usage, true};
    return m_graph.createBuffer(desc);
}

void PassBuilder::readBuffer(BufferHandle handle, BufferUsage usage) {
    ResourceAccess access{};
    access.buffer = handle;
    access.bufferUsage = usage;
    access.stageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    access.accessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    m_graph.addPassRead(m_passIndex, access);
}

void PassBuilder::writeBuffer(BufferHandle handle) {
    ResourceAccess access{};
    access.buffer = handle;
    access.bufferUsage = BufferUsage::StorageBuffer;
    access.stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    access.accessMask = VK_ACCESS_SHADER_WRITE_BIT;
    m_graph.addPassWrite(m_passIndex, access);
}

void PassBuilder::setComputeOnly() {
    m_graph.m_passes[m_passIndex].type = PassType::Compute;
}

void PassBuilder::setViewport(uint32_t width, uint32_t height) {
    m_graph.m_passes[m_passIndex].viewportWidth = width;
    m_graph.m_passes[m_passIndex].viewportHeight = height;
}

// RenderGraph implementation
RenderGraph::~RenderGraph() {
    shutdown();
}

bool RenderGraph::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                              GpuAllocator* allocator) {
    if (m_device != VK_NULL_HANDLE) {
        return true;
    }

    m_device = device;
    m_physicalDevice = physicalDevice;
    m_allocator = allocator;

    std::cout << "Render graph initialized" << std::endl;
    return true;
}

void RenderGraph::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(m_device);

    // Free all allocated resources
    for (size_t i = 0; i < m_physicalTextures.size(); ++i) {
        freeTexture({static_cast<uint32_t>(i)});
    }

    for (size_t i = 0; i < m_physicalBuffers.size(); ++i) {
        freeBuffer({static_cast<uint32_t>(i)});
    }

    // Destroy render passes and framebuffers
    for (auto& pass : m_passes) {
        if (pass.vulkanFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, pass.vulkanFramebuffer, nullptr);
        }
        if (pass.vulkanRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, pass.vulkanRenderPass, nullptr);
        }
    }

    m_passes.clear();
    m_compiledPasses.clear();
    m_textureDescs.clear();
    m_bufferDescs.clear();
    m_physicalTextures.clear();
    m_physicalBuffers.clear();
    m_textureNameMap.clear();
    m_bufferNameMap.clear();

    m_device = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_allocator = nullptr;
}

void RenderGraph::addPass(const std::string& name,
                           std::function<void(PassBuilder&)> setup,
                           std::function<void(VkCommandBuffer)> execute) {
    RenderPassDef pass{};
    pass.name = name;
    pass.index = static_cast<uint32_t>(m_passes.size());
    pass.type = PassType::Graphics;
    pass.executeCallback = std::move(execute);

    m_passes.push_back(std::move(pass));

    // Run setup to populate resource dependencies
    PassBuilder builder(*this, pass.index);
    setup(builder);

    m_compiled = false;
}

void RenderGraph::addComputePass(const std::string& name,
                                   std::function<void(PassBuilder&)> setup,
                                   std::function<void(VkCommandBuffer)> execute) {
    RenderPassDef pass{};
    pass.name = name;
    pass.index = static_cast<uint32_t>(m_passes.size());
    pass.type = PassType::Compute;
    pass.executeCallback = std::move(execute);

    m_passes.push_back(std::move(pass));

    PassBuilder builder(*this, pass.index);
    setup(builder);

    m_compiled = false;
}

TextureHandle RenderGraph::importTexture(const std::string& name, VkImage image,
                                           VkImageView view, VkFormat format,
                                           uint32_t width, uint32_t height,
                                           VkImageLayout currentLayout) {
    TextureHandle handle{static_cast<uint32_t>(m_textureDescs.size())};

    TextureDesc desc{};
    desc.name = name;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.isExternal = true;
    desc.isTransient = false;

    m_textureDescs.push_back(desc);
    m_textureNameMap[name] = handle;

    // Create physical texture entry
    PhysicalTexture physical{};
    physical.image = image;
    physical.view = view;
    physical.format = format;
    physical.width = width;
    physical.height = height;
    physical.currentLayout = currentLayout;
    physical.ownsMemory = false;

    m_physicalTextures.push_back(physical);

    return handle;
}

BufferHandle RenderGraph::importBuffer(const std::string& name, VkBuffer buffer,
                                         VkDeviceSize size, void* mapped) {
    BufferHandle handle{static_cast<uint32_t>(m_bufferDescs.size())};

    BufferDesc desc{};
    desc.name = name;
    desc.size = size;
    desc.isTransient = false;

    m_bufferDescs.push_back(desc);
    m_bufferNameMap[name] = handle;

    PhysicalBuffer physical{};
    physical.buffer = buffer;
    physical.size = size;
    physical.mapped = mapped;
    physical.ownsMemory = false;

    m_physicalBuffers.push_back(physical);

    return handle;
}

void RenderGraph::setOutput(TextureHandle handle) {
    m_outputHandle = handle;
}

TextureHandle RenderGraph::createTexture(const TextureDesc& desc) {
    // Check if texture with this name already exists
    auto it = m_textureNameMap.find(desc.name);
    if (it != m_textureNameMap.end()) {
        return it->second;
    }

    TextureHandle handle{static_cast<uint32_t>(m_textureDescs.size())};
    m_textureDescs.push_back(desc);
    m_textureNameMap[desc.name] = handle;

    // Physical resource will be allocated during compile()
    m_physicalTextures.push_back({});

    return handle;
}

BufferHandle RenderGraph::createBuffer(const BufferDesc& desc) {
    auto it = m_bufferNameMap.find(desc.name);
    if (it != m_bufferNameMap.end()) {
        return it->second;
    }

    BufferHandle handle{static_cast<uint32_t>(m_bufferDescs.size())};
    m_bufferDescs.push_back(desc);
    m_bufferNameMap[desc.name] = handle;

    m_physicalBuffers.push_back({});

    return handle;
}

void RenderGraph::addPassRead(uint32_t passIndex, const ResourceAccess& access) {
    if (passIndex < m_passes.size()) {
        m_passes[passIndex].reads.push_back(access);
    }
}

void RenderGraph::addPassWrite(uint32_t passIndex, const ResourceAccess& access) {
    if (passIndex < m_passes.size()) {
        m_passes[passIndex].writes.push_back(access);
    }
}

bool RenderGraph::compile() {
    if (m_compiled) {
        return true;
    }

    if (m_passes.empty()) {
        std::cerr << "RenderGraph: No passes to compile" << std::endl;
        return false;
    }

    // Step 1: Allocate resources
    allocateResources();

    // Step 2: Build dependency graph and sort passes
    buildDependencyGraph();
    topologicalSort();

    // Step 3: Compute barriers between passes
    computeBarriers();

    // Step 4: Create Vulkan render passes and framebuffers
    createRenderPasses();
    createFramebuffers();

    m_compiled = true;
    std::cout << "RenderGraph compiled: " << m_passes.size() << " passes, "
              << m_textureDescs.size() << " textures, "
              << m_bufferDescs.size() << " buffers" << std::endl;

    return true;
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    if (!m_compiled) {
        std::cerr << "RenderGraph: Not compiled" << std::endl;
        return;
    }

    for (const auto& compiledPass : m_compiledPasses) {
        const RenderPassDef& pass = m_passes[compiledPass.passIndex];

        // Insert barriers
        for (const auto& barrier : compiledPass.barriers) {
            if (barrier.texture.isValid()) {
                PhysicalTexture& tex = m_physicalTextures[barrier.texture.index];

                VkImageMemoryBarrier imageBarrier{};
                imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                imageBarrier.srcAccessMask = barrier.srcAccess;
                imageBarrier.dstAccessMask = barrier.dstAccess;
                imageBarrier.oldLayout = barrier.oldLayout;
                imageBarrier.newLayout = barrier.newLayout;
                imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageBarrier.image = tex.image;
                imageBarrier.subresourceRange.aspectMask =
                    (barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
                     barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                    ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                imageBarrier.subresourceRange.baseMipLevel = 0;
                imageBarrier.subresourceRange.levelCount = 1;
                imageBarrier.subresourceRange.baseArrayLayer = 0;
                imageBarrier.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(cmd, barrier.srcStage, barrier.dstStage, 0,
                                      0, nullptr, 0, nullptr, 1, &imageBarrier);

                tex.currentLayout = barrier.newLayout;
            }
        }

        // Execute pass
        if (pass.type == PassType::Graphics && pass.vulkanRenderPass != VK_NULL_HANDLE) {
            VkRenderPassBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            beginInfo.renderPass = pass.vulkanRenderPass;
            beginInfo.framebuffer = pass.vulkanFramebuffer;
            beginInfo.renderArea.offset = {0, 0};
            beginInfo.renderArea.extent = {pass.viewportWidth, pass.viewportHeight};

            // Clear values for attachments
            std::vector<VkClearValue> clearValues;
            for (size_t i = 0; i < pass.colorAttachments.size(); ++i) {
                VkClearValue clear{};
                clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                clearValues.push_back(clear);
            }
            if (pass.depthAttachment.isValid()) {
                VkClearValue clear{};
                clear.depthStencil = {1.0f, 0};
                clearValues.push_back(clear);
            }

            beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
            beginInfo.pClearValues = clearValues.data();

            vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

            // Set viewport and scissor
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(pass.viewportWidth);
            viewport.height = static_cast<float>(pass.viewportHeight);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = {pass.viewportWidth, pass.viewportHeight};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }

        // Call user execute callback
        if (pass.executeCallback) {
            pass.executeCallback(cmd);
        }

        // End render pass if graphics
        if (pass.type == PassType::Graphics && pass.vulkanRenderPass != VK_NULL_HANDLE) {
            vkCmdEndRenderPass(cmd);
        }
    }
}

void RenderGraph::reset() {
    // Clear per-frame state but keep resources
    m_passes.clear();
    m_compiledPasses.clear();
    m_compiled = false;
    m_outputHandle = TextureHandle::invalid();

    // Reset layouts for next frame
    for (auto& tex : m_physicalTextures) {
        if (tex.ownsMemory) {
            tex.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }
}

PhysicalTexture* RenderGraph::getPhysicalTexture(TextureHandle handle) {
    if (!handle.isValid() || handle.index >= m_physicalTextures.size()) {
        return nullptr;
    }
    return &m_physicalTextures[handle.index];
}

PhysicalBuffer* RenderGraph::getPhysicalBuffer(BufferHandle handle) {
    if (!handle.isValid() || handle.index >= m_physicalBuffers.size()) {
        return nullptr;
    }
    return &m_physicalBuffers[handle.index];
}

void RenderGraph::buildDependencyGraph() {
    // Build producer/consumer relationships based on resource access
    for (auto& pass : m_passes) {
        pass.refCount = 0;
        pass.executed = false;
    }

    // Count how many passes each pass depends on
    for (size_t i = 0; i < m_passes.size(); ++i) {
        for (const auto& read : m_passes[i].reads) {
            // Find producer of this resource
            for (size_t j = 0; j < i; ++j) {
                for (const auto& write : m_passes[j].writes) {
                    if (read.texture.isValid() && read.texture == write.texture) {
                        m_passes[i].refCount++;
                    }
                    if (read.buffer.isValid() && read.buffer == write.buffer) {
                        m_passes[i].refCount++;
                    }
                }
            }
        }
    }
}

void RenderGraph::topologicalSort() {
    m_compiledPasses.clear();
    m_compiledPasses.reserve(m_passes.size());

    // Simple linear ordering for now (passes are added in correct order)
    // A full implementation would use Kahn's algorithm
    for (size_t i = 0; i < m_passes.size(); ++i) {
        CompiledPass compiled{};
        compiled.passIndex = static_cast<uint32_t>(i);
        m_compiledPasses.push_back(compiled);
    }
}

void RenderGraph::allocateResources() {
    // Allocate all textures
    for (size_t i = 0; i < m_textureDescs.size(); ++i) {
        const TextureDesc& desc = m_textureDescs[i];
        if (!desc.isExternal && m_physicalTextures[i].image == VK_NULL_HANDLE) {
            allocateTexture({static_cast<uint32_t>(i)});
        }
    }

    // Allocate all buffers
    for (size_t i = 0; i < m_bufferDescs.size(); ++i) {
        if (m_physicalBuffers[i].buffer == VK_NULL_HANDLE) {
            allocateBuffer({static_cast<uint32_t>(i)});
        }
    }
}

void RenderGraph::computeBarriers() {
    // Track last usage of each resource
    std::unordered_map<uint32_t, ResourceAccess> lastTextureAccess;
    std::unordered_map<uint32_t, ResourceAccess> lastBufferAccess;

    for (auto& compiledPass : m_compiledPasses) {
        const RenderPassDef& pass = m_passes[compiledPass.passIndex];

        // Check reads - need barrier if resource was written
        for (const auto& read : pass.reads) {
            if (read.texture.isValid()) {
                auto it = lastTextureAccess.find(read.texture.index);
                if (it != lastTextureAccess.end() && it->second.isWrite()) {
                    ResourceBarrier barrier{};
                    barrier.texture = read.texture;
                    barrier.srcStage = it->second.stageMask;
                    barrier.dstStage = read.stageMask;
                    barrier.srcAccess = it->second.accessMask;
                    barrier.dstAccess = read.accessMask;
                    barrier.oldLayout = it->second.imageLayout;
                    barrier.newLayout = read.imageLayout;
                    compiledPass.barriers.push_back(barrier);
                } else if (m_physicalTextures[read.texture.index].currentLayout != read.imageLayout) {
                    // Transition from undefined
                    ResourceBarrier barrier{};
                    barrier.texture = read.texture;
                    barrier.srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    barrier.dstStage = read.stageMask;
                    barrier.srcAccess = 0;
                    barrier.dstAccess = read.accessMask;
                    barrier.oldLayout = m_physicalTextures[read.texture.index].currentLayout;
                    barrier.newLayout = read.imageLayout;
                    compiledPass.barriers.push_back(barrier);
                }
                lastTextureAccess[read.texture.index] = read;
            }
        }

        // Update last access for writes
        for (const auto& write : pass.writes) {
            if (write.texture.isValid()) {
                auto it = lastTextureAccess.find(write.texture.index);
                if (it != lastTextureAccess.end()) {
                    VkImageLayout oldLayout = it->second.imageLayout;
                    if (oldLayout != write.imageLayout) {
                        ResourceBarrier barrier{};
                        barrier.texture = write.texture;
                        barrier.srcStage = it->second.stageMask;
                        barrier.dstStage = write.stageMask;
                        barrier.srcAccess = it->second.accessMask;
                        barrier.dstAccess = write.accessMask;
                        barrier.oldLayout = oldLayout;
                        barrier.newLayout = write.imageLayout;
                        compiledPass.barriers.push_back(barrier);
                    }
                } else {
                    // First use - transition from undefined
                    ResourceBarrier barrier{};
                    barrier.texture = write.texture;
                    barrier.srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    barrier.dstStage = write.stageMask;
                    barrier.srcAccess = 0;
                    barrier.dstAccess = write.accessMask;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = write.imageLayout;
                    compiledPass.barriers.push_back(barrier);
                }
                lastTextureAccess[write.texture.index] = write;
            }
        }
    }
}

void RenderGraph::createRenderPasses() {
    for (auto& pass : m_passes) {
        if (pass.type != PassType::Graphics) {
            continue;
        }

        if (pass.colorAttachments.empty() && !pass.depthAttachment.isValid()) {
            continue;
        }

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs;
        VkAttachmentReference depthRef{};
        depthRef.attachment = VK_ATTACHMENT_UNUSED;

        // Color attachments
        for (size_t i = 0; i < pass.colorAttachments.size(); ++i) {
            TextureHandle handle = pass.colorAttachments[i];
            const TextureDesc& desc = m_textureDescs[handle.index];

            VkAttachmentDescription attachment{};
            attachment.format = desc.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference ref{};
            ref.attachment = static_cast<uint32_t>(attachments.size());
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments.push_back(attachment);
            colorRefs.push_back(ref);
        }

        // Depth attachment
        if (pass.depthAttachment.isValid()) {
            const TextureDesc& desc = m_textureDescs[pass.depthAttachment.index];

            VkAttachmentDescription attachment{};
            attachment.format = desc.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            depthRef.attachment = static_cast<uint32_t>(attachments.size());
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            attachments.push_back(attachment);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
        subpass.pDepthStencilAttachment = depthRef.attachment == VK_ATTACHMENT_UNUSED ? nullptr : &depthRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &pass.vulkanRenderPass) != VK_SUCCESS) {
            std::cerr << "Failed to create render pass for: " << pass.name << std::endl;
        }
    }
}

void RenderGraph::createFramebuffers() {
    for (auto& pass : m_passes) {
        if (pass.vulkanRenderPass == VK_NULL_HANDLE) {
            continue;
        }

        std::vector<VkImageView> attachmentViews;

        for (const auto& handle : pass.colorAttachments) {
            attachmentViews.push_back(m_physicalTextures[handle.index].view);
        }

        if (pass.depthAttachment.isValid()) {
            attachmentViews.push_back(m_physicalTextures[pass.depthAttachment.index].view);
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = pass.vulkanRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachmentViews.size());
        framebufferInfo.pAttachments = attachmentViews.data();
        framebufferInfo.width = pass.viewportWidth;
        framebufferInfo.height = pass.viewportHeight;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &pass.vulkanFramebuffer) != VK_SUCCESS) {
            std::cerr << "Failed to create framebuffer for: " << pass.name << std::endl;
        }
    }
}

bool RenderGraph::allocateTexture(TextureHandle handle) {
    if (!handle.isValid() || handle.index >= m_textureDescs.size()) {
        return false;
    }

    const TextureDesc& desc = m_textureDescs[handle.index];
    PhysicalTexture& physical = m_physicalTextures[handle.index];

    // Determine Vulkan usage flags
    VkImageUsageFlags usage = toVkImageUsage(desc.usage);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = desc.format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.samples = desc.samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &physical.image) != VK_SUCCESS) {
        std::cerr << "Failed to create image: " << desc.name << std::endl;
        return false;
    }

    // Allocate memory
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, physical.image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &physical.memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate image memory: " << desc.name << std::endl;
        vkDestroyImage(m_device, physical.image, nullptr);
        physical.image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(m_device, physical.image, physical.memory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = physical.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = desc.format;

    bool isDepth = (desc.format == VK_FORMAT_D32_SFLOAT ||
                    desc.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                    desc.format == VK_FORMAT_D16_UNORM);

    viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &physical.view) != VK_SUCCESS) {
        std::cerr << "Failed to create image view: " << desc.name << std::endl;
        vkDestroyImage(m_device, physical.image, nullptr);
        vkFreeMemory(m_device, physical.memory, nullptr);
        physical.image = VK_NULL_HANDLE;
        physical.memory = VK_NULL_HANDLE;
        return false;
    }

    physical.format = desc.format;
    physical.width = desc.width;
    physical.height = desc.height;
    physical.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    physical.ownsMemory = true;

    return true;
}

bool RenderGraph::allocateBuffer(BufferHandle handle) {
    if (!handle.isValid() || handle.index >= m_bufferDescs.size()) {
        return false;
    }

    const BufferDesc& desc = m_bufferDescs[handle.index];
    PhysicalBuffer& physical = m_physicalBuffers[handle.index];

    VkBufferUsageFlags usage = toVkBufferUsage(desc.usage);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &physical.buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create buffer: " << desc.name << std::endl;
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, physical.buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &physical.memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, physical.buffer, nullptr);
        physical.buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(m_device, physical.buffer, physical.memory, 0);
    physical.size = desc.size;
    physical.ownsMemory = true;

    return true;
}

void RenderGraph::freeTexture(TextureHandle handle) {
    if (!handle.isValid() || handle.index >= m_physicalTextures.size()) {
        return;
    }

    PhysicalTexture& physical = m_physicalTextures[handle.index];
    if (!physical.ownsMemory) {
        return;
    }

    if (physical.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, physical.view, nullptr);
    }
    if (physical.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, physical.image, nullptr);
    }
    if (physical.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, physical.memory, nullptr);
    }

    physical = {};
}

void RenderGraph::freeBuffer(BufferHandle handle) {
    if (!handle.isValid() || handle.index >= m_physicalBuffers.size()) {
        return;
    }

    PhysicalBuffer& physical = m_physicalBuffers[handle.index];
    if (!physical.ownsMemory) {
        return;
    }

    if (physical.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, physical.buffer, nullptr);
    }
    if (physical.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, physical.memory, nullptr);
    }

    physical = {};
}

uint32_t RenderGraph::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

VkImageUsageFlags RenderGraph::toVkImageUsage(TextureUsage usage) {
    VkImageUsageFlags result = 0;

    if (hasFlag(usage, TextureUsage::ColorAttachment)) {
        result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (hasFlag(usage, TextureUsage::DepthAttachment)) {
        result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (hasFlag(usage, TextureUsage::ShaderRead)) {
        result |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (hasFlag(usage, TextureUsage::ShaderWrite) || hasFlag(usage, TextureUsage::Storage)) {
        result |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (hasFlag(usage, TextureUsage::TransferSrc)) {
        result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasFlag(usage, TextureUsage::TransferDst)) {
        result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    return result;
}

VkBufferUsageFlags RenderGraph::toVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags result = 0;

    if (hasFlag(usage, BufferUsage::VertexBuffer)) {
        result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::IndexBuffer)) {
        result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::UniformBuffer)) {
        result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::StorageBuffer)) {
        result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::IndirectBuffer)) {
        result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::TransferSrc)) {
        result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasFlag(usage, BufferUsage::TransferDst)) {
        result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    return result;
}

VkImageLayout RenderGraph::getOptimalLayout(TextureUsage usage) {
    if (hasFlag(usage, TextureUsage::ColorAttachment)) {
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (hasFlag(usage, TextureUsage::DepthAttachment)) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (hasFlag(usage, TextureUsage::ShaderRead)) {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (hasFlag(usage, TextureUsage::Storage)) {
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    if (hasFlag(usage, TextureUsage::Present)) {
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    if (hasFlag(usage, TextureUsage::TransferSrc)) {
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (hasFlag(usage, TextureUsage::TransferDst)) {
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    return VK_IMAGE_LAYOUT_GENERAL;
}

} // namespace ohao
