// Unified Model Loader — routes to the best backend per format.
// FBX → ufbx, GLB/GLTF → Assimp, OBJ → native.

#include "model_loader.hpp"
#include "animation/skeleton.hpp"
#include "animation/animation_clip.hpp"

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

// === Main Entry Point ===

std::shared_ptr<Model> ModelLoader::load(const std::string& path) {
    std::string ext = getExtension(path);

    if (ext == "obj") {
        return loadOBJ(path);
    } else if (ext == "fbx") {
        return loadFBX(path);
    } else {
        // GLB, GLTF, DAE, etc → Assimp
        return loadAssimp(path);
    }
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
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_LimitBoneWeights |
        aiProcess_FlipUVs;

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

        model->materialEmissiveTexIndex[m] = loadAssimpTexture(scene, mat,
            aiTextureType_EMISSIVE, modelDir, model->emissiveTextures);

        std::cout << "  Material " << m << " (" << matName.C_Str() << "): "
                  << "diffuse=" << (model->materialTextureIndex[m] >= 0 ? "tex" : "color")
                  << " normal=" << (model->materialNormalTexIndex[m] >= 0 ? "tex" : "none")
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

    // --- Skeleton (Assimp node tree → same format as ufbx) ---
    if (!boneNameToIdx.empty()) {
        model->skeleton = std::make_shared<Skeleton>();
        model->skeleton->joints.resize(nextJointIdx);
        model->skeleton->jointMatrices.resize(nextJointIdx, glm::mat4(1.0f));
        model->skeleton->useNodeTree = true;
        model->skeleton->globalInverse = glm::inverse(toGlm(scene->mRootNode->mTransformation));

        // Build full node tree
        std::function<void(const aiNode*, int)> buildTree =
        [&](const aiNode* aiNode, int parentTreeIdx) {
            int myIdx = static_cast<int>(model->skeleton->nodeTree.size());

            SkeletonNode sn;
            sn.name = aiNode->mName.C_Str();
            sn.defaultTransform = toGlm(aiNode->mTransformation);
            sn.animatedTransform = sn.defaultTransform;
            sn.parentNode = parentTreeIdx;

            auto boneIt = boneNameToIdx.find(sn.name);
            if (boneIt != boneNameToIdx.end()) {
                sn.boneIndex = boneIt->second;
                model->skeleton->joints[boneIt->second].name = sn.name;
                model->skeleton->joints[boneIt->second].inverseBindMatrix = inverseBindMatrices[boneIt->second];
            }

            model->skeleton->nodeTree.push_back(std::move(sn));

            for (unsigned c = 0; c < aiNode->mNumChildren; c++) {
                int childIdx = static_cast<int>(model->skeleton->nodeTree.size());
                model->skeleton->nodeTree[myIdx].children.push_back(childIdx);
                buildTree(aiNode->mChildren[c], myIdx);
            }
        };
        model->skeleton->nodeTreeRoot = 0;
        buildTree(scene->mRootNode, -1);

        model->skeleton->computeJointMatrices();
        std::cout << "[ModelLoader] Skeleton: " << nextJointIdx << " joints, "
                  << model->skeleton->nodeTree.size() << " nodes" << std::endl;
    }

    // --- Animations (Assimp channels → target node tree nodes) ---
    if (scene->mNumAnimations > 0 && model->skeleton) {
        // Build node name → tree index map
        std::unordered_map<std::string, int> nodeNameToTreeIdx;
        for (size_t ni = 0; ni < model->skeleton->nodeTree.size(); ni++)
            nodeNameToTreeIdx[model->skeleton->nodeTree[ni].name] = static_cast<int>(ni);

        for (unsigned ai = 0; ai < scene->mNumAnimations; ai++) {
            const aiAnimation* anim = scene->mAnimations[ai];
            auto clip = std::make_shared<AnimationClip>();
            clip->name = anim->mName.length > 0 ? anim->mName.C_Str()
                        : "animation_" + std::to_string(ai);

            double ticksPerSec = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0;
            clip->duration = static_cast<float>(anim->mDuration / ticksPerSec);

            for (unsigned ci = 0; ci < anim->mNumChannels; ci++) {
                const aiNodeAnim* nodeAnim = anim->mChannels[ci];
                std::string nodeName = nodeAnim->mNodeName.C_Str();

                auto nodeIt = nodeNameToTreeIdx.find(nodeName);
                if (nodeIt == nodeNameToTreeIdx.end()) continue;
                int nodeTreeIdx = nodeIt->second;

                auto boneIt = boneNameToIdx.find(nodeName);
                int jointIdx = (boneIt != boneNameToIdx.end()) ? boneIt->second : -1;

                // Translation
                if (nodeAnim->mNumPositionKeys > 0) {
                    AnimationChannel ch;
                    ch.targetNode = nodeTreeIdx;
                    ch.targetJoint = jointIdx;
                    ch.property = AnimationProperty::TRANSLATION;
                    for (unsigned k = 0; k < nodeAnim->mNumPositionKeys; k++) {
                        ch.timestamps.push_back(static_cast<float>(nodeAnim->mPositionKeys[k].mTime / ticksPerSec));
                        auto& v = nodeAnim->mPositionKeys[k].mValue;
                        ch.values.push_back(glm::vec4(v.x, v.y, v.z, 0));
                    }
                    clip->channels.push_back(std::move(ch));
                }

                // Rotation
                if (nodeAnim->mNumRotationKeys > 0) {
                    AnimationChannel ch;
                    ch.targetNode = nodeTreeIdx;
                    ch.targetJoint = jointIdx;
                    ch.property = AnimationProperty::ROTATION;
                    for (unsigned k = 0; k < nodeAnim->mNumRotationKeys; k++) {
                        ch.timestamps.push_back(static_cast<float>(nodeAnim->mRotationKeys[k].mTime / ticksPerSec));
                        auto& q = nodeAnim->mRotationKeys[k].mValue;
                        ch.values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
                    }
                    clip->channels.push_back(std::move(ch));
                }

                // Scale
                if (nodeAnim->mNumScalingKeys > 0) {
                    AnimationChannel ch;
                    ch.targetNode = nodeTreeIdx;
                    ch.targetJoint = jointIdx;
                    ch.property = AnimationProperty::SCALE;
                    for (unsigned k = 0; k < nodeAnim->mNumScalingKeys; k++) {
                        ch.timestamps.push_back(static_cast<float>(nodeAnim->mScalingKeys[k].mTime / ticksPerSec));
                        auto& v = nodeAnim->mScalingKeys[k].mValue;
                        ch.values.push_back(glm::vec4(v.x, v.y, v.z, 0));
                    }
                    clip->channels.push_back(std::move(ch));
                }
            }

            model->animations.push_back(std::move(clip));
            std::cout << "  Clip '" << model->animations.back()->name << "': "
                      << model->animations.back()->duration << "s, "
                      << model->animations.back()->channels.size() << " channels" << std::endl;
        }
    }

    std::cout << "[ModelLoader] Loaded: " << path << " ("
              << model->vertices.size() << " verts, "
              << model->indices.size() << " indices, "
              << numMats << " materials, "
              << model->albedoTextures.size() << " textures)" << std::endl;
    return model;
}

} // namespace ohao
