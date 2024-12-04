#include "model.hpp"
#include <cstddef>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace ohao {

std::vector<VkVertexInputBindingDescription>
Vertex::getBindingDescriptions(){
    std::vector<VkVertexInputBindingDescription> bindingDescription(1);
    bindingDescription[0].binding = 0;
    bindingDescription[0].stride = sizeof(Vertex);
    bindingDescription[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

std::vector<VkVertexInputAttributeDescription>
Vertex::getAttributeDescriptions(){
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions(4);

    // position
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, position);

    // Color
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, color);

    // Normal
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, normal);

    //TexCoord
    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, texCoord);

    return attributeDescriptions;
}

void
Model::loadFromOBJ(const std::string& filename) {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;

    std::string mtlFilename;
    std::string currentMaterial;

    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "mtllib") {
            iss >> mtlFilename;
            size_t lastSlash = filename.find_last_of("/\\");
            std::string directory = (lastSlash != std::string::npos) ?
                filename.substr(0, lastSlash + 1) : "";
            std::string fullMtlPath = directory + mtlFilename;

            std::cout << "Loading material file: " << fullMtlPath << std::endl;
            loadMTL(fullMtlPath);
        }
        else if (type == "usemtl") {
            iss >> currentMaterial;
            std::cout << "Switching to material: " << currentMaterial << std::endl;
        }
        else if (type == "v") {
            glm::vec3 pos;
            iss >> pos.x >> pos.y >> pos.z;
            positions.push_back(pos);
        }
        else if (type == "vn") {
            glm::vec3 normal;
            iss >> normal.x >> normal.y >> normal.z;
            normals.push_back(normal);
        }
        else if (type == "vt") {
            glm::vec2 texCoord;
            iss >> texCoord.x >> texCoord.y;
            texCoords.push_back(texCoord);
        }
        else if (type == "f") {
            std::string v1, v2, v3;
            iss >> v1 >> v2 >> v3;

            auto processVertex = [&](const std::string& vertex) {
                std::istringstream viss(vertex);
                std::string indexStr;
                std::vector<int> indices;

                while (std::getline(viss, indexStr, '/')) {
                    if (indexStr.empty()) {
                        indices.push_back(0);
                    } else {
                        indices.push_back(std::stoi(indexStr));
                    }
                }

                Vertex v{};
                v.position = positions[indices[0] - 1];
                if (indices.size() > 1 && indices[1] > 0) {
                    v.texCoord = texCoords[indices[1] - 1];
                }
                if (indices.size() > 2 && indices[2] > 0) {
                    v.normal = normals[indices[2] - 1];
                }

                // Set default color (will be updated in assignMaterialColors)
                v.color = glm::vec3(0.8f);
                return v;
            };

            vertices.push_back(processVertex(v1));
            vertices.push_back(processVertex(v2));
            vertices.push_back(processVertex(v3));

            // Add indices
            uint32_t baseIndex = static_cast<uint32_t>(vertices.size()) - 3;
            indices.push_back(baseIndex);
            indices.push_back(baseIndex + 1);
            indices.push_back(baseIndex + 2);

            // Store the current material for this face
            materialAssignments.push_back(currentMaterial);
            std::cout << "Added face with material: '" << currentMaterial << "'" << std::endl;
        }
    }

    std::cout << "Loaded " << vertices.size() << " vertices, "
              << indices.size() << " indices, and "
              << materialAssignments.size() << " material assignments" << std::endl;

    assignMaterialColors();
}

void
Model::loadMTL(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open material file: " + filename);
    }

    MaterialData* currentMaterial = nullptr;
    std::string line;

    std::cout << "Loading MTL file: " << filename << std::endl;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "newmtl") {
            std::string name;
            iss >> name;
            materials[name] = MaterialData();
            currentMaterial = &materials[name];
            currentMaterial->name = name;

            // Initialize default values
            currentMaterial->ambient = glm::vec3(0.2f);
            currentMaterial->diffuse = glm::vec3(0.8f);
            currentMaterial->specular = glm::vec3(0.0f);
            currentMaterial->shininess = 1.0f;
            currentMaterial->ior = 1.45f;
            currentMaterial->opacity = 1.0f;
            currentMaterial->illum = 2;

            std::cout << "Defined new material: " << name << std::endl;
        }
        else if (currentMaterial != nullptr) {
            if (token == "Ka") {
                iss >> currentMaterial->ambient.x >> currentMaterial->ambient.y >> currentMaterial->ambient.z;
                std::cout << "  Ambient: " << currentMaterial->ambient.x << " "
                         << currentMaterial->ambient.y << " "
                         << currentMaterial->ambient.z << std::endl;
            }
            else if (token == "Kd") {
                iss >> currentMaterial->diffuse.x >> currentMaterial->diffuse.y >> currentMaterial->diffuse.z;
                std::cout << "  Diffuse: " << currentMaterial->diffuse.x << " "
                         << currentMaterial->diffuse.y << " "
                         << currentMaterial->diffuse.z << std::endl;
            }
            else if (token == "Ks") {
                iss >> currentMaterial->specular.x >> currentMaterial->specular.y >> currentMaterial->specular.z;
            }
            else if (token == "Ns") {
                iss >> currentMaterial->shininess;
            }
            else if (token == "Ni") {
                iss >> currentMaterial->ior;
            }
            else if (token == "d" || token == "Tr") {
                iss >> currentMaterial->opacity;
            }
            else if (token == "illum") {
                iss >> currentMaterial->illum;
            }
            else if (token == "Ke"){
                iss >> currentMaterial->emission.x >> currentMaterial->emission.y >> currentMaterial->emission.z;
                currentMaterial->isLight = true;
            }
            else if (token == "Light_Position") {
                iss >> currentMaterial->lightPosition.x
                    >> currentMaterial->lightPosition.y
                    >> currentMaterial->lightPosition.z;
            }
            else if (token == "Light_Intensity") {
                iss >> currentMaterial->lightIntensity;
            }
        }
    }

    std::cout << "Loaded " << materials.size() << " materials" << std::endl;
}


void
Model::assignMaterialColors() {
    if (materialAssignments.empty()) {
        std::cout << "No material assignments found, using default colors" << std::endl;
        for (auto& vertex : vertices) {
            vertex.color = glm::vec3(0.8f);
        }
        return;
    }

    std::cout << "Assigning colors to " << vertices.size() << " vertices with "
              << materialAssignments.size() << " material assignments" << std::endl;

    // Use a default color for faces without material assignments
    glm::vec3 defaultColor(0.8f, 0.8f, 0.8f);

    for (size_t i = 0; i < vertices.size(); i += 3) {
        // Calculate which face this vertex belongs to
        size_t faceIndex = i / 3;

        // Check if we have a material assignment for this face
        if (faceIndex >= materialAssignments.size()) {
            std::cout << "Face " << faceIndex << " has no material assignment, using default color" << std::endl;
            for (size_t j = 0; j < 3; ++j) {
                vertices[i + j].color = defaultColor;
            }
            continue;
        }

        // Get the material name for this face
        const std::string& matName = materialAssignments[faceIndex];
        if (matName.empty()) {
            std::cout << "Face " << faceIndex << " has empty material name, using default color" << std::endl;
            for (size_t j = 0; j < 3; ++j) {
                vertices[i + j].color = defaultColor;
            }
            continue;
        }

        // Find the material
        auto matIt = materials.find(matName);
        if (matIt != materials.end()) {
            const auto& material = matIt->second;
            for (size_t j = 0; j < 3 && (i + j) < vertices.size(); ++j) {
                vertices[i + j].color = material.diffuse;
            }
            std::cout << "Face " << faceIndex << ": Assigned material '" << matName
                     << "' with color: " << material.diffuse.x << ", "
                     << material.diffuse.y << ", "
                     << material.diffuse.z << std::endl;
        } else {
            std::cout << "Face " << faceIndex << ": Material '" << matName
                     << "' not found, using default color" << std::endl;
            for (size_t j = 0; j < 3 && (i + j) < vertices.size(); ++j) {
                vertices[i + j].color = defaultColor;
            }
        }
    }

    std::cout << "Finished assigning colors to all vertices" << std::endl;
}

}//namespace
