#include "renderer_impl.hpp"
#include "scene/asset/model.hpp"

namespace ohao {

VkShaderModule VulkanRenderer::loadShaderModule(std::string_view filepath) {
    // Search multiple paths for shader SPVs
    const std::string pathStr(filepath);
    const auto slash = filepath.find_last_of("/\\");
    const std::string filename = std::string(filepath.substr(slash == std::string_view::npos ? 0 : slash + 1));
    std::vector<std::string> searchPaths = {
        pathStr,
        "build/shaders/" + filename,
        "build/Release/bin/shaders/" + filename,
    };
    std::ifstream file;
    for (const auto& p : searchPaths) {
        file.open(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) break;
    }
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << pathStr << std::endl;
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
        std::cerr << "Failed to create shader module!" << std::endl;
        return VK_NULL_HANDLE;
    }

    std::cout << "Loaded shader: " << pathStr << std::endl;
    return shaderModule;
}

} // namespace ohao
