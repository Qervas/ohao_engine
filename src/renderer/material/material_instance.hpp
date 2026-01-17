#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "bindless_texture_manager.hpp"

namespace ohao {

// Forward declarations
class MaterialManager;
class BindlessTextureManager;

// Material blend mode
enum class BlendMode : uint32_t {
    Opaque = 0,
    AlphaBlend = 1,
    Additive = 2,
    Multiply = 3
};

// Material render queue
enum class RenderQueue : uint32_t {
    Background = 1000,
    Geometry = 2000,
    AlphaTest = 2450,
    Transparent = 3000,
    Overlay = 4000
};

// Material feature flags
enum class MaterialFeatures : uint32_t {
    None = 0,
    DoubleSided = 1 << 0,
    AlphaTest = 1 << 1,
    ReceiveShadows = 1 << 2,
    CastShadows = 1 << 3,
    UseNormalMap = 1 << 4,
    UseEmissive = 1 << 5,
    UseAO = 1 << 6,
    UseHeight = 1 << 7,
    ClearCoat = 1 << 8,
    Subsurface = 1 << 9,
    Anisotropy = 1 << 10,
    Transmission = 1 << 11,
    Sheen = 1 << 12
};

inline MaterialFeatures operator|(MaterialFeatures a, MaterialFeatures b) {
    return static_cast<MaterialFeatures>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline MaterialFeatures operator&(MaterialFeatures a, MaterialFeatures b) {
    return static_cast<MaterialFeatures>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(MaterialFeatures flags, MaterialFeatures flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

// Base PBR material parameters (GPU-compatible layout)
struct PBRMaterialParams {
    glm::vec4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 emissiveColor{0.0f, 0.0f, 0.0f, 1.0f};

    float roughness{0.5f};
    float metallic{0.0f};
    float ao{1.0f};
    float normalStrength{1.0f};

    float heightScale{0.05f};
    float alphaThreshold{0.5f};
    float ior{1.5f};              // Index of refraction
    float transmission{0.0f};

    // Clear coat (car paint, varnish)
    float clearCoatIntensity{0.0f};
    float clearCoatRoughness{0.0f};

    // Subsurface scattering (skin, wax)
    float subsurfaceIntensity{0.0f};
    float subsurfaceRadius{1.0f};
    glm::vec4 subsurfaceColor{1.0f, 0.2f, 0.1f, 1.0f};

    // Anisotropy (brushed metal)
    float anisotropy{0.0f};
    float anisotropyRotation{0.0f};

    // Sheen (fabric, velvet)
    float sheenIntensity{0.0f};
    float sheenRoughness{0.3f};
    glm::vec4 sheenColor{1.0f, 1.0f, 1.0f, 1.0f};

    // Texture indices (for bindless texturing)
    uint32_t albedoTexIndex{UINT32_MAX};
    uint32_t normalTexIndex{UINT32_MAX};
    uint32_t roughnessTexIndex{UINT32_MAX};
    uint32_t metallicTexIndex{UINT32_MAX};
    uint32_t aoTexIndex{UINT32_MAX};
    uint32_t emissiveTexIndex{UINT32_MAX};
    uint32_t heightTexIndex{UINT32_MAX};
    uint32_t opacityTexIndex{UINT32_MAX};

    uint32_t features{0};         // MaterialFeatures bitmask
    uint32_t padding[3];
};

static_assert(sizeof(PBRMaterialParams) % 16 == 0, "PBRMaterialParams must be 16-byte aligned");

// Material template (base material that can be instanced)
struct MaterialTemplate {
    std::string name;
    PBRMaterialParams defaultParams;
    BlendMode blendMode{BlendMode::Opaque};
    RenderQueue renderQueue{RenderQueue::Geometry};
    MaterialFeatures features{MaterialFeatures::ReceiveShadows | MaterialFeatures::CastShadows};

    // Shader permutation key
    uint32_t shaderVariant{0};
};

// Material instance (references template, can override parameters)
class MaterialInstance {
public:
    MaterialInstance(const MaterialTemplate* templ, MaterialManager* manager);
    ~MaterialInstance() = default;

    // Get GPU-ready parameters
    const PBRMaterialParams& getParams() const { return m_params; }
    PBRMaterialParams& getParams() { return m_params; }

    // Template info
    const MaterialTemplate* getTemplate() const { return m_template; }
    const std::string& getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    // Render state
    BlendMode getBlendMode() const { return m_blendMode; }
    void setBlendMode(BlendMode mode) { m_blendMode = mode; }

    RenderQueue getRenderQueue() const { return m_renderQueue; }
    void setRenderQueue(RenderQueue queue) { m_renderQueue = queue; }

    // Color properties
    void setAlbedoColor(const glm::vec3& color) { m_params.albedoColor = glm::vec4(color, m_params.albedoColor.a); }
    void setAlbedoColor(const glm::vec4& color) { m_params.albedoColor = color; }
    void setEmissiveColor(const glm::vec3& color) { m_params.emissiveColor = glm::vec4(color, 1.0f); }

    // PBR properties
    void setRoughness(float r) { m_params.roughness = glm::clamp(r, 0.0f, 1.0f); }
    void setMetallic(float m) { m_params.metallic = glm::clamp(m, 0.0f, 1.0f); }
    void setAO(float ao) { m_params.ao = glm::clamp(ao, 0.0f, 1.0f); }
    void setNormalStrength(float s) { m_params.normalStrength = s; }

    // Texture assignment
    void setAlbedoTexture(BindlessTextureHandle handle);
    void setNormalTexture(BindlessTextureHandle handle);
    void setRoughnessTexture(BindlessTextureHandle handle);
    void setMetallicTexture(BindlessTextureHandle handle);
    void setAOTexture(BindlessTextureHandle handle);
    void setEmissiveTexture(BindlessTextureHandle handle);
    void setHeightTexture(BindlessTextureHandle handle);
    void setOpacityTexture(BindlessTextureHandle handle);

    // Advanced material layers
    void setClearCoat(float intensity, float roughness);
    void setSubsurface(float intensity, float radius, const glm::vec3& color);
    void setAnisotropy(float intensity, float rotation);
    void setSheen(float intensity, float roughness, const glm::vec3& color);
    void setTransmission(float transmission, float ior = 1.5f);

    // Feature toggles
    void setDoubleSided(bool enabled);
    void setAlphaTest(bool enabled, float threshold = 0.5f);
    void setReceiveShadows(bool enabled);
    void setCastShadows(bool enabled);

    // Check if dirty (needs GPU update)
    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

    // GPU buffer offset (set by MaterialManager)
    uint32_t getBufferOffset() const { return m_bufferOffset; }
    void setBufferOffset(uint32_t offset) { m_bufferOffset = offset; }

private:
    void setFeature(MaterialFeatures feature, bool enabled);

    const MaterialTemplate* m_template{nullptr};
    MaterialManager* m_manager{nullptr};

    std::string m_name;
    PBRMaterialParams m_params;
    BlendMode m_blendMode{BlendMode::Opaque};
    RenderQueue m_renderQueue{RenderQueue::Geometry};

    uint32_t m_bufferOffset{UINT32_MAX};
    bool m_dirty{true};
};

// Material manager (handles template creation, instancing, and GPU buffer)
class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    BindlessTextureManager* textureManager,
                    uint32_t maxMaterials = 1024);
    void cleanup();

    // Template management
    MaterialTemplate* createTemplate(const std::string& name);
    MaterialTemplate* getTemplate(const std::string& name);
    const MaterialTemplate* getDefaultTemplate() const { return &m_defaultTemplate; }

    // Instance management
    MaterialInstance* createInstance(const MaterialTemplate* templ = nullptr);
    MaterialInstance* createInstance(const std::string& templateName);
    void destroyInstance(MaterialInstance* instance);

    // Batch update dirty materials to GPU
    void updateGPU();

    // Descriptor set for material buffer
    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }
    VkBuffer getMaterialBuffer() const { return m_materialBuffer; }

    // Texture manager access
    BindlessTextureManager* getTextureManager() const { return m_textureManager; }

    // Stats
    uint32_t getMaterialCount() const { return static_cast<uint32_t>(m_instances.size()); }
    uint32_t getMaxMaterials() const { return m_maxMaterials; }

private:
    bool createMaterialBuffer();
    bool createDescriptorResources();
    void updateDescriptorSet();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    BindlessTextureManager* m_textureManager{nullptr};

    // Templates
    std::unordered_map<std::string, std::unique_ptr<MaterialTemplate>> m_templates;
    MaterialTemplate m_defaultTemplate;

    // Instances
    std::vector<std::unique_ptr<MaterialInstance>> m_instances;
    std::vector<uint32_t> m_freeSlots;

    // GPU buffer
    VkBuffer m_materialBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_materialMemory{VK_NULL_HANDLE};
    void* m_mappedMemory{nullptr};

    // Descriptors
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    uint32_t m_maxMaterials{1024};
};

} // namespace ohao
