#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include "scene/asset/model.hpp"
#include "animation/skeleton.hpp"
#include "animation/animation_clip.hpp"
#include <iostream>
#include <filesystem>

namespace ohao {

bool Model::loadFromGLTF(const std::string& filename) {
    tinygltf::Model gltfModel;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool loaded = false;
    std::string ext = filename.substr(filename.find_last_of('.'));
    if (ext == ".glb" || ext == ".GLB") {
        loaded = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filename);
    } else {
        loaded = loader.LoadASCIIFromFile(&gltfModel, &err, &warn, filename);
    }

    if (!warn.empty()) {
        std::cerr << "GLTF Warning: " << warn << std::endl;
    }
    if (!err.empty()) {
        std::cerr << "GLTF Error: " << err << std::endl;
    }
    if (!loaded) {
        std::cerr << "Failed to load GLTF: " << filename << std::endl;
        return false;
    }

    sourcePath = filename;
    vertices.clear();
    indices.clear();
    materials.clear();

    // Load materials
    std::string basePath = std::filesystem::path(filename).parent_path().string();
    for (const auto& gltfMat : gltfModel.materials) {
        MaterialData mat{};
        mat.name = gltfMat.name.empty() ? "gltf_material" : gltfMat.name;

        // PBR metallic-roughness
        const auto& pbr = gltfMat.pbrMetallicRoughness;
        mat.baseColorFactor = glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );
        mat.metallicFactor = static_cast<float>(pbr.metallicFactor);
        mat.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

        // Also set the OBJ-style diffuse from baseColor for compatibility
        mat.diffuse = glm::vec3(mat.baseColorFactor);
        mat.ambient = mat.diffuse * 0.1f;
        mat.specular = glm::vec3(0.5f);
        mat.shininess = (1.0f - mat.roughnessFactor) * 128.0f;
        mat.opacity = mat.baseColorFactor.a;

        // Texture references (store texture paths for later loading)
        if (pbr.baseColorTexture.index >= 0) {
            int texIdx = gltfModel.textures[pbr.baseColorTexture.index].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                mat.baseColorTexture = gltfModel.images[texIdx].uri;
                mat.diffuseTexture = mat.baseColorTexture;
            }
        }
        if (pbr.metallicRoughnessTexture.index >= 0) {
            int texIdx = gltfModel.textures[pbr.metallicRoughnessTexture.index].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                mat.metallicRoughnessTexture = gltfModel.images[texIdx].uri;
            }
        }

        // Normal map
        if (gltfMat.normalTexture.index >= 0) {
            int texIdx = gltfModel.textures[gltfMat.normalTexture.index].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                mat.normalTexture = gltfModel.images[texIdx].uri;
            }
        }

        // Occlusion
        if (gltfMat.occlusionTexture.index >= 0) {
            int texIdx = gltfModel.textures[gltfMat.occlusionTexture.index].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                mat.occlusionTexture = gltfModel.images[texIdx].uri;
            }
        }

        // Emissive
        if (gltfMat.emissiveTexture.index >= 0) {
            int texIdx = gltfModel.textures[gltfMat.emissiveTexture.index].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                mat.emissiveTexture = gltfModel.images[texIdx].uri;
            }
        }
        mat.emission = glm::vec3(
            static_cast<float>(gltfMat.emissiveFactor[0]),
            static_cast<float>(gltfMat.emissiveFactor[1]),
            static_cast<float>(gltfMat.emissiveFactor[2])
        );

        // Alpha mode
        if (gltfMat.alphaMode == "MASK") {
            mat.alphaMode = MaterialData::AlphaMode::MASK;
        } else if (gltfMat.alphaMode == "BLEND") {
            mat.alphaMode = MaterialData::AlphaMode::BLEND;
        } else {
            mat.alphaMode = MaterialData::AlphaMode::OPAQUE;
        }
        mat.alphaCutoff = static_cast<float>(gltfMat.alphaCutoff);
        mat.doubleSided = gltfMat.doubleSided;

        materials[mat.name] = mat;
    }

    // If no materials, create a default
    if (materials.empty()) {
        setupDefaultMaterial();
    }

    // Helper to get accessor data pointer
    auto getBufferData = [&](int accessorIndex) -> const unsigned char* {
        if (accessorIndex < 0) return nullptr;
        const auto& accessor = gltfModel.accessors[accessorIndex];
        const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
        const auto& buffer = gltfModel.buffers[bufferView.buffer];
        return buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    };

    auto getAccessorCount = [&](int accessorIndex) -> size_t {
        if (accessorIndex < 0) return 0;
        return gltfModel.accessors[accessorIndex].count;
    };

    auto getAccessorStride = [&](int accessorIndex) -> int {
        if (accessorIndex < 0) return 0;
        const auto& accessor = gltfModel.accessors[accessorIndex];
        const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
        int stride = bufferView.byteStride;
        if (stride == 0) {
            // Tightly packed
            int componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
            int numComponents = tinygltf::GetNumComponentsInType(accessor.type);
            stride = componentSize * numComponents;
        }
        return stride;
    };

    // Process all meshes
    uint32_t vertexOffset = 0;

    for (const auto& mesh : gltfModel.meshes) {
        for (const auto& primitive : mesh.primitives) {
            // Get attribute accessors
            int posAccessor = -1, normAccessor = -1, uvAccessor = -1, colorAccessor = -1;
            int tangentAccessor = -1, jointsAccessor = -1, weightsAccessor = -1;

            for (const auto& [attrName, accessorIdx] : primitive.attributes) {
                if (attrName == "POSITION") posAccessor = accessorIdx;
                else if (attrName == "NORMAL") normAccessor = accessorIdx;
                else if (attrName == "TEXCOORD_0") uvAccessor = accessorIdx;
                else if (attrName == "COLOR_0") colorAccessor = accessorIdx;
                else if (attrName == "TANGENT") tangentAccessor = accessorIdx;
                else if (attrName == "JOINTS_0") jointsAccessor = accessorIdx;
                else if (attrName == "WEIGHTS_0") weightsAccessor = accessorIdx;
            }

            if (posAccessor < 0) continue; // No positions = skip

            size_t vertexCount = getAccessorCount(posAccessor);
            const unsigned char* posData = getBufferData(posAccessor);
            const unsigned char* normData = normAccessor >= 0 ? getBufferData(normAccessor) : nullptr;
            const unsigned char* uvData = uvAccessor >= 0 ? getBufferData(uvAccessor) : nullptr;
            const unsigned char* colorData = colorAccessor >= 0 ? getBufferData(colorAccessor) : nullptr;
            const unsigned char* tangentData = tangentAccessor >= 0 ? getBufferData(tangentAccessor) : nullptr;
            const unsigned char* jointsData = jointsAccessor >= 0 ? getBufferData(jointsAccessor) : nullptr;
            const unsigned char* weightsData = weightsAccessor >= 0 ? getBufferData(weightsAccessor) : nullptr;

            int posStride = getAccessorStride(posAccessor);
            int normStride = normAccessor >= 0 ? getAccessorStride(normAccessor) : 0;
            int uvStride = uvAccessor >= 0 ? getAccessorStride(uvAccessor) : 0;
            int colorStride = colorAccessor >= 0 ? getAccessorStride(colorAccessor) : 0;
            int tangentStride = tangentAccessor >= 0 ? getAccessorStride(tangentAccessor) : 0;
            int jointsStride = jointsAccessor >= 0 ? getAccessorStride(jointsAccessor) : 0;
            int weightsStride = weightsAccessor >= 0 ? getAccessorStride(weightsAccessor) : 0;

            // Get material color
            glm::vec3 matColor(0.8f);
            if (primitive.material >= 0 && primitive.material < static_cast<int>(gltfModel.materials.size())) {
                const auto& pbr = gltfModel.materials[primitive.material].pbrMetallicRoughness;
                matColor = glm::vec3(
                    static_cast<float>(pbr.baseColorFactor[0]),
                    static_cast<float>(pbr.baseColorFactor[1]),
                    static_cast<float>(pbr.baseColorFactor[2])
                );
            }

            // Build vertices
            for (size_t i = 0; i < vertexCount; ++i) {
                Vertex v{};

                // Position
                const float* pos = reinterpret_cast<const float*>(posData + i * posStride);
                v.position = glm::vec3(pos[0], pos[1], pos[2]);

                // Normal
                if (normData) {
                    const float* norm = reinterpret_cast<const float*>(normData + i * normStride);
                    v.normal = glm::vec3(norm[0], norm[1], norm[2]);
                } else {
                    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }

                // UV
                if (uvData) {
                    const float* uv = reinterpret_cast<const float*>(uvData + i * uvStride);
                    v.texCoord = glm::vec2(uv[0], uv[1]);
                } else {
                    v.texCoord = glm::vec2(0.0f);
                }

                // Color (from vertex attribute or material)
                if (colorData) {
                    const auto& accessor = gltfModel.accessors[colorAccessor];
                    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        const float* col = reinterpret_cast<const float*>(colorData + i * colorStride);
                        v.color = glm::vec3(col[0], col[1], col[2]);
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* col = reinterpret_cast<const uint16_t*>(colorData + i * colorStride);
                        v.color = glm::vec3(col[0] / 65535.0f, col[1] / 65535.0f, col[2] / 65535.0f);
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        const uint8_t* col = colorData + i * colorStride;
                        v.color = glm::vec3(col[0] / 255.0f, col[1] / 255.0f, col[2] / 255.0f);
                    } else {
                        v.color = matColor;
                    }
                } else {
                    v.color = matColor;
                }

                // Tangent
                if (tangentData) {
                    const float* tan = reinterpret_cast<const float*>(tangentData + i * tangentStride);
                    v.tangent = glm::vec4(tan[0], tan[1], tan[2], tan[3]);
                } else {
                    v.tangent = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                }

                // Joint indices (bone indices)
                if (jointsData) {
                    const auto& accessor = gltfModel.accessors[jointsAccessor];
                    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        const uint8_t* j = jointsData + i * jointsStride;
                        v.boneIndices = glm::ivec4(j[0], j[1], j[2], j[3]);
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* j = reinterpret_cast<const uint16_t*>(jointsData + i * jointsStride);
                        v.boneIndices = glm::ivec4(j[0], j[1], j[2], j[3]);
                    }
                } else {
                    v.boneIndices = glm::ivec4(0, 0, 0, 0);
                }

                // Bone weights
                if (weightsData) {
                    const float* w = reinterpret_cast<const float*>(weightsData + i * weightsStride);
                    v.boneWeights = glm::vec4(w[0], w[1], w[2], w[3]);
                } else {
                    v.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                }

                vertices.push_back(v);
            }

            // Build indices
            if (primitive.indices >= 0) {
                const auto& accessor = gltfModel.accessors[primitive.indices];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                const unsigned char* indexData = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

                for (size_t i = 0; i < accessor.count; ++i) {
                    uint32_t index = 0;
                    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        index = reinterpret_cast<const uint16_t*>(indexData)[i];
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        index = reinterpret_cast<const uint32_t*>(indexData)[i];
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        index = indexData[i];
                    }
                    indices.push_back(vertexOffset + index);
                }
            } else {
                // No index buffer - generate sequential indices
                for (size_t i = 0; i < vertexCount; ++i) {
                    indices.push_back(vertexOffset + static_cast<uint32_t>(i));
                }
            }

            // Track material per triangle for RT texture lookup
            uint32_t matIdx = primitive.material >= 0 ? static_cast<uint32_t>(primitive.material) : 0;
            size_t numTriangles = 0;
            if (primitive.indices >= 0) {
                numTriangles = gltfModel.accessors[primitive.indices].count / 3;
            } else {
                numTriangles = vertexCount / 3;
            }
            for (size_t t = 0; t < numTriangles; t++) {
                materialPerTriangle.push_back(matIdx);
            }

            vertexOffset += static_cast<uint32_t>(vertexCount);
        }
    }

    // Dump first material's extension keys for debugging
    if (!gltfModel.materials.empty()) {
        const auto& mat0 = gltfModel.materials[0];
        std::cout << "  Mat0 extensions:";
        for (const auto& [key, val] : mat0.extensions) std::cout << " " << key;
        std::cout << std::endl;
        std::cout << "  Mat0 extras keys:";
        if (mat0.extras.IsObject()) {
            for (const auto& key : mat0.extras.Keys()) std::cout << " " << key;
        }
        std::cout << std::endl;
        // Check non-PBR texture slots
        std::cout << "  Mat0 normalTex=" << mat0.normalTexture.index
                  << " occlusionTex=" << mat0.occlusionTexture.index
                  << " emissiveTex=" << mat0.emissiveTexture.index << std::endl;
    }

    // Extract per-material base colors AND textures from GLTF materials
    for (size_t mi = 0; mi < gltfModel.materials.size(); mi++) {
        const auto& gltfMat = gltfModel.materials[mi];
        const auto& pbr = gltfMat.pbrMetallicRoughness;
        materialColors.push_back(glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.roughnessFactor)
        ));
        materialMetallic.push_back(static_cast<float>(pbr.metallicFactor));

        // Check for KHR_materials_pbrSpecularGlossiness extension (CC3 models)
        int diffuseTexIndex = pbr.baseColorTexture.index;
        if (diffuseTexIndex < 0) {
            auto extIt = gltfMat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (extIt != gltfMat.extensions.end() && extIt->second.IsObject()) {
                const auto& sg = extIt->second;
                // Get diffuse factor
                if (sg.Has("diffuseFactor") && sg.Get("diffuseFactor").IsArray()) {
                    const auto& df = sg.Get("diffuseFactor");
                    if (df.ArrayLen() >= 3) {
                        materialColors.back() = glm::vec4(
                            static_cast<float>(df.Get(0).IsNumber() ? df.Get(0).Get<double>() : 1.0),
                            static_cast<float>(df.Get(1).IsNumber() ? df.Get(1).Get<double>() : 1.0),
                            static_cast<float>(df.Get(2).IsNumber() ? df.Get(2).Get<double>() : 1.0),
                            materialColors.back().w
                        );
                    }
                }
                // Get glossiness → roughness conversion
                if (sg.Has("glossinessFactor") && sg.Get("glossinessFactor").IsNumber()) {
                    float glossiness = static_cast<float>(sg.Get("glossinessFactor").Get<double>());
                    materialColors.back().w = 1.0f - glossiness;  // roughness = 1 - glossiness
                }
                // Get diffuse texture index
                if (sg.Has("diffuseTexture") && sg.Get("diffuseTexture").IsObject()) {
                    const auto& dt = sg.Get("diffuseTexture");
                    if (dt.Has("index") && dt.Get("index").IsInt()) {
                        diffuseTexIndex = dt.Get("index").Get<int>();
                    }
                }
            }
        }

        // Extract albedo texture if present
        if (diffuseTexIndex >= 0 && diffuseTexIndex < static_cast<int>(gltfModel.textures.size())) {
            int texIdx = gltfModel.textures[diffuseTexIndex].source;
            if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                const auto& img = gltfModel.images[texIdx];
                if (!img.image.empty() && img.width > 0 && img.height > 0) {
                    TextureData td;
                    td.width = img.width;
                    td.height = img.height;
                    td.materialIndex = static_cast<int>(mi);
                    // tinygltf decodes to RGBA (component=4) or RGB (component=3)
                    if (img.component == 4) {
                        td.pixels = img.image;
                    } else if (img.component == 3) {
                        // Convert RGB → RGBA
                        td.pixels.resize(img.width * img.height * 4);
                        for (int p = 0; p < img.width * img.height; p++) {
                            td.pixels[p*4+0] = img.image[p*3+0];
                            td.pixels[p*4+1] = img.image[p*3+1];
                            td.pixels[p*4+2] = img.image[p*3+2];
                            td.pixels[p*4+3] = 255;
                        }
                    }
                    // Compute average color from texture for material base color
                    double avgR = 0, avgG = 0, avgB = 0;
                    int pixCount = td.width * td.height;
                    for (int p = 0; p < pixCount; p++) {
                        avgR += td.pixels[p*4+0];
                        avgG += td.pixels[p*4+1];
                        avgB += td.pixels[p*4+2];
                    }
                    avgR /= (pixCount * 255.0);
                    avgG /= (pixCount * 255.0);
                    avgB /= (pixCount * 255.0);
                    // Override the material base color with the texture's average color
                    materialColors.back() = glm::vec4(
                        static_cast<float>(avgR),
                        static_cast<float>(avgG),
                        static_cast<float>(avgB),
                        materialColors.back().w  // keep roughness
                    );
                    std::cout << "  Material " << mi << " (" << gltfMat.name << "): tex "
                              << td.width << "x" << td.height
                              << " avg=(" << avgR << "," << avgG << "," << avgB << ")" << std::endl;

                    materialTextureIndex.push_back(static_cast<int>(albedoTextures.size()));
                    albedoTextures.push_back(std::move(td));
                    goto diffuse_done;
                }
            }
        }
        materialTextureIndex.push_back(-1);
        diffuse_done:

        // Extract normal texture if present
        {
            int normalTexFound = -1;
            if (gltfMat.normalTexture.index >= 0 &&
                gltfMat.normalTexture.index < static_cast<int>(gltfModel.textures.size())) {
                int texIdx = gltfModel.textures[gltfMat.normalTexture.index].source;
                if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                    const auto& img = gltfModel.images[texIdx];
                    if (!img.image.empty() && img.width > 0 && img.height > 0) {
                        TextureData td;
                        td.width = img.width;
                        td.height = img.height;
                        td.materialIndex = static_cast<int>(mi);
                        if (img.component == 4) {
                            td.pixels = img.image;
                        } else if (img.component == 3) {
                            td.pixels.resize(img.width * img.height * 4);
                            for (int p = 0; p < img.width * img.height; p++) {
                                td.pixels[p*4+0] = img.image[p*3+0];
                                td.pixels[p*4+1] = img.image[p*3+1];
                                td.pixels[p*4+2] = img.image[p*3+2];
                                td.pixels[p*4+3] = 255;
                            }
                        }
                        normalTexFound = static_cast<int>(normalTextures.size());
                        normalTextures.push_back(std::move(td));
                    }
                }
            }
            materialNormalTexIndex.push_back(normalTexFound);
        }

        // Extract metallicRoughnessTexture: GLTF packs G=roughness, B=metallic
        {
            int rmTexFound = -1;
            if (pbr.metallicRoughnessTexture.index >= 0 &&
                pbr.metallicRoughnessTexture.index < static_cast<int>(gltfModel.textures.size())) {
                int texIdx = gltfModel.textures[pbr.metallicRoughnessTexture.index].source;
                if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                    const auto& img = gltfModel.images[texIdx];
                    if (!img.image.empty() && img.width > 0 && img.height > 0) {
                        // Repack: GLTF (R=AO, G=roughness, B=metallic) → standard order (R=AO, G=Roughness, B=Metallic, A=255)
                        TextureData td;
                        td.width = img.width;
                        td.height = img.height;
                        td.materialIndex = static_cast<int>(mi);
                        td.pixels.resize(img.width * img.height * 4);
                        int comp = img.component;
                        for (int p = 0; p < img.width * img.height; p++) {
                            uint8_t ao        = (comp >= 1) ? img.image[p * comp + 0] : 255;  // R channel
                            uint8_t roughness = (comp >= 2) ? img.image[p * comp + 1] : 128;  // G channel
                            uint8_t metallic  = (comp >= 3) ? img.image[p * comp + 2] : 0;    // B channel
                            td.pixels[p*4+0] = ao;
                            td.pixels[p*4+1] = roughness;
                            td.pixels[p*4+2] = metallic;
                            td.pixels[p*4+3] = 255;
                        }
                        rmTexFound = static_cast<int>(roughMetalTextures.size());
                        roughMetalTextures.push_back(std::move(td));
                    }
                }
            }
            materialRoughMetalTexIndex.push_back(rmTexFound);
        }

        // Extract emissive texture
        {
            int emTexFound = -1;
            if (gltfMat.emissiveTexture.index >= 0 &&
                gltfMat.emissiveTexture.index < static_cast<int>(gltfModel.textures.size())) {
                int texIdx = gltfModel.textures[gltfMat.emissiveTexture.index].source;
                if (texIdx >= 0 && texIdx < static_cast<int>(gltfModel.images.size())) {
                    const auto& img = gltfModel.images[texIdx];
                    if (!img.image.empty() && img.width > 0 && img.height > 0) {
                        TextureData td;
                        td.width = img.width;
                        td.height = img.height;
                        td.materialIndex = static_cast<int>(mi);
                        if (img.component == 4) {
                            td.pixels = img.image;
                        } else if (img.component == 3) {
                            td.pixels.resize(img.width * img.height * 4);
                            for (int p = 0; p < img.width * img.height; p++) {
                                td.pixels[p*4+0] = img.image[p*3+0];
                                td.pixels[p*4+1] = img.image[p*3+1];
                                td.pixels[p*4+2] = img.image[p*3+2];
                                td.pixels[p*4+3] = 255;
                            }
                        }
                        emTexFound = static_cast<int>(emissiveTextures.size());
                        emissiveTextures.push_back(std::move(td));
                    }
                }
            }
            materialEmissiveTexIndex.push_back(emTexFound);
        }

        std::cout << "  Material " << mi << " (" << gltfMat.name << "): "
                  << "diffuse=" << (materialTextureIndex.back() >= 0 ? "tex" : "color")
                  << " normal=" << (materialNormalTexIndex.back() >= 0 ? "tex" : "none")
                  << " roughMetal=" << (materialRoughMetalTexIndex.back() >= 0 ? "tex" : "scalar")
                  << " emissive=" << (materialEmissiveTexIndex.back() >= 0 ? "tex" : "none")
                  << std::endl;
    }
    if (materialColors.empty()) {
        materialColors.push_back(glm::vec4(0.8f, 0.8f, 0.8f, 0.5f));
        materialMetallic.push_back(0.0f);
        materialTextureIndex.push_back(-1);
        materialNormalTexIndex.push_back(-1);
        materialRoughMetalTexIndex.push_back(-1);
        materialEmissiveTexIndex.push_back(-1);
    }

    std::cout << "GLTF loaded: " << filename
              << " (" << vertices.size() << " vertices, " << indices.size() << " indices, "
              << materialColors.size() << " materials, "
              << albedoTextures.size() << " textures, "
              << gltfModel.images.size() << " images, "
              << gltfModel.textures.size() << " gltf textures)" << std::endl;

    // Load skins (skeleton data)
    if (!gltfModel.skins.empty()) {
        const auto& gltfSkin = gltfModel.skins[0]; // Use first skin
        skeleton = std::make_shared<Skeleton>();

        skeleton->joints.resize(gltfSkin.joints.size());

        // Parse inverse bind matrices
        std::vector<glm::mat4> inverseBindMatrices(gltfSkin.joints.size(), glm::mat4(1.0f));
        if (gltfSkin.inverseBindMatrices >= 0) {
            const auto& accessor = gltfModel.accessors[gltfSkin.inverseBindMatrices];
            const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
            const auto& buffer = gltfModel.buffers[bufferView.buffer];
            const float* matData = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

            for (size_t i = 0; i < accessor.count && i < gltfSkin.joints.size(); ++i) {
                // GLTF stores matrices in column-major order (same as GLM)
                memcpy(&inverseBindMatrices[i], matData + i * 16, sizeof(glm::mat4));
            }
        }

        // Build joint hierarchy
        // First, create a map from node index to joint index
        std::unordered_map<int, int> nodeToJoint;
        for (size_t i = 0; i < gltfSkin.joints.size(); ++i) {
            nodeToJoint[gltfSkin.joints[i]] = static_cast<int>(i);
        }

        for (size_t i = 0; i < gltfSkin.joints.size(); ++i) {
            int nodeIndex = gltfSkin.joints[i];
            const auto& node = gltfModel.nodes[nodeIndex];

            Joint& joint = skeleton->joints[i];
            joint.name = node.name.empty() ? "joint_" + std::to_string(i) : node.name;
            joint.inverseBindMatrix = inverseBindMatrices[i];

            // Find parent: walk the node hierarchy
            joint.parentIndex = -1; // Default: root
            for (size_t j = 0; j < gltfSkin.joints.size(); ++j) {
                if (j == i) continue;
                int parentNodeIndex = gltfSkin.joints[j];
                const auto& parentNode = gltfModel.nodes[parentNodeIndex];
                for (int child : parentNode.children) {
                    if (child == nodeIndex) {
                        joint.parentIndex = static_cast<int>(j);
                        break;
                    }
                }
                if (joint.parentIndex >= 0) break;
            }

            // Set local transform from node
            if (node.matrix.size() == 16) {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        joint.localTransform[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
            } else {
                glm::vec3 translation(0.0f);
                glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
                glm::vec3 scale(1.0f);

                if (node.translation.size() == 3) {
                    translation = glm::vec3(
                        static_cast<float>(node.translation[0]),
                        static_cast<float>(node.translation[1]),
                        static_cast<float>(node.translation[2]));
                }
                if (node.rotation.size() == 4) {
                    rotation = glm::quat(
                        static_cast<float>(node.rotation[3]),  // w
                        static_cast<float>(node.rotation[0]),  // x
                        static_cast<float>(node.rotation[1]),  // y
                        static_cast<float>(node.rotation[2])); // z
                }
                if (node.scale.size() == 3) {
                    scale = glm::vec3(
                        static_cast<float>(node.scale[0]),
                        static_cast<float>(node.scale[1]),
                        static_cast<float>(node.scale[2]));
                }

                joint.localTransform = glm::translate(glm::mat4(1.0f), translation) *
                                       glm::mat4_cast(rotation) *
                                       glm::scale(glm::mat4(1.0f), scale);
            }
        }

        // Compute skeleton root transform: walk non-joint ancestors of the root joint.
        // GLTF inverseBindMatrices are relative to scene root, so we need the full
        // ancestor chain above the first joint (e.g., Armature node transform).
        {
            // Find a root joint (parentIndex == -1) and trace its node ancestry
            int rootJointNodeIdx = -1;
            for (size_t i = 0; i < gltfSkin.joints.size(); ++i) {
                if (skeleton->joints[i].parentIndex < 0) {
                    rootJointNodeIdx = gltfSkin.joints[i];
                    break;
                }
            }

            if (rootJointNodeIdx >= 0) {
                // Build child→parent map for the full node tree
                std::unordered_map<int, int> nodeParent;
                for (size_t n = 0; n < gltfModel.nodes.size(); ++n) {
                    for (int child : gltfModel.nodes[n].children) {
                        nodeParent[child] = static_cast<int>(n);
                    }
                }

                // Walk up from root joint's parent, accumulating non-joint transforms
                std::vector<glm::mat4> ancestorStack;
                int cur = rootJointNodeIdx;
                while (nodeParent.count(cur)) {
                    int parentIdx = nodeParent[cur];
                    if (nodeToJoint.count(parentIdx)) break; // stop at another joint

                    const auto& pnode = gltfModel.nodes[parentIdx];
                    glm::mat4 parentLocal(1.0f);
                    if (pnode.matrix.size() == 16) {
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c)
                                parentLocal[c][r] = static_cast<float>(pnode.matrix[c * 4 + r]);
                    } else {
                        glm::vec3 t(0.0f); glm::quat rot(1,0,0,0); glm::vec3 s(1.0f);
                        if (pnode.translation.size() == 3) t = glm::vec3(pnode.translation[0], pnode.translation[1], pnode.translation[2]);
                        if (pnode.rotation.size() == 4) rot = glm::quat(float(pnode.rotation[3]), float(pnode.rotation[0]), float(pnode.rotation[1]), float(pnode.rotation[2]));
                        if (pnode.scale.size() == 3) s = glm::vec3(pnode.scale[0], pnode.scale[1], pnode.scale[2]);
                        parentLocal = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), s);
                    }
                    ancestorStack.push_back(parentLocal);
                    cur = parentIdx;
                }

                // Multiply in reverse order (outermost ancestor first)
                glm::mat4 rootXform(1.0f);
                for (int i = static_cast<int>(ancestorStack.size()) - 1; i >= 0; --i) {
                    rootXform = rootXform * ancestorStack[i];
                }
                skeleton->rootTransform = rootXform;
            }
        }

        // Compute initial joint matrices
        skeleton->computeJointMatrices();

        std::cout << "GLTF skin loaded: " << gltfSkin.joints.size() << " joints" << std::endl;
    }

    // Load animations
    for (const auto& gltfAnim : gltfModel.animations) {
        auto clip = std::make_shared<AnimationClip>();
        clip->name = gltfAnim.name.empty() ? "animation_" + std::to_string(animations.size()) : gltfAnim.name;
        clip->duration = 0.0f;

        // Build node-to-joint map from skin (needed to map animation targets to joints)
        std::unordered_map<int, int> nodeToJoint;
        if (!gltfModel.skins.empty()) {
            const auto& gltfSkin = gltfModel.skins[0];
            for (size_t i = 0; i < gltfSkin.joints.size(); ++i) {
                nodeToJoint[gltfSkin.joints[i]] = static_cast<int>(i);
            }
        }

        for (const auto& gltfChannel : gltfAnim.channels) {
            AnimationChannel channel;

            // Map node index to joint index
            auto jointIt = nodeToJoint.find(gltfChannel.target_node);
            if (jointIt == nodeToJoint.end()) continue; // Not a joint we care about
            channel.targetJoint = jointIt->second;

            // Property
            if (gltfChannel.target_path == "translation") {
                channel.property = AnimationProperty::TRANSLATION;
            } else if (gltfChannel.target_path == "rotation") {
                channel.property = AnimationProperty::ROTATION;
            } else if (gltfChannel.target_path == "scale") {
                channel.property = AnimationProperty::SCALE;
            } else {
                continue; // Unsupported property (e.g., weights for morph targets)
            }

            // Sampler data
            const auto& sampler = gltfAnim.samplers[gltfChannel.sampler];

            // Interpolation type
            if (sampler.interpolation == "STEP") {
                channel.interpolation = InterpolationType::STEP;
            } else if (sampler.interpolation == "CUBICSPLINE") {
                channel.interpolation = InterpolationType::CUBIC_SPLINE;
            } else {
                channel.interpolation = InterpolationType::LINEAR;
            }

            // Timestamps (input)
            {
                const auto& accessor = gltfModel.accessors[sampler.input];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                const float* timeData = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

                channel.timestamps.resize(accessor.count);
                for (size_t i = 0; i < accessor.count; ++i) {
                    channel.timestamps[i] = timeData[i];
                    clip->duration = std::max(clip->duration, timeData[i]);
                }
            }

            // Values (output)
            {
                const auto& accessor = gltfModel.accessors[sampler.output];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                const float* valData = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

                channel.values.resize(accessor.count);
                int components = (channel.property == AnimationProperty::ROTATION) ? 4 : 3;
                for (size_t i = 0; i < accessor.count; ++i) {
                    if (components == 4) {
                        channel.values[i] = glm::vec4(
                            valData[i * 4 + 0], valData[i * 4 + 1],
                            valData[i * 4 + 2], valData[i * 4 + 3]);
                    } else {
                        channel.values[i] = glm::vec4(
                            valData[i * 3 + 0], valData[i * 3 + 1],
                            valData[i * 3 + 2], 0.0f);
                    }
                }
            }

            clip->channels.push_back(std::move(channel));
        }

        if (!clip->channels.empty()) {
            animations.push_back(std::move(clip));
        }
    }

    if (!animations.empty()) {
        std::cout << "GLTF animations loaded: " << animations.size() << " clips" << std::endl;
    }

    std::cout << "GLTF loaded: " << filename << " (" << vertices.size() << " vertices, "
              << indices.size() << " indices, " << materials.size() << " materials)" << std::endl;

    return !vertices.empty();
}

} // namespace ohao
