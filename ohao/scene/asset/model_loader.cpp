// Unified Model Loader — routes to the best backend per format.
// FBX → ufbx, GLB/GLTF → Assimp, OBJ → native.

#include "model_loader.hpp"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_STATIC
#include "stb_image.h"

#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cstring>

namespace ohao {

// === Helpers ===

static glm::mat4 toGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );
}
static glm::vec3 toGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
static glm::quat toGlm(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

std::string ModelLoader::getExtension(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = std::tolower(c);
    return ext;
}

// Mesh repair: merge vertices at the same position and recalculate smooth normals.
// AI-generated meshes (Meshy, etc.) export "triangle soup" where every triangle has 3 unique
// vertices. This makes smooth normals impossible — each vertex only sees 1 face.
// Fix: group vertices by position, accumulate face normals across all triangles at each position,
// then write back area-weighted smooth normals. Same as Blender's "Shade Smooth" on import.
static void repairMeshNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    if (indices.size() < 3 || vertices.empty()) return;

    // Detect triangle soup: if vertexCount == indexCount, no vertices are shared
    bool isTriangleSoup = (vertices.size() == indices.size());

    // Spatial hash: quantize positions to group vertices at the same location
    // Use a grid cell size based on mesh bounds
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v.position);
        bmax = glm::max(bmax, v.position);
    }
    float meshSize = glm::length(bmax - bmin);
    float cellSize = meshSize * 1e-5f;  // ~0.001% of mesh size
    if (cellSize < 1e-7f) cellSize = 1e-7f;
    float invCell = 1.0f / cellSize;

    // Hash position → list of vertex indices at that position
    auto posHash = [&](const glm::vec3& p) -> uint64_t {
        int64_t x = int64_t(std::floor(p.x * invCell));
        int64_t y = int64_t(std::floor(p.y * invCell));
        int64_t z = int64_t(std::floor(p.z * invCell));
        // FNV-1a hash
        uint64_t h = 14695981039346656037ULL;
        h ^= uint64_t(x); h *= 1099511628211ULL;
        h ^= uint64_t(y); h *= 1099511628211ULL;
        h ^= uint64_t(z); h *= 1099511628211ULL;
        return h;
    };

    // Map: position hash → accumulated smooth normal
    std::unordered_map<uint64_t, glm::vec3> smoothNormals;
    smoothNormals.reserve(vertices.size() / 2);

    // Accumulate area-weighted face normals at each unique position
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3& p0 = vertices[indices[i]].position;
        const glm::vec3& p1 = vertices[indices[i+1]].position;
        const glm::vec3& p2 = vertices[indices[i+2]].position;

        glm::vec3 faceN = glm::cross(p1 - p0, p2 - p0);
        // Cross product magnitude = 2x triangle area → area weighting is automatic

        for (uint32_t k = 0; k < 3; k++) {
            uint64_t h = posHash(vertices[indices[i+k]].position);
            smoothNormals[h] += faceN;
        }
    }

    // Normalize accumulated normals
    for (auto& [h, n] : smoothNormals) {
        float len = glm::length(n);
        if (len > 1e-8f) n /= len;
        else n = glm::vec3(0, 1, 0);
    }

    // Write back smooth normals to all vertices
    uint32_t changed = 0;
    for (auto& v : vertices) {
        uint64_t h = posHash(v.position);
        auto it = smoothNormals.find(h);
        if (it != smoothNormals.end()) {
            if (glm::dot(v.normal, it->second) < 0.95f) changed++;
            v.normal = it->second;
        }
    }

    size_t uniquePositions = smoothNormals.size();
    if (isTriangleSoup || changed > vertices.size() / 10) {
        std::cout << "[ModelLoader] Mesh repair: " << vertices.size() << " vertices → "
                  << uniquePositions << " unique positions, "
                  << changed << " normals smoothed" << std::endl;
    }
}

// === Main Entry Point ===

std::shared_ptr<Model> ModelLoader::load(const std::string& path) {
    std::string ext = getExtension(path);

    std::shared_ptr<Model> model;
    if (ext == "obj") {
        model = loadOBJ(path);
    } else if (ext == "fbx") {
        // Try ufbx first (correct rotation orders, embedded textures)
        model = loadFBX(path);
        // Retry through Assimp for better mesh processing
        // (JoinIdenticalVertices, GenSmoothNormals, FixInfacingNormals fix AI-generated meshes)
        if (model) {
            auto assimpModel = loadAssimp(path);
            if (assimpModel && !assimpModel->vertices.empty()) {
                std::cout << "[ModelLoader] FBX: using Assimp geometry (better mesh cleanup)" << std::endl;
                // Keep ufbx textures if Assimp didn't find any (ufbx extracts embedded textures better)
                if (assimpModel->albedoTextures.empty() && !model->albedoTextures.empty()) {
                    assimpModel->albedoTextures = std::move(model->albedoTextures);
                    assimpModel->normalTextures = std::move(model->normalTextures);
                    assimpModel->roughMetalTextures = std::move(model->roughMetalTextures);
                    assimpModel->emissiveTextures = std::move(model->emissiveTextures);
                    assimpModel->materialTextureIndex = std::move(model->materialTextureIndex);
                    assimpModel->materialNormalTexIndex = std::move(model->materialNormalTexIndex);
                    assimpModel->materialRoughMetalTexIndex = std::move(model->materialRoughMetalTexIndex);
                    assimpModel->materialEmissiveTexIndex = std::move(model->materialEmissiveTexIndex);
                    std::cout << "[ModelLoader] Kept ufbx textures (" << assimpModel->albedoTextures.size() << " albedo)" << std::endl;
                }
                model = assimpModel;
            }
        }
    } else {
        // GLB, GLTF, DAE, etc → Assimp
        model = loadAssimp(path);
    }

    // Post-load: fix inverted normals (handles broken AI-generated exports)
    if (model && !model->vertices.empty()) {
        repairMeshNormals(model->vertices, model->indices);
    }

    return model;
}

// === OBJ Loader (native) ===

std::shared_ptr<Model> ModelLoader::loadOBJ(const std::string& path) {
    auto model = std::make_shared<Model>();
    if (model->loadFromOBJ(path)) return model;
    return nullptr;
}

// === FBX Loader (ufbx) ===

std::shared_ptr<Model> ModelLoader::loadFBX(const std::string& path) {
    auto model = std::make_shared<Model>();
    if (model->loadFromFBX(path)) return model;
    return nullptr;
}

// === Assimp Loader (GLB, GLTF, Collada, etc.) ===

// Load texture from Assimp material (embedded or file)
static int loadAssimpTexture(const aiScene* scene, const aiMaterial* mat,
                              aiTextureType type, const std::string& modelDir,
                              std::vector<Model::TextureData>& textures) {
    if (mat->GetTextureCount(type) == 0) return -1;

    aiString texPath;
    mat->GetTexture(type, 0, &texPath);
    std::string pathStr = texPath.C_Str();

    Model::TextureData td;

    // Embedded texture
    if (!pathStr.empty()) {
        const aiTexture* embTex = scene->GetEmbeddedTexture(texPath.C_Str());
        if (embTex) {
            if (embTex->mHeight == 0) {
                int w, h, ch;
                uint8_t* px = stbi_load_from_memory(
                    reinterpret_cast<const uint8_t*>(embTex->pcData),
                    embTex->mWidth, &w, &h, &ch, 4);
                if (px) {
                    td.width = w; td.height = h;
                    td.pixels.assign(px, px + w * h * 4);
                    stbi_image_free(px);
                    int idx = static_cast<int>(textures.size());
                    textures.push_back(std::move(td));
                    return idx;
                }
            }
        }
    }

    // File texture
    std::vector<std::string> candidates = {
        pathStr,
        modelDir + "/" + pathStr,
        modelDir + "/" + std::filesystem::path(pathStr).filename().string(),
        modelDir + "/../textures/" + std::filesystem::path(pathStr).filename().string(),
    };
    for (const auto& c : candidates) {
        if (!std::filesystem::exists(c)) continue;
        int w, h, ch;
        uint8_t* px = stbi_load(c.c_str(), &w, &h, &ch, 4);
        if (px) {
            td.width = w; td.height = h;
            td.pixels.assign(px, px + w * h * 4);
            stbi_image_free(px);
            int idx = static_cast<int>(textures.size());
            textures.push_back(std::move(td));
            return idx;
        }
    }
    return -1;
}

std::shared_ptr<Model> ModelLoader::loadAssimp(const std::string& path) {
    Assimp::Importer importer;

    unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_LimitBoneWeights |
        aiProcess_FlipUVs |
        aiProcess_FindDegenerates |
        aiProcess_SortByPType |
        aiProcess_ImproveCacheLocality |
        aiProcess_FixInfacingNormals;

    const aiScene* scene = importer.ReadFile(path, flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::cerr << "[ModelLoader] Assimp error: " << importer.GetErrorString() << std::endl;
        return nullptr;
    }

    auto model = std::make_shared<Model>();
    model->setSourcePath(path);
    std::string modelDir = std::filesystem::path(path).parent_path().string();

    std::cout << "[ModelLoader] Assimp: " << scene->mNumMeshes << " meshes, "
              << scene->mNumMaterials << " materials, "
              << scene->mNumTextures << " textures, "
              << scene->mNumAnimations << " animations" << std::endl;

    // --- Materials ---
    uint32_t numMats = scene->mNumMaterials;
    model->materialColors.resize(numMats, glm::vec4(0.8f, 0.8f, 0.8f, 0.5f));
    model->materialMetallic.resize(numMats, 0.0f);
    model->materialTextureIndex.resize(numMats, -1);
    model->materialNormalTexIndex.resize(numMats, -1);
    model->materialRoughMetalTexIndex.resize(numMats, -1);
    model->materialEmissiveTexIndex.resize(numMats, -1);

    for (unsigned m = 0; m < numMats; m++) {
        const aiMaterial* mat = scene->mMaterials[m];
        aiString matName; mat->Get(AI_MATKEY_NAME, matName);

        aiColor3D color(0.8f, 0.8f, 0.8f);
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, color);

        float roughness = 0.5f, metallic = 0.0f;
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
        mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);

        // Fallback: shininess → roughness
        float shininess = 0.0f;
        if (roughness == 0.5f && mat->Get(AI_MATKEY_SHININESS, shininess) == aiReturn_SUCCESS) {
            roughness = 1.0f - glm::clamp(shininess / 1000.0f, 0.0f, 1.0f);
        }

        model->materialColors[m] = glm::vec4(color.r, color.g, color.b, roughness);
        model->materialMetallic[m] = metallic;

        // Textures
        model->materialTextureIndex[m] = loadAssimpTexture(scene, mat,
            aiTextureType_DIFFUSE, modelDir, model->albedoTextures);
        if (model->materialTextureIndex[m] < 0)
            model->materialTextureIndex[m] = loadAssimpTexture(scene, mat,
                aiTextureType_BASE_COLOR, modelDir, model->albedoTextures);

        model->materialNormalTexIndex[m] = loadAssimpTexture(scene, mat,
            aiTextureType_NORMALS, modelDir, model->normalTextures);
        if (model->materialNormalTexIndex[m] < 0)
            model->materialNormalTexIndex[m] = loadAssimpTexture(scene, mat,
                aiTextureType_HEIGHT, modelDir, model->normalTextures);

        // Roughness/metallic texture (GLTF stores as aiTextureType_UNKNOWN with metallic-roughness)
        model->materialRoughMetalTexIndex[m] = loadAssimpTexture(scene, mat,
            aiTextureType_UNKNOWN, modelDir, model->roughMetalTextures);
        if (model->materialRoughMetalTexIndex[m] < 0)
            model->materialRoughMetalTexIndex[m] = loadAssimpTexture(scene, mat,
                aiTextureType_METALNESS, modelDir, model->roughMetalTextures);
        if (model->materialRoughMetalTexIndex[m] < 0)
            model->materialRoughMetalTexIndex[m] = loadAssimpTexture(scene, mat,
                aiTextureType_DIFFUSE_ROUGHNESS, modelDir, model->roughMetalTextures);

        model->materialEmissiveTexIndex[m] = loadAssimpTexture(scene, mat,
            aiTextureType_EMISSIVE, modelDir, model->emissiveTextures);

        std::cout << "  Material " << m << " (" << matName.C_Str() << "): "
                  << "diffuse=" << (model->materialTextureIndex[m] >= 0 ? "tex" : "color")
                  << " normal=" << (model->materialNormalTexIndex[m] >= 0 ? "tex" : "none")
                  << " roughMetal=" << (model->materialRoughMetalTexIndex[m] >= 0 ? "tex" : "none")
                  << " rough=" << roughness << " metal=" << metallic << std::endl;
    }

    // --- Geometry + Bone Weights ---
    std::unordered_map<std::string, int> boneNameToIdx;
    std::vector<glm::mat4> inverseBindMatrices;
    int nextJointIdx = 0;

    // Collect all bones
    for (unsigned mi = 0; mi < scene->mNumMeshes; mi++) {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (unsigned bi = 0; bi < mesh->mNumBones; bi++) {
            const aiBone* bone = mesh->mBones[bi];
            std::string name = bone->mName.C_Str();
            if (boneNameToIdx.find(name) == boneNameToIdx.end()) {
                boneNameToIdx[name] = nextJointIdx++;
                inverseBindMatrices.push_back(toGlm(bone->mOffsetMatrix));
            }
        }
    }

    // Extract geometry
    uint32_t globalVertexOffset = 0;
    for (unsigned mi = 0; mi < scene->mNumMeshes; mi++) {
        const aiMesh* mesh = scene->mMeshes[mi];
        uint32_t matIdx = mesh->mMaterialIndex;

        // Bone data
        struct BoneInfluence { glm::ivec4 indices{0}; glm::vec4 weights{0.0f}; };
        std::vector<BoneInfluence> boneData(mesh->mNumVertices);

        for (unsigned bi = 0; bi < mesh->mNumBones; bi++) {
            const aiBone* bone = mesh->mBones[bi];
            int boneIdx = boneNameToIdx[bone->mName.C_Str()];
            for (unsigned wi = 0; wi < bone->mNumWeights; wi++) {
                unsigned vertIdx = bone->mWeights[wi].mVertexId;
                float weight = bone->mWeights[wi].mWeight;
                auto& bd = boneData[vertIdx];
                for (int s = 0; s < 4; s++) {
                    if (bd.weights[s] == 0.0f) {
                        bd.indices[s] = boneIdx;
                        bd.weights[s] = weight;
                        break;
                    }
                }
            }
        }

        // Vertices
        for (unsigned vi = 0; vi < mesh->mNumVertices; vi++) {
            Vertex v{};
            v.position = toGlm(mesh->mVertices[vi]);
            v.normal = mesh->HasNormals() ? toGlm(mesh->mNormals[vi]) : glm::vec3(0, 1, 0);
            v.texCoord = mesh->HasTextureCoords(0)
                ? glm::vec2(mesh->mTextureCoords[0][vi].x, mesh->mTextureCoords[0][vi].y)
                : glm::vec2(0);
            v.texCoord1 = mesh->HasTextureCoords(1)
                ? glm::vec2(mesh->mTextureCoords[1][vi].x, mesh->mTextureCoords[1][vi].y)
                : v.texCoord;  // fallback to UV0 if no UV1
            if (mesh->HasTangentsAndBitangents())
                v.tangent = glm::vec4(toGlm(mesh->mTangents[vi]), 1.0f);
            v.color = glm::vec3(model->materialColors[matIdx]);

            auto& bd = boneData[vi];
            v.boneIndices = bd.indices;
            float wSum = bd.weights.x + bd.weights.y + bd.weights.z + bd.weights.w;
            v.boneWeights = wSum > 0.0f ? bd.weights / wSum : glm::vec4(1, 0, 0, 0);

            model->vertices.push_back(v);
        }

        // Indices
        for (unsigned fi = 0; fi < mesh->mNumFaces; fi++) {
            const aiFace& face = mesh->mFaces[fi];
            if (face.mNumIndices != 3) continue;
            for (unsigned ii = 0; ii < 3; ii++)
                model->indices.push_back(globalVertexOffset + face.mIndices[ii]);
            model->materialPerTriangle.push_back(matIdx);
        }

        globalVertexOffset += mesh->mNumVertices;
    }

    // Skeletal animation removed — skeleton and animation channels are no
    // longer built. Bone weights remain in the Vertex struct as inert data.
    (void)nextJointIdx;
    (void)inverseBindMatrices;

    std::cout << "[ModelLoader] Loaded: " << path << " ("
              << model->vertices.size() << " verts, "
              << model->indices.size() << " indices, "
              << numMats << " materials, "
              << model->albedoTextures.size() << " textures)" << std::endl;
    return model;
}

} // namespace ohao
