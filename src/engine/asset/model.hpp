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

    // Texture maps
    std::string diffuseTexture;     // map_Kd
    std::string ambientTexture;     // map_Ka  
    std::string specularTexture;    // map_Ks
    std::string normalTexture;      // map_Bump or bump
    std::string heightTexture;      // map_d or map_disp
    
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

    bool loadFromOBJ(const std::string& filename);
    bool loadMTL(const std::string& filename);
    void setupDefaultMaterial();

    const std::string& getSourcePath() const { return sourcePath; }
    void setSourcePath(const std::string& path) { sourcePath = path; }

private:
    void assignMaterialColors();
    uint32_t getOrCreateVertex(const std::string& vertexStr, 
                              const std::vector<glm::vec3>& positions,
                              const std::vector<glm::vec3>& normals, 
                              const std::vector<glm::vec2>& texCoords);
    
    std::string sourcePath;
    std::unordered_map<std::string, uint32_t> vertexMap; // For vertex deduplication
};
}
