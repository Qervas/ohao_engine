#include <cstddef>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "engine/asset/model.hpp"
#include "renderer/material/material.hpp"

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

bool Model::loadFromOBJ(const std::string& filename) {
    try{
        sourcePath = filename;

        vertices.clear();
        indices.clear();
        materials.clear();
        materialAssignments.clear();
        vertexMap.clear(); // Clear vertex deduplication map
        
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec2> texCoords;

        std::string mtlFilename;
        std::string currentMaterial;

        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return false;
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
                try {
                    loadMTL(fullMtlPath);
                } catch (const std::exception& e) {
                    std::cout << "Warning: Failed to load MTL file: " << e.what() << std::endl;
                    std::cout << "Using default materials instead." << std::endl;
                    setupDefaultMaterial();
                }
            }
            else if (type == "usemtl") {
                iss >> currentMaterial;
                if (materials.find(currentMaterial) == materials.end()) {
                    std::cout << "Warning: Material '" << currentMaterial << "' not found, using default" << std::endl;
                    currentMaterial = "default";
                }
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
                std::vector<std::string> faceVertices;
                std::string vertex;
                
                // Read all vertices in this face (could be triangle or quad)
                while (iss >> vertex) {
                    faceVertices.push_back(vertex);
                }
                
                // Process each vertex in the face and get/create indices
                std::vector<uint32_t> faceIndices;
                for (const auto& vertexStr : faceVertices) {
                    uint32_t index = getOrCreateVertex(vertexStr, positions, normals, texCoords);
                    faceIndices.push_back(index);
                }
                
                // Triangulate the face (handles both triangles and quads)
                if (faceIndices.size() == 3) {
                    // Triangle - direct add
                    indices.push_back(faceIndices[0]);
                    indices.push_back(faceIndices[1]);
                    indices.push_back(faceIndices[2]);
                    
                    materialAssignments.push_back(currentMaterial);
                } else if (faceIndices.size() == 4) {
                    // Quad - split into two triangles (0,1,2) and (0,2,3)
                    // First triangle
                    indices.push_back(faceIndices[0]);
                    indices.push_back(faceIndices[1]);
                    indices.push_back(faceIndices[2]);
                    materialAssignments.push_back(currentMaterial);
                    
                    // Second triangle
                    indices.push_back(faceIndices[0]);
                    indices.push_back(faceIndices[2]);
                    indices.push_back(faceIndices[3]);
                    materialAssignments.push_back(currentMaterial);
                } else if (faceIndices.size() > 4) {
                    // N-gon - fan triangulation from first vertex
                    for (size_t i = 1; i < faceIndices.size() - 1; ++i) {
                        indices.push_back(faceIndices[0]);
                        indices.push_back(faceIndices[i]);
                        indices.push_back(faceIndices[i + 1]);
                        materialAssignments.push_back(currentMaterial);
                    }
                }
                
                // Only log occasionally for very large models to reduce console spam
                if ((faceIndices.size() >= 4) || (faceVertices.size() % 1000 == 0)) {
                    std::cout << "Added face with " << faceIndices.size() << " vertices, material: '" << currentMaterial << "'" << std::endl;
                }
            }
        }
        if (vertices.empty() || indices.empty()) {
            std::cerr << "No geometry data loaded from OBJ file" << std::endl;
            return false;
        }

        if(materials.empty()){
            setupDefaultMaterial();
        }

        assignMaterialColors();

        std::cout << "Successfully loaded " << vertices.size() << " vertices, "
                  << indices.size() << " indices, and "
                  << materialAssignments.size() << " material assignments" << std::endl;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error during model import: " << e.what() << std::endl;
        return false;
    }
}

uint32_t Model::getOrCreateVertex(const std::string& vertexStr, 
                                 const std::vector<glm::vec3>& positions,
                                 const std::vector<glm::vec3>& normals, 
                                 const std::vector<glm::vec2>& texCoords) {
    
    // Check if we've already created this vertex
    auto it = vertexMap.find(vertexStr);
    if (it != vertexMap.end()) {
        return it->second;
    }
    
    // Parse the vertex string (format: "v/vt/vn" or "v//vn" or "v/vt" or "v")
    std::istringstream viss(vertexStr);
    std::string indexStr;
    std::vector<int> vertexIndices;

    while (std::getline(viss, indexStr, '/')) {
        if (indexStr.empty()) {
            vertexIndices.push_back(0);
        } else {
            vertexIndices.push_back(std::stoi(indexStr));
        }
    }

    // Create the vertex
    Vertex vertex{};
    
    // Position (required)
    if (!vertexIndices.empty() && vertexIndices[0] > 0) {
        vertex.position = positions[vertexIndices[0] - 1];
    }
    
    // Texture coordinates (optional)
    if (vertexIndices.size() > 1 && vertexIndices[1] > 0 && vertexIndices[1] <= texCoords.size()) {
        vertex.texCoord = texCoords[vertexIndices[1] - 1];
    } else {
        vertex.texCoord = glm::vec2(0.0f, 0.0f);
    }
    
    // Normal (optional)
    if (vertexIndices.size() > 2 && vertexIndices[2] > 0 && vertexIndices[2] <= normals.size()) {
        vertex.normal = normals[vertexIndices[2] - 1];
    } else {
        // Generate a default normal if none provided
        vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // Set default color (will be updated by material assignment)
    vertex.color = glm::vec3(0.8f);
    
    // Add vertex to our list and map
    uint32_t index = static_cast<uint32_t>(vertices.size());
    vertices.push_back(vertex);
    vertexMap[vertexStr] = index;
    
    return index;
}

bool Model::loadMTL(const std::string& filename) {
    try{

        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open material file: " << filename << std::endl;
            return false;
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
                    currentMaterial->isLight = glm::length(currentMaterial->emission) > 0.0f;
                    std::cout << "  Emission: " << currentMaterial->emission.x << " "
                            << currentMaterial->emission.y << " "
                            << currentMaterial->emission.z << std::endl;
                }
                else if (token == "Light_Position") {
                    iss >> currentMaterial->lightPosition.x
                        >> currentMaterial->lightPosition.y
                        >> currentMaterial->lightPosition.z;
                }
                else if (token == "Light_Intensity") {
                    iss >> currentMaterial->lightIntensity;
                }
                // Texture maps
                else if (token == "map_Kd") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->diffuseTexture = texturePath;
                    std::cout << "  Diffuse texture: " << texturePath << std::endl;
                }
                else if (token == "map_Ka") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->ambientTexture = texturePath;
                    std::cout << "  Ambient texture: " << texturePath << std::endl;
                }
                else if (token == "map_Ks") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->specularTexture = texturePath;
                    std::cout << "  Specular texture: " << texturePath << std::endl;
                }
                else if (token == "map_Bump" || token == "bump") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->normalTexture = texturePath;
                    std::cout << "  Normal texture: " << texturePath << std::endl;
                }
                else if (token == "map_d" || token == "map_disp") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->heightTexture = texturePath;
                    std::cout << "  Height texture: " << texturePath << std::endl;
                }
            }
        }
        if (materials.empty()) {
            std::cerr << "No materials loaded from MTL file" << std::endl;
            return false;
        }

        std::cout << "Successfully loaded " << materials.size() << " materials" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error during MTL import: " << e.what() << std::endl;
        return false;
    }
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
              << materialAssignments.size() << " material assignments (indexed mesh)" << std::endl;

    // Initialize all vertices with default color
    glm::vec3 defaultColor(0.8f, 0.8f, 0.8f);
    for (auto& vertex : vertices) {
        vertex.color = defaultColor;
    }

    // Create a map to track which material each vertex should use
    // If a vertex is shared by multiple faces with different materials,
    // we'll use the first material encountered
    std::vector<bool> vertexAssigned(vertices.size(), false);

    // Process each triangle (3 indices per triangle)
    for (size_t i = 0; i < indices.size(); i += 3) {
        size_t faceIndex = i / 3;
        
        // Check if we have a material assignment for this face
        if (faceIndex >= materialAssignments.size()) {
            std::cout << "Face " << faceIndex << " has no material assignment, using default color" << std::endl;
            continue;
        }

        // Get the material name for this face
        const std::string& matName = materialAssignments[faceIndex];
        auto matIt = materials.find(matName.empty() ? "default" : matName);
        
        if (matIt == materials.end()) {
            std::cout << "Face " << std::hex << faceIndex << std::dec 
                     << ": Material '" << matName << "' not found, using default color" << std::endl;
            continue;
        }
        
        const MaterialData& material = matIt->second;
        glm::vec3 materialColor = material.diffuse;
        
        // Assign color to the three vertices of this face
        // Only assign if vertex hasn't been assigned a color yet
        for (int j = 0; j < 3; j++) {
            uint32_t vertexIndex = indices[i + j];
            if (vertexIndex < vertices.size() && !vertexAssigned[vertexIndex]) {
                vertices[vertexIndex].color = materialColor;
                vertexAssigned[vertexIndex] = true;
            }
        }
        
        // Only log occasionally for large models to reduce console spam
        if (faceIndex % 1000 == 0 || faceIndex < 10) {
            std::cout << "Face " << std::hex << faceIndex << std::dec 
                     << ": Assigned material '" << matName << "' with color: " 
                     << materialColor.x << ", " << materialColor.y << ", " << materialColor.z << std::endl;
        }
    }
    
    std::cout << "Finished assigning colors to indexed mesh" << std::endl;
}

void Model::setupDefaultMaterial() {
    MaterialData defaultMaterial;
    defaultMaterial.name = "default";
    defaultMaterial.ambient = glm::vec3(0.2f);
    defaultMaterial.diffuse = glm::vec3(0.8f);
    defaultMaterial.specular = glm::vec3(0.5f);
    defaultMaterial.shininess = 32.0f;
    defaultMaterial.ior = 1.45f;
    defaultMaterial.opacity = 1.0f;
    defaultMaterial.illum = 2;
    defaultMaterial.isLight = false;

    materials["default"] = defaultMaterial;
    std::cout << "Created default material" << std::endl;
}

}//namespace
