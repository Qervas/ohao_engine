#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <filesystem>
#include <chrono>
#include <glm/glm.hpp>

namespace ohao {

class OhaoVkDevice;

// Forward declarations
struct ShaderVariant;
class ShaderProgram;
class ComputeShader;

// Shader types supported by the system
enum class ShaderStage {
    VERTEX,
    FRAGMENT, 
    GEOMETRY,
    TESSELLATION_CONTROL,
    TESSELLATION_EVALUATION,
    COMPUTE
};

// Shader compilation defines/macros
struct ShaderDefines {
    std::unordered_map<std::string, std::string> defines;
    
    void addDefine(const std::string& name, const std::string& value = "") {
        defines[name] = value;
    }
    
    void removeDefine(const std::string& name) {
        defines.erase(name);
    }
    
    bool hasDefine(const std::string& name) const {
        return defines.find(name) != defines.end();
    }
    
    std::string generateDefineString() const;
    bool operator==(const ShaderDefines& other) const;
    bool operator!=(const ShaderDefines& other) const { return !(*this == other); }
};

// Shader variant - different compiled versions of the same shader
struct ShaderVariant {
    std::string name;
    ShaderDefines defines;
    VkShaderModule module{VK_NULL_HANDLE};
    std::string entryPoint{"main"};
    std::filesystem::file_time_type lastModified;
    
    // Hash for quick lookup
    size_t hash{0};
    
    ShaderVariant(const std::string& variantName, const ShaderDefines& variantDefines)
        : name(variantName), defines(variantDefines) {
        calculateHash();
    }
    
    void calculateHash();
    bool isValid() const { return module != VK_NULL_HANDLE; }
};

// Individual shader (vertex, fragment, etc.)
class Shader {
public:
    Shader(const std::string& name, ShaderStage stage, const std::string& filePath);
    ~Shader();
    
    // Variant management
    bool compileVariant(const ShaderDefines& defines, OhaoVkDevice* device);
    ShaderVariant* getVariant(const ShaderDefines& defines);
    const ShaderVariant* getVariant(const ShaderDefines& defines) const;
    ShaderVariant* getDefaultVariant();
    
    // File watching for hot reload
    bool needsRecompilation() const;
    void updateLastModified();
    
    // Getters
    const std::string& getName() const { return name; }
    ShaderStage getStage() const { return stage; }
    const std::string& getFilePath() const { return filePath; }
    const std::vector<std::unique_ptr<ShaderVariant>>& getVariants() const { return variants; }
    
    // Cleanup
    void destroyVariant(const ShaderDefines& defines);
    void destroyAllVariants();
    
private:
    std::string name;
    ShaderStage stage;
    std::string filePath;
    std::filesystem::file_time_type sourceLastModified;
    std::vector<std::unique_ptr<ShaderVariant>> variants;
    
    // Compilation
    std::string preprocessShader(const std::string& source, const ShaderDefines& defines) const;
    VkShaderModule compileShaderModule(const std::string& source, OhaoVkDevice* device) const;
    
    // File operations
    std::string readShaderFile() const;
    VkShaderStageFlagBits stageToVkStage() const;
};

// Graphics shader program (vertex + fragment + optional geometry/tessellation)
class ShaderProgram {
public:
    struct PipelineInfo {
        VkPipelineLayout layout{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
        ShaderDefines defines;
    };
    
    ShaderProgram(const std::string& name);
    ~ShaderProgram();
    
    // Shader attachment
    void attachShader(std::shared_ptr<Shader> shader);
    void detachShader(ShaderStage stage);
    
    // Pipeline creation and management
    bool createPipeline(const ShaderDefines& defines, 
                       VkPipelineLayout layout,
                       VkRenderPass renderPass,
                       VkExtent2D extent,
                       OhaoVkDevice* device);
    
    VkPipeline getPipeline(const ShaderDefines& defines) const;
    bool hasPipeline(const ShaderDefines& defines) const;
    
    // Validation
    bool isComplete() const; // Has vertex + fragment at minimum
    
    // Hot reload support
    bool needsRecompilation() const;
    void recompileIfNeeded(OhaoVkDevice* device);
    
    // Getters
    const std::string& getName() const { return name; }
    std::shared_ptr<Shader> getShader(ShaderStage stage) const;
    
    // Cleanup
    void destroyPipeline(const ShaderDefines& defines);
    void destroyAllPipelines();
    
private:
    std::string name;
    std::unordered_map<ShaderStage, std::shared_ptr<Shader>> shaders;
    std::vector<std::unique_ptr<PipelineInfo>> pipelines;
    
    std::vector<VkPipelineShaderStageCreateInfo> createShaderStages(const ShaderDefines& defines) const;
};

// Compute shader program
class ComputeShader {
public:
    struct PipelineInfo {
        VkPipelineLayout layout{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
        ShaderDefines defines;
        glm::uvec3 workGroupSize{1, 1, 1};
    };
    
    ComputeShader(const std::string& name, std::shared_ptr<Shader> computeShader);
    ~ComputeShader();
    
    // Pipeline creation
    bool createPipeline(const ShaderDefines& defines,
                       VkPipelineLayout layout,
                       OhaoVkDevice* device);
    
    VkPipeline getPipeline(const ShaderDefines& defines) const;
    
    // Dispatch helpers
    void dispatch(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1) const;
    void dispatchIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset) const;
    
    // Work group size management
    void setWorkGroupSize(uint32_t x, uint32_t y = 1, uint32_t z = 1);
    glm::uvec3 getWorkGroupSize() const { return workGroupSize; }
    
    // Hot reload support
    bool needsRecompilation() const;
    void recompileIfNeeded(OhaoVkDevice* device);
    
    // Getters
    const std::string& getName() const { return name; }
    std::shared_ptr<Shader> getShader() const { return computeShader; }
    
    // Cleanup
    void destroyPipeline(const ShaderDefines& defines);
    void destroyAllPipelines();
    
private:
    std::string name;
    std::shared_ptr<Shader> computeShader;
    glm::uvec3 workGroupSize{8, 8, 1}; // Default work group size
    std::vector<std::unique_ptr<PipelineInfo>> pipelines;
};

// Main shader manager
class ShaderManager {
public:
    ShaderManager();
    ~ShaderManager();
    
    bool initialize(OhaoVkDevice* device, const std::string& shaderDirectory = "shaders/");
    void cleanup();
    
    // Shader loading and creation
    std::shared_ptr<Shader> loadShader(const std::string& name, 
                                      ShaderStage stage, 
                                      const std::string& relativePath);
    
    std::shared_ptr<ShaderProgram> createShaderProgram(const std::string& name,
                                                       const std::string& vertexShader,
                                                       const std::string& fragmentShader,
                                                       const std::string& geometryShader = "",
                                                       const std::string& tessControlShader = "",
                                                       const std::string& tessEvalShader = "");
    
    std::shared_ptr<ComputeShader> createComputeShader(const std::string& name,
                                                       const std::string& computeShaderPath);
    
    // Access existing resources
    std::shared_ptr<Shader> getShader(const std::string& name) const;
    std::shared_ptr<ShaderProgram> getShaderProgram(const std::string& name) const;
    std::shared_ptr<ComputeShader> getComputeShader(const std::string& name) const;
    
    // Hot reloading
    void enableHotReload(bool enable) { hotReloadEnabled = enable; }
    void checkForChanges(); // Call this periodically to check for file changes
    void recompileAll(); // Force recompilation of all shaders
    
    // Global defines (applied to all shaders)
    void setGlobalDefine(const std::string& name, const std::string& value = "");
    void removeGlobalDefine(const std::string& name);
    const ShaderDefines& getGlobalDefines() const { return globalDefines; }
    
    // Shader directory management
    void addShaderSearchPath(const std::string& path);
    std::string resolveShaderPath(const std::string& relativePath) const;
    
    // Statistics and debugging
    size_t getShaderCount() const { return shaders.size(); }
    size_t getShaderProgramCount() const { return shaderPrograms.size(); }
    size_t getComputeShaderCount() const { return computeShaders.size(); }
    void logStatistics() const;
    
    // Cleanup operations
    void destroyShader(const std::string& name);
    void destroyShaderProgram(const std::string& name);
    void destroyComputeShader(const std::string& name);
    
private:
    OhaoVkDevice* device{nullptr};
    std::string baseShaderDirectory;
    std::vector<std::string> searchPaths;
    
    // Resource storage
    std::unordered_map<std::string, std::shared_ptr<Shader>> shaders;
    std::unordered_map<std::string, std::shared_ptr<ShaderProgram>> shaderPrograms;
    std::unordered_map<std::string, std::shared_ptr<ComputeShader>> computeShaders;
    
    // Global settings
    ShaderDefines globalDefines;
    bool hotReloadEnabled{false};
    
    // Helper functions
    std::string resolveShaderPathInternal(const std::string& relativePath) const;
    void updateSearchPaths();
};

} // namespace ohao