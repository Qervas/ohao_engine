#include "ohao_vk_shader_module.hpp"
#include "ohao_vk_device.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ohao {

OhaoVkShaderModule::~OhaoVkShaderModule() {
    cleanup();
}

bool OhaoVkShaderModule::initialize(OhaoVkDevice* devicePtr) {
    if (!devicePtr) {
        std::cerr << "Null device provided to OhaoVkShaderModule::initialize" << std::endl;
        return false;
    }
    device = devicePtr;
    return true;
}

void OhaoVkShaderModule::cleanup() {
    destroyAllShaderModules();
}

bool OhaoVkShaderModule::createShaderModule(
    const std::string& name,
    const std::string& filename,
    ShaderType type,
    const std::string& entryPoint)
{
    try {
        auto code = readShaderFile(filename);
        VkShaderModule shaderModule = createShaderModule(code);

        ShaderStage shaderStage{};
        shaderStage.module = shaderModule;
        shaderStage.type = type;
        shaderStage.entryPoint = entryPoint;

        // Store the shader module
        auto [it, inserted] = shaderModules.insert({name, shaderStage});
        if (!inserted) {
            std::cerr << "Shader with name '" << name << "' already exists" << std::endl;
            vkDestroyShaderModule(device->getDevice(), shaderModule, nullptr);
            return false;
        }

        std::cout << "Successfully created shader module: " << name << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to create shader module: " << e.what() << std::endl;
        return false;
    }
}

VkPipelineShaderStageCreateInfo
OhaoVkShaderModule::getShaderStageInfo(const std::string& name) const {
    auto it = shaderModules.find(name);
    if (it == shaderModules.end()) {
        throw std::runtime_error("Shader module not found: " + name);
    }

    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = shaderTypeToVkShaderStage(it->second.type);
    shaderStageInfo.module = it->second.module;
    shaderStageInfo.pName = it->second.entryPoint.c_str();
    return shaderStageInfo;
}

std::vector<VkPipelineShaderStageCreateInfo>
OhaoVkShaderModule::getShaderStageInfos() const {
    std::vector<VkPipelineShaderStageCreateInfo> stageInfos;
    stageInfos.reserve(shaderModules.size());

    for (const auto& [name, shader] : shaderModules) {
        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = shaderTypeToVkShaderStage(shader.type);
        stageInfo.module = shader.module;
        stageInfo.pName = shader.entryPoint.c_str();
        stageInfos.push_back(stageInfo);
    }

    return stageInfos;
}

void OhaoVkShaderModule::destroyShaderModule(const std::string& name) {
    auto it = shaderModules.find(name);
    if (it != shaderModules.end()) {
        if (it->second.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device->getDevice(), it->second.module, nullptr);
        }
        shaderModules.erase(it);
    }
}

void OhaoVkShaderModule::destroyAllShaderModules() {
    for (const auto& [name, shader] : shaderModules) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device->getDevice(), shader.module, nullptr);
        }
    }
    shaderModules.clear();
}

const OhaoVkShaderModule::ShaderStage*
OhaoVkShaderModule::getShaderStage(const std::string& name) const {
    auto it = shaderModules.find(name);
    return (it != shaderModules.end()) ? &it->second : nullptr;
}

bool OhaoVkShaderModule::hasShader(const std::string& name) const {
    return shaderModules.find(name) != shaderModules.end();
}

std::vector<char> OhaoVkShaderModule::readShaderFile(const std::string& filename) const {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + filename);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

VkShaderModule OhaoVkShaderModule::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device->getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }

    return shaderModule;
}

VkShaderStageFlagBits
OhaoVkShaderModule::shaderTypeToVkShaderStage(ShaderType type) const {
    switch (type) {
        case ShaderType::VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderType::FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderType::COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderType::GEOMETRY:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderType::TESSELLATION_CONTROL:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderType::TESSELLATION_EVALUATION:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        default:
            throw std::runtime_error("Unknown shader type");
    }
}

} // namespace ohao
