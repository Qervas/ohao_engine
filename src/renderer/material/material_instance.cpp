#include "material_instance.hpp"
#include <iostream>
#include <cstring>

namespace ohao {

// MaterialInstance implementation

MaterialInstance::MaterialInstance(const MaterialTemplate* templ, MaterialManager* manager)
    : m_template(templ), m_manager(manager) {
    if (templ) {
        m_params = templ->defaultParams;
        m_blendMode = templ->blendMode;
        m_renderQueue = templ->renderQueue;
        m_params.features = static_cast<uint32_t>(templ->features);
    }
}

void MaterialInstance::setAlbedoTexture(BindlessTextureHandle handle) {
    m_params.albedoTexIndex = handle.index;
    m_dirty = true;
}

void MaterialInstance::setNormalTexture(BindlessTextureHandle handle) {
    m_params.normalTexIndex = handle.index;
    setFeature(MaterialFeatures::UseNormalMap, handle.valid());
    m_dirty = true;
}

void MaterialInstance::setRoughnessTexture(BindlessTextureHandle handle) {
    m_params.roughnessTexIndex = handle.index;
    m_dirty = true;
}

void MaterialInstance::setMetallicTexture(BindlessTextureHandle handle) {
    m_params.metallicTexIndex = handle.index;
    m_dirty = true;
}

void MaterialInstance::setAOTexture(BindlessTextureHandle handle) {
    m_params.aoTexIndex = handle.index;
    setFeature(MaterialFeatures::UseAO, handle.valid());
    m_dirty = true;
}

void MaterialInstance::setEmissiveTexture(BindlessTextureHandle handle) {
    m_params.emissiveTexIndex = handle.index;
    setFeature(MaterialFeatures::UseEmissive, handle.valid());
    m_dirty = true;
}

void MaterialInstance::setHeightTexture(BindlessTextureHandle handle) {
    m_params.heightTexIndex = handle.index;
    setFeature(MaterialFeatures::UseHeight, handle.valid());
    m_dirty = true;
}

void MaterialInstance::setOpacityTexture(BindlessTextureHandle handle) {
    m_params.opacityTexIndex = handle.index;
    m_dirty = true;
}

void MaterialInstance::setClearCoat(float intensity, float roughness) {
    m_params.clearCoatIntensity = glm::clamp(intensity, 0.0f, 1.0f);
    m_params.clearCoatRoughness = glm::clamp(roughness, 0.0f, 1.0f);
    setFeature(MaterialFeatures::ClearCoat, intensity > 0.0f);
    m_dirty = true;
}

void MaterialInstance::setSubsurface(float intensity, float radius, const glm::vec3& color) {
    m_params.subsurfaceIntensity = glm::clamp(intensity, 0.0f, 1.0f);
    m_params.subsurfaceRadius = glm::max(radius, 0.0f);
    m_params.subsurfaceColor = glm::vec4(color, 1.0f);
    setFeature(MaterialFeatures::Subsurface, intensity > 0.0f);
    m_dirty = true;
}

void MaterialInstance::setAnisotropy(float intensity, float rotation) {
    m_params.anisotropy = glm::clamp(intensity, -1.0f, 1.0f);
    m_params.anisotropyRotation = rotation;
    setFeature(MaterialFeatures::Anisotropy, intensity != 0.0f);
    m_dirty = true;
}

void MaterialInstance::setSheen(float intensity, float roughness, const glm::vec3& color) {
    m_params.sheenIntensity = glm::clamp(intensity, 0.0f, 1.0f);
    m_params.sheenRoughness = glm::clamp(roughness, 0.0f, 1.0f);
    m_params.sheenColor = glm::vec4(color, 1.0f);
    setFeature(MaterialFeatures::Sheen, intensity > 0.0f);
    m_dirty = true;
}

void MaterialInstance::setTransmission(float transmission, float ior) {
    m_params.transmission = glm::clamp(transmission, 0.0f, 1.0f);
    m_params.ior = glm::clamp(ior, 1.0f, 3.0f);
    setFeature(MaterialFeatures::Transmission, transmission > 0.0f);
    m_dirty = true;
}

void MaterialInstance::setDoubleSided(bool enabled) {
    setFeature(MaterialFeatures::DoubleSided, enabled);
    m_dirty = true;
}

void MaterialInstance::setAlphaTest(bool enabled, float threshold) {
    setFeature(MaterialFeatures::AlphaTest, enabled);
    m_params.alphaThreshold = threshold;
    if (enabled && m_renderQueue == RenderQueue::Geometry) {
        m_renderQueue = RenderQueue::AlphaTest;
    }
    m_dirty = true;
}

void MaterialInstance::setReceiveShadows(bool enabled) {
    setFeature(MaterialFeatures::ReceiveShadows, enabled);
    m_dirty = true;
}

void MaterialInstance::setCastShadows(bool enabled) {
    setFeature(MaterialFeatures::CastShadows, enabled);
    m_dirty = true;
}

void MaterialInstance::setFeature(MaterialFeatures feature, bool enabled) {
    if (enabled) {
        m_params.features |= static_cast<uint32_t>(feature);
    } else {
        m_params.features &= ~static_cast<uint32_t>(feature);
    }
}

// MaterialManager implementation

MaterialManager::~MaterialManager() {
    cleanup();
}

bool MaterialManager::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                  BindlessTextureManager* textureManager,
                                  uint32_t maxMaterials) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_textureManager = textureManager;
    m_maxMaterials = maxMaterials;

    // Setup default template
    m_defaultTemplate.name = "Default";
    m_defaultTemplate.defaultParams = PBRMaterialParams{};
    m_defaultTemplate.defaultParams.albedoColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    m_defaultTemplate.defaultParams.roughness = 0.5f;
    m_defaultTemplate.defaultParams.metallic = 0.0f;
    m_defaultTemplate.features = MaterialFeatures::ReceiveShadows | MaterialFeatures::CastShadows;

    // Reserve free slots
    m_freeSlots.reserve(maxMaterials);
    for (uint32_t i = 0; i < maxMaterials; ++i) {
        m_freeSlots.push_back(maxMaterials - 1 - i);
    }

    if (!createMaterialBuffer()) {
        return false;
    }

    if (!createDescriptorResources()) {
        return false;
    }

    std::cout << "MaterialManager initialized with capacity: " << maxMaterials << std::endl;
    return true;
}

void MaterialManager::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    m_instances.clear();
    m_templates.clear();

    if (m_mappedMemory) {
        vkUnmapMemory(m_device, m_materialMemory);
        m_mappedMemory = nullptr;
    }

    if (m_materialBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_materialBuffer, nullptr);
        m_materialBuffer = VK_NULL_HANDLE;
    }

    if (m_materialMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_materialMemory, nullptr);
        m_materialMemory = VK_NULL_HANDLE;
    }

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}

bool MaterialManager::createMaterialBuffer() {
    VkDeviceSize bufferSize = sizeof(PBRMaterialParams) * m_maxMaterials;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_materialBuffer) != VK_SUCCESS) {
        std::cerr << "Failed to create material buffer" << std::endl;
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_materialBuffer, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            memTypeIndex = i;
            break;
        }
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_materialMemory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate material buffer memory" << std::endl;
        return false;
    }

    vkBindBufferMemory(m_device, m_materialBuffer, m_materialMemory, 0);

    // Map persistently
    vkMapMemory(m_device, m_materialMemory, 0, bufferSize, 0, &m_mappedMemory);

    return true;
}

bool MaterialManager::createDescriptorResources() {
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create material descriptor set layout" << std::endl;
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::cerr << "Failed to create material descriptor pool" << std::endl;
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        std::cerr << "Failed to allocate material descriptor set" << std::endl;
        return false;
    }

    updateDescriptorSet();
    return true;
}

void MaterialManager::updateDescriptorSet() {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_materialBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

MaterialTemplate* MaterialManager::createTemplate(const std::string& name) {
    auto templ = std::make_unique<MaterialTemplate>();
    templ->name = name;
    templ->defaultParams = m_defaultTemplate.defaultParams;
    templ->blendMode = BlendMode::Opaque;
    templ->renderQueue = RenderQueue::Geometry;
    templ->features = MaterialFeatures::ReceiveShadows | MaterialFeatures::CastShadows;

    MaterialTemplate* ptr = templ.get();
    m_templates[name] = std::move(templ);
    return ptr;
}

MaterialTemplate* MaterialManager::getTemplate(const std::string& name) {
    auto it = m_templates.find(name);
    return it != m_templates.end() ? it->second.get() : nullptr;
}

MaterialInstance* MaterialManager::createInstance(const MaterialTemplate* templ) {
    if (m_freeSlots.empty()) {
        std::cerr << "No free material slots" << std::endl;
        return nullptr;
    }

    if (!templ) {
        templ = &m_defaultTemplate;
    }

    uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();

    auto instance = std::make_unique<MaterialInstance>(templ, this);
    instance->setBufferOffset(slot);

    MaterialInstance* ptr = instance.get();
    m_instances.push_back(std::move(instance));

    return ptr;
}

MaterialInstance* MaterialManager::createInstance(const std::string& templateName) {
    const MaterialTemplate* templ = getTemplate(templateName);
    return createInstance(templ ? templ : &m_defaultTemplate);
}

void MaterialManager::destroyInstance(MaterialInstance* instance) {
    if (!instance) return;

    uint32_t offset = instance->getBufferOffset();

    auto it = std::find_if(m_instances.begin(), m_instances.end(),
        [instance](const std::unique_ptr<MaterialInstance>& ptr) {
            return ptr.get() == instance;
        });

    if (it != m_instances.end()) {
        m_instances.erase(it);
        m_freeSlots.push_back(offset);
    }
}

void MaterialManager::updateGPU() {
    if (!m_mappedMemory) return;

    PBRMaterialParams* gpuData = static_cast<PBRMaterialParams*>(m_mappedMemory);

    for (auto& instance : m_instances) {
        if (instance->isDirty()) {
            uint32_t offset = instance->getBufferOffset();
            if (offset < m_maxMaterials) {
                gpuData[offset] = instance->getParams();
                instance->clearDirty();
            }
        }
    }
}

} // namespace ohao
