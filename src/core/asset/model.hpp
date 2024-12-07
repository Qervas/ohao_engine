#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <vulkan/vulkan_core.h>

namespace ohao{

struct Vertex{

    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 texCoord;

    static std::vector<VkVertexInputBindingDescription> getBindingDescriptions();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();

};

struct MaterialData{
    std::string name;
    glm::vec3 ambient;
    glm::vec3 diffuse;
    glm::vec3 specular;
    glm::vec3 emission;
    float shininess;
    float ior;      // Index of refraction
    float opacity;
    int illum;      // Illumination model

    bool isLight{false};
    glm::vec3 lightPosition{0.0f};
    float lightIntensity{1.0f};
};

class Model{
public:
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<std::string, MaterialData> materials;
    std::vector<std::string> materialAssignments;

    void loadFromOBJ(const std::string& filename);
    void setupDefaultMaterial();

private:
    void loadMTL(const std::string& filename);
    void assignMaterialColors();
};
}
