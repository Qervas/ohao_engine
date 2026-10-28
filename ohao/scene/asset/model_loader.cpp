// Unified Model Loader — routes to the best backend per format.
// FBX → ufbx, GLB/GLTF → Assimp, OBJ → native.

#include "model_loader.hpp"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "stb_image.h"

#include "core/log.hpp"
#include "level/gltf_structure.hpp"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cctype>

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

std::string ModelLoader::getExtension(std::string_view path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(path.substr(dot + 1));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool ModelLoader::isSupportedExtension(std::string_view ext) {
    // Accept with or without leading dot
    if (!ext.empty() && ext.front() == '.') ext.remove_prefix(1);
    return ext == "obj" || ext == "fbx" || ext == "glb" || ext == "gltf" ||
           ext == "dae" || ext == "blend" || ext == "3ds" || ext == "ply";
}

// A normal the file did not give: the loaders leave zero there (or a non-finite value when a
// transform normalised a zero).
static bool missingNormal(const glm::vec3& n) {
    const float len2 = glm::dot(n, n);
    return !(len2 > 1e-12f) || !std::isfinite(len2);
}

// Mesh repair: a vertex without a normal gets the area-weighted average of the faces at its
// position (vertices grouped by position), as Blender's "Shade Smooth" would make it. A
// file's own normals are kept, so its hard edges survive (sub-project 5, step 2: the repair
// used to overwrite every normal, smoothing flat-shaded boxes across their edges).
static void repairMeshNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    if (indices.size() < 3 || vertices.empty()) return;
    if (std::none_of(vertices.begin(), vertices.end(), [](const Vertex& v) { return missingNormal(v.normal); })) return;

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

    // Write the smooth normal to the vertices that have none.
    uint32_t repaired = 0;
    for (auto& v : vertices) {
        if (!missingNormal(v.normal)) continue;
        const auto it = smoothNormals.find(posHash(v.position));
        v.normal = it != smoothNormals.end() ? it->second : glm::vec3(0, 1, 0);
        ++repaired;
    }
    OHAO_LOG(Info, asset, "ModelLoader: mesh repair: {} of {} vertices had no normal ({} positions)", repaired,
             vertices.size(), smoothNormals.size());
}

// === Main Entry Point ===

std::shared_ptr<Model> ModelLoader::load(std::string_view path) {
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
                OHAO_LOG(Info, asset, "ModelLoader: FBX: using Assimp geometry (better mesh cleanup)");
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
                    // ufbx loads no occlusion map; Assimp's would not match ufbx's textures.
                    assimpModel->occlusionTextures.clear();
                    assimpModel->materialOcclusionTexIndex.assign(assimpModel->materialOcclusionTexIndex.size(), -1);
                    OHAO_LOG(Info, asset, "ModelLoader: kept ufbx textures ({} albedo)", assimpModel->albedoTextures.size());
                }
                model = assimpModel;
            }
        }
    } else {
        // GLB, GLTF, DAE, etc → Assimp
        model = loadAssimp(path);
    }

    // Post-load: give vertices the file left without a normal their faces' smooth normal
    if (model && !model->vertices.empty()) {
        repairMeshNormals(model->vertices, model->indices);
    }

    return model;
}

Result<std::shared_ptr<Model>> ModelLoader::loadResult(std::string_view path) {
    if (path.empty()) {
        return err_string<std::shared_ptr<Model>>("ModelLoader: empty path");
    }
    const auto ext = getExtension(path);
    if (!isSupportedExtension(ext)) {
        return err_string<std::shared_ptr<Model>>(
            "ModelLoader: unsupported extension '" + ext + "' for path '" + std::string(path) + "'");
    }
    auto model = load(path);
    if (!model || model->empty()) {
        return err_string<std::shared_ptr<Model>>(
            "ModelLoader: failed to load '" + std::string(path) + "'");
    }
    return Result<std::shared_ptr<Model>>::ok(std::move(model));
}

Result<std::shared_ptr<Model>> ModelLoader::loadMesh(std::string_view path, std::string_view mesh) {
    using R = Result<std::shared_ptr<Model>>;
    const std::string where = "'" + std::string(path) + "'";
    const std::string ext = getExtension(path);
    if (ext != "glb" && ext != "gltf") return R::err(where + ": per-mesh loading needs a glTF file (.glb or .gltf)");
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    if (!in) return R::err(where + ": cannot be read");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto structure = level::readGltfStructure(bytes);
    if (!structure) return R::err(where + ": " + structure.error().message);
    const auto& meshes = structure.value().meshes;

    int index = -1;
    if (!mesh.empty() && mesh.front() == '#') {
        int value = 0;
        const auto [end, ec] = std::from_chars(mesh.data() + 1, mesh.data() + mesh.size(), value);
        if (ec == std::errc() && end == mesh.data() + mesh.size() && value >= 0 && value < static_cast<int>(meshes.size()))
            index = value;
    } else {
        for (int i = 0; i < static_cast<int>(meshes.size()); ++i) {
            if (meshes[i].name != mesh) continue;
            if (index >= 0)
                return R::err(where + ": meshes " + std::to_string(index) + " and " + std::to_string(i) + " are both named '" +
                              std::string(mesh) + "'; use '#" + std::to_string(index) + "'");
            index = i;
        }
    }
    if (index < 0)
        return R::err(where + " has no mesh '" + std::string(mesh) + "' (" + std::to_string(meshes.size()) + " meshes)");

    MeshRange range;
    for (int i = 0; i < static_cast<int>(meshes.size()); ++i) {
        if (i < index) range.first += meshes[i].primitives;
        range.total += meshes[i].primitives;
    }
    range.count = meshes[index].primitives;
    auto model = loadAssimp(path, &range);
    if (!model) return R::err(where + ": mesh '" + std::string(mesh) + "' could not be loaded");
    repairMeshNormals(model->vertices, model->indices);
    if (model->empty()) return R::err(where + ": mesh '" + std::string(mesh) + "' has no triangles");
    return R::ok(std::move(model));
}

// === OBJ Loader (native) ===

std::shared_ptr<Model> ModelLoader::loadOBJ(std::string_view path) {
    auto model = std::make_shared<Model>();
    if (model->loadFromOBJ(path)) return model;
    return nullptr;
}

// === FBX Loader (ufbx) ===

std::shared_ptr<Model> ModelLoader::loadFBX(std::string_view path) {
    auto model = std::make_shared<Model>();
    if (model->loadFromFBX(path)) return model;
    return nullptr;
}

// === Assimp Loader (GLB, GLTF, Collada, etc.) ===

// Load texture from Assimp material (embedded or file)
static int loadAssimpTexture(const aiScene* scene, const aiMaterial* mat,
                              aiTextureType type, std::string_view modelDir,
                              std::vector<Model::TextureData>& textures) {
    if (mat->GetTextureCount(type) == 0) return -1;

    aiString texPath;
    mat->GetTexture(type, 0, &texPath);
    std::string pathStr = texPath.C_Str();
    const std::string dir{modelDir};

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
        dir + "/" + pathStr,
        dir + "/" + std::filesystem::path(pathStr).filename().string(),
        dir + "/../textures/" + std::filesystem::path(pathStr).filename().string(),
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

std::shared_ptr<Model> ModelLoader::loadAssimp(std::string_view path, const MeshRange* range) {
    Assimp::Importer importer;
    const std::string pathStr{path};

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
    // One glTF mesh: its primitives are Assimp's meshes [first, first + count) only while no
    // step splits a mesh. SortByPType splits a mesh of mixed primitive types, and
    // FindDegenerates turns degenerate triangles into lines and points that it would then
    // split off, so neither runs here (non-triangle faces are skipped when emitting anyway).
    if (range) flags &= ~(aiProcess_FindDegenerates | aiProcess_SortByPType);

    // Read first, post-process second: a file with skinned meshes keeps its vertices as written.
    // JoinIdenticalVertices compares position, normal and UVs, not joints or weights, so it merged
    // two vertices that differ only in their weights (sub-project 10's rig, at each box seam) and
    // the merged vertex took both sets.
    const aiScene* scene = importer.ReadFile(pathStr, 0);
    if (scene) {
        bool bones = false;
        for (unsigned m = 0; m < scene->mNumMeshes && !bones; ++m) bones = scene->mMeshes[m]->HasBones();
        if (bones) flags &= ~aiProcess_JoinIdenticalVertices;
        scene = importer.ApplyPostProcessing(flags);
    }
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        OHAO_LOG(Error, asset, "ModelLoader: Assimp error: {}", importer.GetErrorString());
        return nullptr;
    }
    if (range && (scene->mNumMeshes != range->total || range->first + range->count > scene->mNumMeshes)) {
        OHAO_LOG(Error, asset, "ModelLoader: {}: Assimp made {} meshes of the file's {} primitives", pathStr,
                 scene->mNumMeshes, range->total);
        return nullptr;
    }

    auto model = std::make_shared<Model>();
    model->setSourcePath(pathStr);
    std::string modelDir = std::filesystem::path(pathStr).parent_path().string();

    OHAO_LOG(Info, asset, "ModelLoader: Assimp: {} meshes, {} materials, {} textures, {} animations",
             scene->mNumMeshes, scene->mNumMaterials, scene->mNumTextures,
             scene->mNumAnimations);

    // --- Materials --- (one mesh: only those its primitives use, renumbered from 0)
    std::vector<unsigned> usedMaterials;
    std::vector<uint32_t> materialSlot(scene->mNumMaterials, 0);
    for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
        bool used = !range;
        for (unsigned i = range ? range->first : 0; range && i < range->first + range->count && !used; ++i)
            used = scene->mMeshes[i]->mMaterialIndex == m;
        if (!used) continue;
        materialSlot[m] = static_cast<uint32_t>(usedMaterials.size());
        usedMaterials.push_back(m);
    }
    uint32_t numMats = static_cast<uint32_t>(usedMaterials.size());
    model->materialColors.resize(numMats, glm::vec4(0.8f, 0.8f, 0.8f, 0.5f));
    model->materialMetallic.resize(numMats, 0.0f);
    model->materialDoubleSided.resize(numMats, 1u);
    model->materialAlphaMode.resize(numMats, 0u);
    model->materialAlphaCutoff.resize(numMats, 0.5f);
    model->materialBaseAlpha.resize(numMats, 1.0f);
    model->materialTextureIndex.resize(numMats, -1);
    model->materialNormalTexIndex.resize(numMats, -1);
    model->materialRoughMetalTexIndex.resize(numMats, -1);
    model->materialEmissiveTexIndex.resize(numMats, -1);
    model->materialOcclusionTexIndex.resize(numMats, -1);

    for (unsigned m = 0; m < numMats; m++) {
        const aiMaterial* mat = scene->mMaterials[usedMaterials[m]];
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
        // glTF's doubleSided (default false): Assimp's glTF importer always reports it as
        // TWOSIDED. Formats that do not say keep the double-sided default.
        int twoSided = 1;
        if (mat->Get(AI_MATKEY_TWOSIDED, twoSided) == aiReturn_SUCCESS)
            model->materialDoubleSided[m] = twoSided != 0 ? 1u : 0u;
        // glTF's alpha: alphaMode (OPAQUE, MASK, BLEND), alphaCutoff (MASK, default 0.5) and the
        // base colour factor's alpha, which coverage multiplies with the albedo texture's.
        aiString alphaMode;
        if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == aiReturn_SUCCESS) {
            const std::string mode = alphaMode.C_Str();
            model->materialAlphaMode[m] = mode == "MASK" ? 1u : mode == "BLEND" ? 2u : 0u;
        }
        float cutoff = 0.5f;
        if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == aiReturn_SUCCESS) model->materialAlphaCutoff[m] = cutoff;
        aiColor4D base(1.0f, 1.0f, 1.0f, 1.0f);
        if (mat->Get(AI_MATKEY_BASE_COLOR, base) == aiReturn_SUCCESS) model->materialBaseAlpha[m] = base.a;
        else if (float opacity = 1.0f; mat->Get(AI_MATKEY_OPACITY, opacity) == aiReturn_SUCCESS)
            model->materialBaseAlpha[m] = opacity;

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

        // glTF occlusionTexture: Assimp's glTF importer reports it as LIGHTMAP. A packed ORM
        // image (the same file as metallicRoughness) is not loaded twice.
        if (mat->GetTextureCount(aiTextureType_LIGHTMAP) > 0) {
            aiString occPath, rmPath;
            mat->GetTexture(aiTextureType_LIGHTMAP, 0, &occPath);
            const bool sameAsRoughMetal = model->materialRoughMetalTexIndex[m] >= 0 &&
                mat->GetTexture(aiTextureType_UNKNOWN, 0, &rmPath) == aiReturn_SUCCESS &&
                std::string_view(occPath.C_Str()) == std::string_view(rmPath.C_Str());
            model->materialOcclusionTexIndex[m] = sameAsRoughMetal
                ? Model::kOcclusionIsRoughMetal
                : loadAssimpTexture(scene, mat, aiTextureType_LIGHTMAP, modelDir, model->occlusionTextures);
        }

        OHAO_LOG(Debug, asset, "  Material {} ({}): diffuse={} normal={} roughMetal={} rough={} metal={}",
                 m, matName.C_Str(),
                 (model->materialTextureIndex[m] >= 0 ? "tex" : "color"),
                 (model->materialNormalTexIndex[m] >= 0 ? "tex" : "none"),
                 (model->materialRoughMetalTexIndex[m] >= 0 ? "tex" : "none"),
                 roughness, metallic);
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

    // Geometry: the node hierarchy, depth first. Each (node, mesh) pair is emitted in model
    // space: positions by the node's accumulated matrix, normals by its inverse transpose,
    // tangents by its upper 3x3 (both renormalised), and triangle winding reversed where the
    // matrix mirrors (negative determinant), so front faces stay front faces. A mesh two
    // nodes reference is emitted twice. Meshes no node references are not emitted.
    uint32_t globalVertexOffset = 0;
    const auto emitMesh = [&](const aiMesh* mesh, const glm::mat4& toModel) {
        const uint32_t matIdx = materialSlot[mesh->mMaterialIndex];
        const glm::mat3 linear(toModel);
        const glm::mat3 normalMat = glm::transpose(glm::inverse(linear));
        const float det = glm::determinant(linear);
        const bool mirrored = det < 0.0f;

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
            v.position = glm::vec3(toModel * glm::vec4(toGlm(mesh->mVertices[vi]), 1.0f));
            // No normal (or a degenerate one) stays zero: repairMeshNormals makes it.
            const glm::vec3 n = mesh->HasNormals() ? normalMat * toGlm(mesh->mNormals[vi]) : glm::vec3(0.0f);
            const float nLen = glm::length(n);
            v.normal = nLen > 1e-20f ? n / nLen : glm::vec3(0.0f);
            v.texCoord = mesh->HasTextureCoords(0)
                ? glm::vec2(mesh->mTextureCoords[0][vi].x, mesh->mTextureCoords[0][vi].y)
                : glm::vec2(0);
            v.texCoord1 = mesh->HasTextureCoords(1)
                ? glm::vec2(mesh->mTextureCoords[1][vi].x, mesh->mTextureCoords[1][vi].y)
                : v.texCoord;  // fallback to UV0 if no UV1
            if (mesh->HasTangentsAndBitangents()) {
                const glm::vec3 t = linear * toGlm(mesh->mTangents[vi]);
                const float tLen = glm::length(t);
                // A mirror flips the bitangent's handedness along with the winding.
                v.tangent = glm::vec4(tLen > 1e-20f ? t / tLen : glm::vec3(1, 0, 0), mirrored ? -1.0f : 1.0f);
            }
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
            model->indices.push_back(globalVertexOffset + face.mIndices[0]);
            model->indices.push_back(globalVertexOffset + face.mIndices[mirrored ? 2 : 1]);
            model->indices.push_back(globalVertexOffset + face.mIndices[mirrored ? 1 : 2]);
            model->materialPerTriangle.push_back(matIdx);
        }

        globalVertexOffset += mesh->mNumVertices;
    };
    std::function<void(const aiNode*, const glm::mat4&)> walk = [&](const aiNode* node, const glm::mat4& parent) {
        const glm::mat4 toModel = parent * toGlm(node->mTransformation);
        for (unsigned k = 0; k < node->mNumMeshes; ++k)
            if (node->mMeshes[k] < scene->mNumMeshes) emitMesh(scene->mMeshes[node->mMeshes[k]], toModel);
        for (unsigned c = 0; c < node->mNumChildren; ++c) walk(node->mChildren[c], toModel);
    };
    if (range) {
        for (uint32_t i = range->first; i < range->first + range->count; ++i) emitMesh(scene->mMeshes[i], glm::mat4(1.0f));
    } else {
        walk(scene->mRootNode, glm::mat4(1.0f));
    }

    // The bone order the vertices index (sub-project 10): skeletons and clips are read from the
    // glTF by ohao/anim; the skinning palette is built in this order by joint name.
    (void)inverseBindMatrices;
    model->boneNames.resize(static_cast<std::size_t>(nextJointIdx));
    for (const auto& [name, idx] : boneNameToIdx) model->boneNames[static_cast<std::size_t>(idx)] = name;

    OHAO_LOG(Info, asset, "ModelLoader: loaded: {} ({} verts, {} indices, {} materials, {} textures)",
             pathStr, model->vertices.size(), model->indices.size(), numMats,
             model->albedoTextures.size());
    return model;
}

} // namespace ohao
