#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace ohao {

class OhaoVkDevice;

class OhaoVkShaderModule {
public:
    enum class ShaderType {
        VERTEX,
        FRAGMENT,
        COMPUTE,
        GEOMETRY,
        TESSELLATION_CONTROL,
        TESSELLATION_EVALUATION
    };

    struct ShaderStage {
        VkShaderModule module{VK_NULL_HANDLE};
        ShaderType type;
        std::string entryPoint{"main"};
    };

    OhaoVkShaderModule() = default;
    ~OhaoVkShaderModule();

    bool initialize(OhaoVkDevice* device);
    void cleanup();

    // Shader loading functions
    bool createShaderModule(const std::string& name,
                          const std::string& filename,
                          ShaderType type,
                          const std::string& entryPoint = "main");

    // Get shader stage info for pipeline creation
    VkPipelineShaderStageCreateInfo getShaderStageInfo(const std::string& name) const;
    std::vector<VkPipelineShaderStageCreateInfo> getShaderStageInfos() const;

    // Resource management
    void destroyShaderModule(const std::string& name);
    void destroyAllShaderModules();

    // Getters
    const ShaderStage* getShaderStage(const std::string& name) const;
    bool hasShader(const std::string& name) const;

private:
    OhaoVkDevice* device{nullptr};
    std::unordered_map<std::string, ShaderStage> shaderModules;

    // Helper functions
    std::vector<char> readShaderFile(const std::string& filename) const;
    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    VkShaderStageFlagBits shaderTypeToVkShaderStage(ShaderType type) const;
};

} // namespace ohao
