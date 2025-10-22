#include "shader_manager.hpp"
#include "shader_compiler.hpp"
#include "renderer/rhi/vk/ohao_vk_device.hpp"
#include "ui/components/console_widget.hpp"
#include <fstream>
#include <sstream>
#include <memory>
#include <algorithm>

namespace ohao {

// ShaderDefines implementation
std::string ShaderDefines::generateDefineString() const {
    std::ostringstream oss;
    for (const auto& [name, value] : defines) {
        oss << "#define " << name;
        if (!value.empty()) {
            oss << " " << value;
        }
        oss << "\n";
    }
    return oss.str();
}

bool ShaderDefines::operator==(const ShaderDefines& other) const {
    return defines == other.defines;
}

// ShaderVariant implementation
void ShaderVariant::calculateHash() {
    // Simple hash calculation
    hash = 0;
    for (const auto& [key, value] : defines.defines) {
        hash ^= std::hash<std::string>{}(key) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::string>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
}

// Shader implementation
Shader::Shader(const std::string& shaderName, ShaderStage shaderStage, const std::string& shaderFilePath)
    : name(shaderName), stage(shaderStage), filePath(shaderFilePath) {
    updateLastModified();
}

Shader::~Shader() {
    destroyAllVariants();
}

bool Shader::compileVariant(const ShaderDefines& defines, OhaoVkDevice* device) {
    if (!device) {
        OHAO_LOG_ERROR("Invalid device for shader compilation");
        return false;
    }
    
    // Check if variant already exists
    if (getVariant(defines) != nullptr) {
        OHAO_LOG_WARNING("Shader variant already exists for: " + name);
        return true;
    }
    
    // Read shader source
    std::string source = readShaderFile();
    if (source.empty()) {
        OHAO_LOG_ERROR("Failed to read shader source: " + name);
        return false;
    }
    
    // Get compiler and compile
    auto& compiler = ShaderCompiler::getInstance();
    
    // Convert defines to vector format
    std::vector<std::string> defineVec;
    for (const auto& [key, value] : defines.defines) {
        if (value.empty()) {
            defineVec.push_back(key);
        } else {
            defineVec.push_back(key + " " + value);
        }
    }
    
    auto result = compiler.compileGLSL(source, stage, name, defineVec);
    if (!result.success) {
        OHAO_LOG_ERROR("Shader compilation failed for " + name + ": " + result.errorMessage);
        return false;
    }
    
    // Create Vulkan shader module
    VkShaderModule module = compiler.createShaderModule(result.spirvCode, device);
    if (module == VK_NULL_HANDLE) {
        OHAO_LOG_ERROR("Failed to create shader module for: " + name);
        return false;
    }
    
    // Create and store variant
    auto variant = std::make_unique<ShaderVariant>(name + "_variant", defines);
    variant->module = module;
    variant->lastModified = sourceLastModified;
    variants.push_back(std::move(variant));
    
    OHAO_LOG("Successfully compiled shader variant: " + name);
    return true;
}

ShaderVariant* Shader::getVariant(const ShaderDefines& defines) {
    for (auto& variant : variants) {
        if (variant->defines == defines) {
            return variant.get();
        }
    }
    return nullptr;
}

const ShaderVariant* Shader::getVariant(const ShaderDefines& defines) const {
    for (const auto& variant : variants) {
        if (variant->defines == defines) {
            return variant.get();
        }
    }
    return nullptr;
}

ShaderVariant* Shader::getDefaultVariant() {
    if (variants.empty()) {
        return nullptr;
    }
    return variants[0].get();
}

bool Shader::needsRecompilation() const {
    if (!std::filesystem::exists(filePath)) {
        return false;
    }
    
    auto currentModified = std::filesystem::last_write_time(filePath);
    return currentModified > sourceLastModified;
}

void Shader::updateLastModified() {
    if (std::filesystem::exists(filePath)) {
        sourceLastModified = std::filesystem::last_write_time(filePath);
    }
}

void Shader::destroyVariant(const ShaderDefines& defines) {
    auto it = std::find_if(variants.begin(), variants.end(),
        [&defines](const std::unique_ptr<ShaderVariant>& variant) {
            return variant->defines == defines;
        });
    
    if (it != variants.end()) {
        variants.erase(it);
    }
}

void Shader::destroyAllVariants() {
    variants.clear();
}

std::string Shader::preprocessShader(const std::string& source, const ShaderDefines& defines) const {
    // Simple preprocessing - just add defines at the top
    return defines.generateDefineString() + source;
}

VkShaderModule Shader::compileShaderModule(const std::string& source, OhaoVkDevice* device) const {
    auto& compiler = ShaderCompiler::getInstance();
    
    auto result = compiler.compileGLSL(source, stage, name);
    if (!result.success) {
        OHAO_LOG_ERROR("Shader module compilation failed: " + result.errorMessage);
        return VK_NULL_HANDLE;
    }
    
    return compiler.createShaderModule(result.spirvCode, device);
}

std::string Shader::readShaderFile() const {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        OHAO_LOG_ERROR("Failed to open shader file: " + filePath);
        return "";
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

VkShaderStageFlagBits Shader::stageToVkStage() const {
    switch (stage) {
        case ShaderStage::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TESSELLATION_CONTROL: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TESSELLATION_EVALUATION: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    }
}

// ShaderManager implementation
ShaderManager::ShaderManager() = default;

ShaderManager::~ShaderManager() {
    cleanup();
}

bool ShaderManager::initialize(OhaoVkDevice* devicePtr, const std::string& shaderDirectory) {
    if (!devicePtr) {
        OHAO_LOG_ERROR("Invalid device provided to ShaderManager");
        return false;
    }
    
    device = devicePtr;
    baseShaderDirectory = shaderDirectory;
    
    // Initialize shader compiler
    auto& compiler = ShaderCompiler::getInstance();
    if (!compiler.initialize()) {
        OHAO_LOG_ERROR("Failed to initialize shader compiler");
        return false;
    }
    
    OHAO_LOG("ShaderManager initialized with base directory: " + baseShaderDirectory);
    return true;
}

void ShaderManager::cleanup() {
    computeShaders.clear();
    shaderPrograms.clear();
    shaders.clear();
    device = nullptr;
    
    // Cleanup shader compiler
    auto& compiler = ShaderCompiler::getInstance();
    compiler.cleanup();
}

std::shared_ptr<Shader> ShaderManager::loadShader(const std::string& name,
                                                  ShaderStage stage,
                                                  const std::string& relativePath) {
    // Check if shader already exists
    auto it = shaders.find(name);
    if (it != shaders.end()) {
        OHAO_LOG_WARNING("Shader " + name + " already loaded");
        return it->second;
    }
    
    // Resolve full path
    std::string fullPath = resolveShaderPath(relativePath);
    if (fullPath.empty()) {
        OHAO_LOG_ERROR("Failed to resolve shader path: " + relativePath);
        return nullptr;
    }
    
    // Create shader
    auto shader = std::make_shared<Shader>(name, stage, fullPath);
    shaders[name] = shader;
    
    OHAO_LOG("Loaded shader: " + name + " from " + relativePath);
    return shader;
}

std::shared_ptr<Shader> ShaderManager::getShader(const std::string& name) const {
    auto it = shaders.find(name);
    return (it != shaders.end()) ? it->second : nullptr;
}

void ShaderManager::setGlobalDefine(const std::string& name, const std::string& value) {
    globalDefines.addDefine(name, value);
    OHAO_LOG("Set global shader define: " + name + " = " + value);
}

void ShaderManager::removeGlobalDefine(const std::string& name) {
    globalDefines.removeDefine(name);
    OHAO_LOG("Removed global shader define: " + name);
}

void ShaderManager::addShaderSearchPath(const std::string& path) {
    searchPaths.push_back(path);
    OHAO_LOG("Added shader search path: " + path);
}

std::string ShaderManager::resolveShaderPath(const std::string& relativePath) const {
    // Add base directory to search paths if not already added
    std::vector<std::string> allPaths = searchPaths;
    if (std::find(allPaths.begin(), allPaths.end(), baseShaderDirectory) == allPaths.end()) {
        allPaths.insert(allPaths.begin(), baseShaderDirectory);
    }
    
    for (const auto& searchPath : allPaths) {
        std::filesystem::path fullPath = std::filesystem::path(searchPath) / relativePath;
        if (std::filesystem::exists(fullPath)) {
            return fullPath.string();
        }
    }
    
    OHAO_LOG_ERROR("Could not resolve shader path: " + relativePath);
    return "";
}

std::shared_ptr<ShaderProgram> ShaderManager::createShaderProgram(const std::string& name,
                                                                const std::string& vertexShader,
                                                                const std::string& fragmentShader,
                                                                const std::string& geometryShader,
                                                                const std::string& tessControlShader,
                                                                const std::string& tessEvalShader) {
    // Check if shader program already exists
    auto it = shaderPrograms.find(name);
    if (it != shaderPrograms.end()) {
        OHAO_LOG_WARNING("Shader program " + name + " already exists");
        return it->second;
    }
    
    auto program = std::make_shared<ShaderProgram>(name);
    
    // Load and attach vertex shader
    auto vertShader = loadShader(name + "_vert", ShaderStage::VERTEX, vertexShader);
    if (vertShader) {
        program->attachShader(vertShader);
    }
    
    // Load and attach fragment shader  
    auto fragShader = loadShader(name + "_frag", ShaderStage::FRAGMENT, fragmentShader);
    if (fragShader) {
        program->attachShader(fragShader);
    }
    
    // Load optional shaders
    if (!geometryShader.empty()) {
        auto geomShader = loadShader(name + "_geom", ShaderStage::GEOMETRY, geometryShader);
        if (geomShader) {
            program->attachShader(geomShader);
        }
    }
    
    if (!tessControlShader.empty()) {
        auto tcShader = loadShader(name + "_tesc", ShaderStage::TESSELLATION_CONTROL, tessControlShader);
        if (tcShader) {
            program->attachShader(tcShader);
        }
    }
    
    if (!tessEvalShader.empty()) {
        auto teShader = loadShader(name + "_tese", ShaderStage::TESSELLATION_EVALUATION, tessEvalShader);
        if (teShader) {
            program->attachShader(teShader);
        }
    }
    
    shaderPrograms[name] = program;
    OHAO_LOG("Created shader program: " + name);
    return program;
}

std::shared_ptr<ComputeShader> ShaderManager::createComputeShader(const std::string& name,
                                                                  const std::string& computeShaderPath) {
    // Check if compute shader already exists
    auto it = computeShaders.find(name);
    if (it != computeShaders.end()) {
        OHAO_LOG_WARNING("Compute shader " + name + " already exists");
        return it->second;
    }
    
    // Load the compute shader
    auto shader = loadShader(name + "_comp", ShaderStage::COMPUTE, computeShaderPath);
    if (!shader) {
        OHAO_LOG_ERROR("Failed to load compute shader: " + computeShaderPath);
        return nullptr;
    }
    
    auto computeShader = std::make_shared<ComputeShader>(name, shader);
    computeShaders[name] = computeShader;
    
    OHAO_LOG("Created compute shader: " + name);
    return computeShader;
}

std::shared_ptr<ShaderProgram> ShaderManager::getShaderProgram(const std::string& name) const {
    auto it = shaderPrograms.find(name);
    return (it != shaderPrograms.end()) ? it->second : nullptr;
}

std::shared_ptr<ComputeShader> ShaderManager::getComputeShader(const std::string& name) const {
    auto it = computeShaders.find(name);
    return (it != computeShaders.end()) ? it->second : nullptr;
}

void ShaderManager::checkForChanges() {
    if (!hotReloadEnabled) {
        return;
    }
    
    for (auto& [name, shader] : shaders) {
        if (shader->needsRecompilation()) {
            OHAO_LOG("Shader " + name + " needs recompilation");
            shader->destroyAllVariants();
            shader->updateLastModified();
        }
    }
}

void ShaderManager::recompileAll() {
    OHAO_LOG("Recompiling all shaders...");
    
    for (auto& [name, shader] : shaders) {
        shader->destroyAllVariants();
        shader->updateLastModified();
    }
    
    for (auto& [name, program] : shaderPrograms) {
        program->destroyAllPipelines();
    }
    
    for (auto& [name, computeShader] : computeShaders) {
        computeShader->destroyAllPipelines();
    }
    
    OHAO_LOG("All shaders marked for recompilation");
}

void ShaderManager::destroyShader(const std::string& name) {
    auto it = shaders.find(name);
    if (it != shaders.end()) {
        shaders.erase(it);
        OHAO_LOG("Destroyed shader: " + name);
    }
}

void ShaderManager::destroyShaderProgram(const std::string& name) {
    auto it = shaderPrograms.find(name);
    if (it != shaderPrograms.end()) {
        shaderPrograms.erase(it);
        OHAO_LOG("Destroyed shader program: " + name);
    }
}

void ShaderManager::destroyComputeShader(const std::string& name) {
    auto it = computeShaders.find(name);
    if (it != computeShaders.end()) {
        computeShaders.erase(it);
        OHAO_LOG("Destroyed compute shader: " + name);
    }
}

void ShaderManager::logStatistics() const {
    OHAO_LOG("=== Shader Manager Statistics ===");
    OHAO_LOG("Shaders: " + std::to_string(shaders.size()));
    OHAO_LOG("Shader Programs: " + std::to_string(shaderPrograms.size()));
    OHAO_LOG("Compute Shaders: " + std::to_string(computeShaders.size()));
    OHAO_LOG("Global Defines: " + std::to_string(globalDefines.defines.size()));
    OHAO_LOG("Hot Reload: " + std::string(hotReloadEnabled ? "Enabled" : "Disabled"));
    OHAO_LOG("==================================");
}

} // namespace ohao