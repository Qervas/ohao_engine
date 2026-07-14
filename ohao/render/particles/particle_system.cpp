#include "particle_system.hpp"
#include "../deferred/render_pass_base.hpp"
#include <array>
#include <iostream>
#include <fstream>
#include <cstring>

namespace ohao {

struct GPUParticle {
    glm::vec4 position;
    glm::vec4 velocity;
    glm::vec4 color;
    glm::vec4 params;
};

ParticleSystem::~ParticleSystem() {
    cleanup();
}

bool ParticleSystem::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                 uint32_t maxParticles) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_maxParticles = maxParticles;

    if (!createBuffers()) return false;
    if (!createDescriptors()) return false;
    if (!createComputePipelines()) return false;

    std::cout << "ParticleSystem: Initialized with " << maxParticles << " max particles" << std::endl;
    return true;
}

void ParticleSystem::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    auto destroyPipeline = [this](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, p, nullptr); p = VK_NULL_HANDLE; }
    };
    auto destroyLayout = [this](VkPipelineLayout& l) {
        if (l != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, l, nullptr); l = VK_NULL_HANDLE; }
    };

    destroyPipeline(m_emitPipeline);
    destroyPipeline(m_updatePipeline);
    destroyPipeline(m_renderPipeline);
    destroyLayout(m_emitPipelineLayout);
    destroyLayout(m_updatePipelineLayout);
    destroyLayout(m_renderPipelineLayout);

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_computeDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_computeDescriptorLayout, nullptr);
        m_computeDescriptorLayout = VK_NULL_HANDLE;
    }
    if (m_renderDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_renderDescriptorLayout, nullptr);
        m_renderDescriptorLayout = VK_NULL_HANDLE;
    }

    auto destroyBuffer = [this](VkBuffer& b, VkDeviceMemory& m) {
        if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, b, nullptr); b = VK_NULL_HANDLE; }
        if (m != VK_NULL_HANDLE) { vkFreeMemory(m_device, m, nullptr); m = VK_NULL_HANDLE; }
    };

    destroyBuffer(m_particleBuffer, m_particleMemory);
    destroyBuffer(m_counterBuffer, m_counterMemory);
    destroyBuffer(m_indirectBuffer, m_indirectMemory);
}

bool ParticleSystem::createBuffers() {
    auto createBuffer = [this](VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags props, VkBuffer& buffer,
                                VkDeviceMemory& memory) -> bool {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, props);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buffer, memory, 0);
        return true;
    };

    // Particle buffer (SSBO)
    VkDeviceSize particleSize = sizeof(GPUParticle) * m_maxParticles;
    if (!createBuffer(particleSize,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      m_particleBuffer, m_particleMemory)) {
        return false;
    }

    // Counter buffer (host-visible for reset)
    if (!createBuffer(sizeof(uint32_t) * 4,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_counterBuffer, m_counterMemory)) {
        return false;
    }

    // Initialize counters
    uint32_t* counters;
    vkMapMemory(m_device, m_counterMemory, 0, sizeof(uint32_t) * 4, 0, (void**)&counters);
    counters[0] = 0; // aliveCount
    counters[1] = 0; // deadCount
    counters[2] = 0; // emitCount
    counters[3] = m_maxParticles; // maxParticles
    vkUnmapMemory(m_device, m_counterMemory);

    // Indirect draw buffer
    if (!createBuffer(sizeof(uint32_t) * 4,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_indirectBuffer, m_indirectMemory)) {
        return false;
    }

    return true;
}

bool ParticleSystem::createDescriptors() {
    // Compute descriptor layout: particle SSBO (0), counter SSBO (1), indirect (2)
    std::array<VkDescriptorSetLayoutBinding, 3> computeBindings{};
    computeBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(computeBindings.size());
    layoutInfo.pBindings = computeBindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_computeDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Render descriptor layout: particle SSBO (read-only, binding 0)
    VkDescriptorSetLayoutBinding renderBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                VK_SHADER_STAGE_VERTEX_BIT, nullptr};

    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &renderBinding;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_renderDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool
    std::array<VkDescriptorPoolSize, 1> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}; // 3 compute + 1 render + spare

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 2;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate compute descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_computeDescriptorLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_computeDescriptorSet) != VK_SUCCESS) {
        return false;
    }

    // Allocate render descriptor set
    allocInfo.pSetLayouts = &m_renderDescriptorLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_renderDescriptorSet) != VK_SUCCESS) {
        return false;
    }

    // Write compute descriptors
    VkDescriptorBufferInfo particleBufInfo{m_particleBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo counterBufInfo{m_counterBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo indirectBufInfo{m_indirectBuffer, 0, VK_WHOLE_SIZE};

    std::array<VkWriteDescriptorSet, 4> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_computeDescriptorSet, 0, 0,
                 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &particleBufInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_computeDescriptorSet, 1, 0,
                 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &counterBufInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_computeDescriptorSet, 2, 0,
                 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &indirectBufInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_renderDescriptorSet, 0, 0,
                 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &particleBufInfo, nullptr};

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    return true;
}

bool ParticleSystem::createComputePipelines() {
    auto createComputePipeline = [this](const std::string& shaderPath, VkDescriptorSetLayout layout,
                                         uint32_t pushConstSize, VkPipeline& pipeline,
                                         VkPipelineLayout& pipelineLayout) -> bool {
        VkShaderModule shader = loadShaderModule(shaderPath);
        if (shader == VK_NULL_HANDLE) return false;

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = shader;
        stageInfo.pName = "main";

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = pushConstSize;

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &layout;
        layoutInfo.pushConstantRangeCount = pushConstSize > 0 ? 1 : 0;
        layoutInfo.pPushConstantRanges = pushConstSize > 0 ? &pushRange : nullptr;

        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, shader, nullptr);
            return false;
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = pipelineLayout;

        VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        vkDestroyShaderModule(m_device, shader, nullptr);
        return result == VK_SUCCESS;
    };

    // Emit push constant struct size
    struct EmitPushConstants {
        glm::vec4 emitPosition;
        glm::vec4 emitDirection;
        glm::vec4 velocityRange;
        glm::vec4 lifetimeRange;
        glm::vec4 colorStart;
        glm::vec4 colorEnd;
        float time;
        uint32_t emitCountThisFrame;
        uint32_t particleType;
        uint32_t pad;
    };

    struct UpdatePushConstants {
        float deltaTime;
        float gravity;
        float drag;
        float pad;
    };

    std::string basePath = RenderPassBase::getShaderBasePath();

    if (!createComputePipeline(basePath + "particles_particle_emit.comp.spv",
                                m_computeDescriptorLayout, sizeof(EmitPushConstants),
                                m_emitPipeline, m_emitPipelineLayout)) {
        std::cerr << "ParticleSystem: Failed to create emit pipeline" << std::endl;
        return false;
    }

    if (!createComputePipeline(basePath + "particles_particle_update.comp.spv",
                                m_computeDescriptorLayout, sizeof(UpdatePushConstants),
                                m_updatePipeline, m_updatePipelineLayout)) {
        std::cerr << "ParticleSystem: Failed to create update pipeline" << std::endl;
        return false;
    }

    return true;
}

bool ParticleSystem::initRenderPipeline(VkRenderPass renderPass) {
    return createRenderPipeline(renderPass);
}

bool ParticleSystem::createRenderPipeline(VkRenderPass renderPass) {
    if (renderPass == VK_NULL_HANDLE) return false;

    std::string basePath = RenderPassBase::getShaderBasePath();

    VkShaderModule vertShader = loadShaderModule(basePath + "particles_particle_render.vert.spv");
    VkShaderModule fragShader = loadShaderModule(basePath + "particles_particle_render.frag.spv");

    if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
        if (vertShader) vkDestroyShaderModule(m_device, vertShader, nullptr);
        if (fragShader) vkDestroyShaderModule(m_device, fragShader, nullptr);
        std::cerr << "ParticleSystem: Failed to load render shaders" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";

    // No vertex input (vertices generated in shader from SSBO data)
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // Billboard quads are double-sided
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test (read-only, no depth write for transparent particles)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // Particles don't write depth
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Alpha blending (additive for bright particles like sparks/muzzle flash)
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // Dynamic state
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Push constants for render
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * 2; // viewProj + cameraRight + cameraUp

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_renderDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_renderPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_renderPipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                 nullptr, &m_renderPipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "ParticleSystem: Failed to create render pipeline" << std::endl;
        return false;
    }

    std::cout << "ParticleSystem: Render pipeline created" << std::endl;
    return true;
}

void ParticleSystem::emit(VkCommandBuffer cmd, const ParticleEmitterConfig& config, float time) {
    if (m_emitPipeline == VK_NULL_HANDLE) return;

    struct EmitPushConstants {
        glm::vec4 emitPosition;
        glm::vec4 emitDirection;
        glm::vec4 velocityRange;
        glm::vec4 lifetimeRange;
        glm::vec4 colorStart;
        glm::vec4 colorEnd;
        float time;
        uint32_t emitCountThisFrame;
        uint32_t particleType;
        uint32_t pad;
    } pc{};

    pc.emitPosition = glm::vec4(config.position, 0.0f);
    pc.emitDirection = glm::vec4(config.direction, config.spreadAngle);
    pc.velocityRange = glm::vec4(config.minSpeed, config.maxSpeed, 0.0f, 0.0f);
    pc.lifetimeRange = glm::vec4(config.minLifetime, config.maxLifetime, config.startSize, config.endSize);
    pc.colorStart = config.colorStart;
    pc.colorEnd = config.colorEnd;
    pc.time = time;
    pc.emitCountThisFrame = config.emitCount;
    pc.particleType = static_cast<uint32_t>(config.type);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_emitPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_emitPipelineLayout,
                            0, 1, &m_computeDescriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_emitPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t groupCount = (config.emitCount + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier for particle buffer
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &barrier, 0, nullptr, 0, nullptr);
}

void ParticleSystem::update(VkCommandBuffer cmd, float deltaTime) {
    if (m_updatePipeline == VK_NULL_HANDLE) return;

    // Reset indirect draw buffer instance count
    uint32_t* indirect;
    vkMapMemory(m_device, m_indirectMemory, 0, sizeof(uint32_t) * 4, 0, (void**)&indirect);
    indirect[0] = 6; // vertexCount (6 vertices per billboard quad)
    indirect[1] = 0; // instanceCount (will be atomically incremented by update shader)
    indirect[2] = 0; // firstVertex
    indirect[3] = 0; // firstInstance
    vkUnmapMemory(m_device, m_indirectMemory);

    struct UpdatePushConstants {
        float deltaTime;
        float gravity;
        float drag;
        float pad;
    } pc{};

    pc.deltaTime = deltaTime;
    pc.gravity = 9.81f;
    pc.drag = 0.1f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_updatePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_updatePipelineLayout,
                            0, 1, &m_computeDescriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_updatePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t groupCount = (m_maxParticles + 255) / 256;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier for render pass
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ParticleSystem::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                             const glm::vec3& cameraRight, const glm::vec3& cameraUp) {
    if (m_renderPipeline == VK_NULL_HANDLE) return;

    struct RenderPushConstants {
        glm::mat4 viewProj;
        glm::vec3 cameraRight;
        float pad1;
        glm::vec3 cameraUp;
        float pad2;
    } pc{};

    pc.viewProj = viewProj;
    pc.cameraRight = cameraRight;
    pc.cameraUp = cameraUp;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_renderPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_renderPipelineLayout,
                            0, 1, &m_renderDescriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_renderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(pc), &pc);

    // Draw using indirect buffer (instance count set by update shader)
    vkCmdDrawIndirect(cmd, m_indirectBuffer, 0, 1, 0);
}

// Preset configurations
ParticleEmitterConfig ParticleSystem::presetMuzzleFlash(const glm::vec3& pos, const glm::vec3& dir) {
    ParticleEmitterConfig config;
    config.position = pos;
    config.direction = dir;
    config.spreadAngle = 0.3f;
    config.minSpeed = 8.0f;
    config.maxSpeed = 15.0f;
    config.minLifetime = 0.05f;
    config.maxLifetime = 0.15f;
    config.startSize = 0.05f;
    config.endSize = 0.01f;
    config.colorStart = glm::vec4(1.0f, 0.8f, 0.3f, 1.0f);
    config.colorEnd = glm::vec4(1.0f, 0.4f, 0.1f, 0.0f);
    config.gravity = 0.0f;
    config.drag = 5.0f;
    config.emitCount = 16;
    config.type = ParticleType::MUZZLE_FLASH;
    return config;
}

ParticleEmitterConfig ParticleSystem::presetImpactSpark(const glm::vec3& pos, const glm::vec3& normal) {
    ParticleEmitterConfig config;
    config.position = pos;
    config.direction = normal;
    config.spreadAngle = 1.2f;
    config.minSpeed = 2.0f;
    config.maxSpeed = 8.0f;
    config.minLifetime = 0.2f;
    config.maxLifetime = 0.6f;
    config.startSize = 0.03f;
    config.endSize = 0.005f;
    config.colorStart = glm::vec4(1.0f, 0.9f, 0.5f, 1.0f);
    config.colorEnd = glm::vec4(1.0f, 0.5f, 0.1f, 0.0f);
    config.gravity = 9.81f;
    config.drag = 0.5f;
    config.emitCount = 24;
    config.type = ParticleType::IMPACT_SPARK;
    return config;
}

ParticleEmitterConfig ParticleSystem::presetExplosion(const glm::vec3& pos) {
    ParticleEmitterConfig config;
    config.position = pos;
    config.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    config.spreadAngle = 3.14159f; // full sphere
    config.minSpeed = 3.0f;
    config.maxSpeed = 12.0f;
    config.minLifetime = 0.5f;
    config.maxLifetime = 2.0f;
    config.startSize = 0.2f;
    config.endSize = 0.5f;
    config.colorStart = glm::vec4(1.0f, 0.6f, 0.1f, 1.0f);
    config.colorEnd = glm::vec4(0.3f, 0.1f, 0.05f, 0.0f);
    config.gravity = 2.0f;
    config.drag = 1.0f;
    config.emitCount = 128;
    config.type = ParticleType::EXPLOSION;
    return config;
}

ParticleEmitterConfig ParticleSystem::presetSmoke(const glm::vec3& pos) {
    ParticleEmitterConfig config;
    config.position = pos;
    config.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    config.spreadAngle = 0.4f;
    config.minSpeed = 0.5f;
    config.maxSpeed = 2.0f;
    config.minLifetime = 1.0f;
    config.maxLifetime = 4.0f;
    config.startSize = 0.1f;
    config.endSize = 0.5f;
    config.colorStart = glm::vec4(0.5f, 0.5f, 0.5f, 0.6f);
    config.colorEnd = glm::vec4(0.3f, 0.3f, 0.3f, 0.0f);
    config.gravity = -0.5f; // slight upward buoyancy
    config.drag = 2.0f;
    config.emitCount = 8;
    config.type = ParticleType::SMOKE;
    return config;
}

ParticleEmitterConfig ParticleSystem::presetWaterSplash(const glm::vec3& pos, const glm::vec3& dir) {
    ParticleEmitterConfig config;
    config.position    = pos;
    config.direction   = dir;
    config.spreadAngle = 1.2f;    // upward spray cone
    config.minSpeed    = 1.5f;
    config.maxSpeed    = 4.0f;
    config.minLifetime = 0.3f;
    config.maxLifetime = 0.9f;
    config.startSize   = 0.04f;   // water droplet — small
    config.endSize     = 0.02f;
    config.colorStart  = glm::vec4(0.55f, 0.75f, 1.0f, 0.9f);  // water-blue
    config.colorEnd    = glm::vec4(0.7f,  0.85f, 1.0f, 0.0f);  // fade to white
    config.gravity     = -6.0f;   // arcs up then falls
    config.drag        = 0.5f;
    config.emitCount   = 32;
    config.type        = ParticleType::WATER_SPLASH;
    return config;
}

uint32_t ParticleSystem::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

VkShaderModule ParticleSystem::loadShaderModule(std::string_view path) {
    const std::string pathStr(path);
    std::ifstream file(pathStr, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "ParticleSystem: Failed to open shader: " << pathStr << std::endl;
        return VK_NULL_HANDLE;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = buffer.size(),
        .pCode = reinterpret_cast<const uint32_t*>(buffer.data()),
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

} // namespace ohao
