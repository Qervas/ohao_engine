#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace ohao {

class OhaoVkDevice;
enum class ShaderStage;

// Shader compilation result
struct ShaderCompilationResult {
    bool success{false};
    std::vector<uint32_t> spirvCode;
    std::string errorMessage;
    std::string warningMessage;
    std::vector<std::string> includedFiles; // For dependency tracking
};

// Shader compiler class
class ShaderCompiler {
public:
    static ShaderCompiler& getInstance() {
        static ShaderCompiler instance;
        return instance;
    }
    
    ~ShaderCompiler();
    
    // Initialize the compiler (call once at startup)
    bool initialize();
    void cleanup();
    
    // Compile GLSL source to SPIR-V
    ShaderCompilationResult compileGLSL(const std::string& source,
                                       ShaderStage stage,
                                       const std::string& filename = "shader.glsl",
                                       const std::vector<std::string>& defines = {},
                                       const std::vector<std::string>& includePaths = {});
    
    // Compile HLSL source to SPIR-V (optional)
    ShaderCompilationResult compileHLSL(const std::string& source,
                                       ShaderStage stage,
                                       const std::string& entryPoint = "main",
                                       const std::string& filename = "shader.hlsl");
    
    // Create Vulkan shader module from SPIR-V
    VkShaderModule createShaderModule(const std::vector<uint32_t>& spirvCode, 
                                     OhaoVkDevice* device) const;
    
    // Utility functions
    bool validateSPIRV(const std::vector<uint32_t>& spirvCode) const;
    std::string disassembleSPIRV(const std::vector<uint32_t>& spirvCode) const;
    
    // Shader reflection (get inputs, outputs, uniforms, etc.)
    struct ShaderReflection {
        struct Variable {
            std::string name;
            uint32_t location{0};
            uint32_t binding{0};
            uint32_t set{0};
            std::string type;
            size_t size{0};
        };
        
        std::vector<Variable> inputs;
        std::vector<Variable> outputs;
        std::vector<Variable> uniforms;
        std::vector<Variable> storageBuffers;
        std::vector<Variable> images;
        std::vector<Variable> samplers;
        glm::uvec3 computeWorkGroupSize{1, 1, 1}; // For compute shaders
    };
    
    ShaderReflection reflectShader(const std::vector<uint32_t>& spirvCode) const;
    
    // Settings
    void setOptimizationLevel(int level) { optimizationLevel = level; }
    void setGenerateDebugInfo(bool enable) { generateDebugInfo = enable; }
    void setWarningsAsErrors(bool enable) { warningsAsErrors = enable; }
    
private:
    ShaderCompiler() = default;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;
    
    bool initialized{false};
    int optimizationLevel{0}; // 0 = no optimization, 1 = size, 2 = performance
    bool generateDebugInfo{true};
    bool warningsAsErrors{false};
    
    // Helper functions
    std::string shaderStageToString(ShaderStage stage) const;
    void addBuiltinDefines(std::vector<std::string>& defines, ShaderStage stage) const;
    std::string preprocessSource(const std::string& source, 
                                const std::vector<std::string>& defines,
                                const std::vector<std::string>& includePaths) const;
};

} // namespace ohao