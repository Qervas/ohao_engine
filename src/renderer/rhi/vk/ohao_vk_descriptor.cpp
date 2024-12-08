#include "ohao_vk_descriptor.hpp"
#include "ohao_vk_device.hpp"
#include <iostream>
#include <memory>
#include <ohao_vk_buffer.hpp>

namespace ohao {

OhaoVkDescriptor::~OhaoVkDescriptor() {
    cleanup();
}

void OhaoVkDescriptor::cleanup() {
    if (device) {
        device->waitIdle();
        for (auto set : descriptorSets) {
            if (set != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device->getDevice(), pool, 1, &set);
            }
        }
        descriptorSets.clear();
        for (auto set : imageDescriptorSets) {
            if (set != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device->getDevice(), pool, 1, &set);
            }
        }
        imageDescriptorSets.clear();

        if (pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device->getDevice(), pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device->getDevice(), layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
        if (imageSamplerLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device->getDevice(), imageSamplerLayout, nullptr);
            imageSamplerLayout = VK_NULL_HANDLE;
        }
        device = nullptr;
    }
}

bool OhaoVkDescriptor::initialize(OhaoVkDevice* devicePtr, uint32_t maxSetsCount) {
    device = devicePtr;
    maxSets = maxSetsCount;

    if (!createSetLayout() || !createCombinedImageSamplerLayout()){
        std::cerr << "Failed to create descriptor layouts!" << std::endl;
        return false;
    }
    if (!createPool()) {
        std::cerr << "Failed to create descriptor pools!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkDescriptor::createSetLayout() {
    if (layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device->getDevice(), layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboLayoutBinding;

    if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor set layout!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkDescriptor::createPool() {
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device->getDevice(), pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    std::vector<VkDescriptorPoolSize> poolSizes(2);

    // For uniform buffers
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = maxSets;

    // For combined image samplers
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = maxSets;  // Adjust count as needed

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = maxSets * 2;  // Total number of sets we might allocate
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(device->getDevice(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor pool!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkDescriptor::createDescriptorSets(
    const std::vector<std::unique_ptr<OhaoVkBuffer>>& uniformBuffers,
    VkDeviceSize bufferSize){
    std::vector<VkDescriptorSetLayout> layouts(maxSets, layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = maxSets;
    allocInfo.pSetLayouts = layouts.data();

    descriptorSets.resize(maxSets);
    if (vkAllocateDescriptorSets(device->getDevice(), &allocInfo, descriptorSets.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate descriptor sets!" << std::endl;
        return false;
    }

    for (size_t i = 0; i < maxSets; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers[i]->getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = bufferSize;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device->getDevice(), 1, &descriptorWrite, 0, nullptr);
    }

    return true;
}

void OhaoVkDescriptor::updateDescriptorSet(
    uint32_t index,
    const OhaoVkBuffer& buffer,
    VkDeviceSize size,
    VkDeviceSize offset){

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer.getBuffer();
    bufferInfo.offset = offset;
    bufferInfo.range = size;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSets[index];
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device->getDevice(), 1, &descriptorWrite, 0, nullptr);
}

bool OhaoVkDescriptor::createCombinedImageSamplerLayout() {
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;

    if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &imageSamplerLayout) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool OhaoVkDescriptor::createCombinedImageSamplerPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;  // Adjust if needed

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;  // Adjust if needed
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    return vkCreateDescriptorPool(device->getDevice(), &poolInfo, nullptr, &pool) == VK_SUCCESS;
}

VkDescriptorSet OhaoVkDescriptor::allocateImageDescriptor(VkImageView imageView, VkSampler sampler) {
    if (imageSamplerLayout == VK_NULL_HANDLE) {
        std::cerr << "Image sampler layout is null!" << std::endl;
        return VK_NULL_HANDLE;
    }

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &imageSamplerLayout;

    if (vkAllocateDescriptorSets(device->getDevice(), &allocInfo, &descriptorSet) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    imageDescriptorSets.push_back(descriptorSet);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = imageView;
    imageInfo.sampler = sampler;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device->getDevice(), 1, &descriptorWrite, 0, nullptr);

    return descriptorSet;
}

} // namespace ohao
