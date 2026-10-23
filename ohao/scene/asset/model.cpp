#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "stb_image.h"
#include <vulkan/vulkan_core.h>
#include "scene/asset/model.hpp"
#include "gpu/vulkan/material.hpp"
#include "core/log.hpp"

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
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions(8);

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

    // TexCoord (UV set 0)
    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, texCoord);

    // TexCoord1 (UV set 1)
    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 4;
    attributeDescriptions[4].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[4].offset = offsetof(Vertex, texCoord1);

    // Tangent (vec4: xyz = direction, w = handedness)
    attributeDescriptions[5].binding = 0;
    attributeDescriptions[5].location = 5;
    attributeDescriptions[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[5].offset = offsetof(Vertex, tangent);

    // Bone indices (ivec4)
    attributeDescriptions[6].binding = 0;
    attributeDescriptions[6].location = 6;
    attributeDescriptions[6].format = VK_FORMAT_R32G32B32A32_SINT;
    attributeDescriptions[6].offset = offsetof(Vertex, boneIndices);

    // Bone weights (vec4)
    attributeDescriptions[7].binding = 0;
    attributeDescriptions[7].location = 7;
    attributeDescriptions[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[7].offset = offsetof(Vertex, boneWeights);

    return attributeDescriptions;
}

bool Model::loadFromOBJ(std::string_view filename) {
    try{
        sourcePath = std::string(filename);

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

        std::ifstream file{sourcePath};
        if (!file.is_open()) {
            OHAO_LOG(Error, asset, "Failed to open file: {}", sourcePath);
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string type;
            iss >> type;

            if (type == "mtllib") {
                iss >> mtlFilename;
                size_t lastSlash = sourcePath.find_last_of("/\\");
                std::string directory = (lastSlash != std::string::npos) ?
                    sourcePath.substr(0, lastSlash + 1) : "";
                std::string fullMtlPath = directory + mtlFilename;

                OHAO_LOG(Debug, asset, "Loading material file: {}", fullMtlPath);
                try {
                    loadMTL(fullMtlPath);
                } catch (const std::exception& e) {
                    OHAO_LOG(Warn, asset, "Failed to load MTL file: {}", e.what());
                    OHAO_LOG(Warn, asset, "Using default materials instead.");
                    setupDefaultMaterial();
                }
            }
            else if (type == "usemtl") {
                iss >> currentMaterial;
                if (materials.find(currentMaterial) == materials.end()) {
                    OHAO_LOG(Warn, asset, "Material '{}' not found, using default", currentMaterial);
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
                
                // Suppress per-face logging for large models
            }
        }
        if (vertices.empty() || indices.empty()) {
            OHAO_LOG(Error, asset, "No geometry data loaded from OBJ file");
            return false;
        }

        if(materials.empty()){
            setupDefaultMaterial();
        }

        assignMaterialColors();

        // Filter alpha-card geometry that lacks proper alpha textures
        // OBJ models don't embed alpha in diffuse — fur/eyelash render as opaque noise
        {
            std::unordered_map<std::string, bool> skipMats = {
                {"fur", true}, {"eyelash", true}, {"tear", true}
            };
            std::vector<uint32_t> newIndices;
            std::vector<std::string> newAssignments;
            for (size_t i = 0; i < materialAssignments.size(); i++) {
                if (skipMats.find(materialAssignments[i]) == skipMats.end()) {
                    newIndices.push_back(indices[i * 3 + 0]);
                    newIndices.push_back(indices[i * 3 + 1]);
                    newIndices.push_back(indices[i * 3 + 2]);
                    newAssignments.push_back(materialAssignments[i]);
                }
            }
            size_t removed = materialAssignments.size() - newAssignments.size();
            if (removed > 0) {
                indices = std::move(newIndices);
                materialAssignments = std::move(newAssignments);
                OHAO_LOG(Info, asset, "Filtered {} alpha-card triangles (fur/eyelash/tear)", removed);
            }
        }

        // Build RT-compatible material buffers from OBJ material system
        {
            // Find textures directory relative to model
            namespace fs = std::filesystem;
            fs::path modelDir = fs::path(sourcePath).parent_path();
            fs::path texDir = modelDir / "textures";
            if (!fs::exists(texDir)) texDir = modelDir;

            std::unordered_map<std::string, uint32_t> matNameToIdx;
            for (const auto& [name, mat] : materials) {
                uint32_t idx = static_cast<uint32_t>(materialColors.size());
                matNameToIdx[name] = idx;
                materialColors.push_back(glm::vec4(mat.diffuse, 0.7f));
                materialMetallic.push_back(0.0f);  // OBJ defaults to dielectric

                // Auto-discover diffuse texture: look for {name}_diff_*.{jpg,jpeg,png}
                int texLayerIdx = -1;
                if (fs::exists(texDir)) {
                    for (const auto& entry : fs::directory_iterator(texDir)) {
                        std::string fname = entry.path().filename().string();
                        // Match: material name + "_diff_"
                        if (fname.find(name + "_diff_") != std::string::npos) {
                            std::string texPath = entry.path().string();
                            int w, h, ch;
                            stbi_uc* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, STBI_rgb_alpha);
                            if (pixels) {
                                TextureData td;
                                td.width = w;
                                td.height = h;
                                td.materialIndex = static_cast<int>(idx);
                                td.pixels.assign(pixels, pixels + w * h * 4);
                                stbi_image_free(pixels);
                                texLayerIdx = static_cast<int>(albedoTextures.size());
                                albedoTextures.push_back(std::move(td));
                                OHAO_LOG(Debug, asset, "  OBJ Material {} ({}): tex {}x{} from {}",
                                         idx, name, w, h, fname);
                            }
                            break;
                        }
                    }
                }
                materialTextureIndex.push_back(texLayerIdx);
                if (texLayerIdx < 0) {
                    OHAO_LOG(Debug, asset, "  OBJ Material {} ({}): color=({},{},{})",
                             idx, name, mat.diffuse.r, mat.diffuse.g, mat.diffuse.b);
                }

                // 4.L v4: auto-discover normal + gloss maps next to the diff
                // map. Convention used by 3ds Max / Photoshop exports:
                //   {name}_norm_*.{jpg,png}   → tangent-space normal map
                //   {name}_gloss_*.{jpg,png}  → grayscale glossiness (1-rough)
                // Roughness packed into G channel of roughMetalTexture, R=AO=1,
                // B=metal=0 so the existing closesthit sampler (rm.g) works.
                int normalIdx = -1;
                int roughMetalIdx = -1;
                if (fs::exists(texDir)) {
                    for (const auto& entry : fs::directory_iterator(texDir)) {
                        std::string fname = entry.path().filename().string();
                        // Normal map
                        if (normalIdx < 0 && fname.find(name + "_norm_") != std::string::npos) {
                            int w, h, ch;
                            stbi_uc* pixels = stbi_load(entry.path().string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
                            if (pixels) {
                                TextureData td; td.width = w; td.height = h;
                                td.materialIndex = static_cast<int>(idx);
                                td.pixels.assign(pixels, pixels + w * h * 4);
                                stbi_image_free(pixels);
                                normalIdx = static_cast<int>(normalTextures.size());
                                normalTextures.push_back(std::move(td));
                                OHAO_LOG(Debug, asset, "  OBJ Material {} ({}): normal {}x{} from {}",
                                         idx, name, w, h, fname);
                            }
                        }
                        // Gloss map → repack as (1, 1-gloss, 0, 255) into roughMetal slot
                        if (roughMetalIdx < 0 && fname.find(name + "_gloss_") != std::string::npos) {
                            int w, h, ch;
                            stbi_uc* pixels = stbi_load(entry.path().string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
                            if (pixels) {
                                std::vector<uint8_t> packed(w * h * 4);
                                for (int i = 0; i < w * h; i++) {
                                    uint8_t gloss = pixels[i * 4 + 0];  // grayscale → R channel
                                    packed[i * 4 + 0] = 255;             // AO
                                    packed[i * 4 + 1] = 255 - gloss;     // roughness = 1 - gloss
                                    packed[i * 4 + 2] = 0;               // metallic
                                    packed[i * 4 + 3] = 255;
                                }
                                stbi_image_free(pixels);
                                TextureData td; td.width = w; td.height = h;
                                td.materialIndex = static_cast<int>(idx);
                                td.pixels = std::move(packed);
                                roughMetalIdx = static_cast<int>(roughMetalTextures.size());
                                roughMetalTextures.push_back(std::move(td));
                                OHAO_LOG(Debug, asset, "  OBJ Material {} ({}): gloss->rough {}x{} from {}",
                                         idx, name, w, h, fname);
                            }
                        }
                    }
                }
                materialNormalTexIndex.push_back(normalIdx);
                materialRoughMetalTexIndex.push_back(roughMetalIdx);
                materialEmissiveTexIndex.push_back(-1);  // no emissive auto-discovery for OBJ yet
            }
            // Per-triangle material index
            materialPerTriangle.resize(materialAssignments.size());
            for (size_t i = 0; i < materialAssignments.size(); i++) {
                auto it = matNameToIdx.find(materialAssignments[i]);
                materialPerTriangle[i] = (it != matNameToIdx.end()) ? it->second : 0;
            }
            if (materialColors.empty()) {
                materialColors.push_back(glm::vec4(0.8f, 0.8f, 0.8f, 0.5f));
                materialMetallic.push_back(0.0f);
                materialTextureIndex.push_back(-1);
            }
        }

        OHAO_LOG(Info, asset, "OBJ loaded: {} vertices, {} indices, {} materials",
                 vertices.size(), indices.size(), materialColors.size());

        return true;
    } catch (const std::exception& e) {
        OHAO_LOG(Error, asset, "Error during model import: {}", e.what());
        return false;
    }
}

uint32_t Model::getOrCreateVertex(std::string_view vertexStr, 
                                 const std::vector<glm::vec3>& positions,
                                 const std::vector<glm::vec3>& normals, 
                                 const std::vector<glm::vec2>& texCoords) {
    
    // Check if we've already created this vertex (no transparent hashers)
    auto it = vertexMap.find(std::string(vertexStr));
    if (it != vertexMap.end()) {
        return it->second;
    }
    
    // Parse the vertex string (format: "v/vt/vn" or "v//vn" or "v/vt" or "v")
    std::istringstream viss{std::string(vertexStr)};
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
    vertexMap[std::string(vertexStr)] = index;
    
    return index;
}

bool Model::loadMTL(std::string_view filename) {
    try{
        const std::string path{filename};

        std::ifstream file{path};
        if (!file.is_open()) {
            OHAO_LOG(Error, asset, "Failed to open material file: {}", path);
            return false;
        }

        MaterialData* currentMaterial = nullptr;
        std::string line;

        OHAO_LOG(Debug, asset, "Loading MTL file: {}", path);

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

                OHAO_LOG(Debug, asset, "Defined new material: {}", name);
            }
            else if (currentMaterial != nullptr) {
                if (token == "Ka") {
                    iss >> currentMaterial->ambient.x >> currentMaterial->ambient.y >> currentMaterial->ambient.z;
                    OHAO_LOG(Debug, asset, "  Ambient: {} {} {}", currentMaterial->ambient.x,
                             currentMaterial->ambient.y, currentMaterial->ambient.z);
                }
                else if (token == "Kd") {
                    iss >> currentMaterial->diffuse.x >> currentMaterial->diffuse.y >> currentMaterial->diffuse.z;
                    OHAO_LOG(Debug, asset, "  Diffuse: {} {} {}", currentMaterial->diffuse.x,
                             currentMaterial->diffuse.y, currentMaterial->diffuse.z);
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
                    OHAO_LOG(Debug, asset, "  Emission: {} {} {}", currentMaterial->emission.x,
                             currentMaterial->emission.y, currentMaterial->emission.z);
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
                    OHAO_LOG(Debug, asset, "  Diffuse texture: {}", texturePath);
                }
                else if (token == "map_Ka") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->ambientTexture = texturePath;
                    OHAO_LOG(Debug, asset, "  Ambient texture: {}", texturePath);
                }
                else if (token == "map_Ks") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->specularTexture = texturePath;
                    OHAO_LOG(Debug, asset, "  Specular texture: {}", texturePath);
                }
                else if (token == "map_Bump" || token == "bump") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->normalTexture = texturePath;
                    OHAO_LOG(Debug, asset, "  Normal texture: {}", texturePath);
                }
                else if (token == "map_d" || token == "map_disp") {
                    std::string texturePath;
                    iss >> texturePath;
                    currentMaterial->heightTexture = texturePath;
                    OHAO_LOG(Debug, asset, "  Height texture: {}", texturePath);
                }
            }
        }
        if (materials.empty()) {
            OHAO_LOG(Error, asset, "No materials loaded from MTL file");
            return false;
        }

        OHAO_LOG(Info, asset, "Successfully loaded {} materials", materials.size());
        return true;
    } catch (const std::exception& e) {
        OHAO_LOG(Error, asset, "Error during MTL import: {}", e.what());
        return false;
    }
}


void
Model::assignMaterialColors() {
    if (materialAssignments.empty()) {
        OHAO_LOG(Warn, asset, "No material assignments found, using default colors");
        for (auto& vertex : vertices) {
            vertex.color = glm::vec3(0.8f);
        }
        return;
    }

    OHAO_LOG(Debug, asset, "Assigning colors to {} vertices with {} material assignments (indexed mesh)",
             vertices.size(), materialAssignments.size());

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
            OHAO_LOG(Debug, asset, "Face {} has no material assignment, using default color", faceIndex);
            continue;
        }

        // Get the material name for this face
        const std::string& matName = materialAssignments[faceIndex];
        auto matIt = materials.find(matName.empty() ? "default" : matName);
        
        if (matIt == materials.end()) {
            OHAO_LOG(Debug, asset, "Face {:#x}: Material '{}' not found, using default color",
                     faceIndex, matName);
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
            OHAO_LOG(Debug, asset, "Face {:#x}: Assigned material '{}' with color: {}, {}, {}",
                     faceIndex, matName, materialColor.x, materialColor.y, materialColor.z);
        }
    }

    OHAO_LOG(Debug, asset, "Finished assigning colors to indexed mesh");
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
    OHAO_LOG(Debug, asset, "Created default material");
}

}//namespace
