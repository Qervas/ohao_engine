// FBX loader via ufbx — handles all FBX rotation orders, pivots, and pre-rotations correctly.
// Falls back to Assimp for non-FBX formats (Collada, etc.)

#include "model.hpp"
#include "ufbx.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cstring>

namespace ohao {

static glm::mat4 toGlm(const ufbx_matrix& m) {
    return glm::mat4(
        float(m.cols[0].x), float(m.cols[0].y), float(m.cols[0].z), 0.0f,
        float(m.cols[1].x), float(m.cols[1].y), float(m.cols[1].z), 0.0f,
        float(m.cols[2].x), float(m.cols[2].y), float(m.cols[2].z), 0.0f,
        float(m.cols[3].x), float(m.cols[3].y), float(m.cols[3].z), 1.0f
    );
}

static glm::vec3 toGlm(const ufbx_vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }
static glm::vec2 toGlm2(const ufbx_vec2& v) { return {float(v.x), float(v.y)}; }

// Load texture from file relative to model directory
static bool loadTextureFile(const std::string& path, const std::string& modelDir,
                            Model::TextureData& td) {
    std::vector<std::string> candidates = {
        path,
        modelDir + "/" + path,
        modelDir + "/" + std::filesystem::path(path).filename().string(),
    };
    for (const auto& c : candidates) {
        if (!std::filesystem::exists(c)) continue;
        int w, h, ch;
        uint8_t* px = stbi_load(c.c_str(), &w, &h, &ch, 4);
        if (px) {
            td.width = w; td.height = h;
            td.pixels.assign(px, px + w * h * 4);
            stbi_image_free(px);
            return true;
        }
    }
    return false;
}

// Load a ufbx texture (embedded or file) into a TextureData vector.
// Returns the index into the vector, or -1 if not found.
static int loadUfbxTexture(const ufbx_material_map& map, const std::string& modelDir,
                            std::vector<Model::TextureData>& textures) {
    if (!map.texture) return -1;
    const ufbx_texture* tex = map.texture;
    Model::TextureData td;

    if (tex->content.size > 0) {
        int w, h, ch;
        uint8_t* px = stbi_load_from_memory(
            static_cast<const uint8_t*>(tex->content.data),
            static_cast<int>(tex->content.size), &w, &h, &ch, 4);
        if (px) {
            td.width = w; td.height = h;
            td.pixels.assign(px, px + w * h * 4);
            stbi_image_free(px);
            int idx = static_cast<int>(textures.size());
            textures.push_back(std::move(td));
            return idx;
        }
    } else if (tex->filename.length > 0) {
        if (loadTextureFile(tex->filename.data, modelDir, td)) {
            int idx = static_cast<int>(textures.size());
            textures.push_back(std::move(td));
            return idx;
        }
    }
    return -1;
}

bool Model::loadFromFBX(const std::string& filename) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;    // Convert to Y-up
    opts.target_unit_meters = 1.0f;                     // Convert to meters

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(filename.c_str(), &opts, &error);
    if (!scene) {
        std::cerr << "ufbx error: " << error.description.data << std::endl;
        return false;
    }

    sourcePath = filename;
    std::string modelDir = std::filesystem::path(filename).parent_path().string();

    std::cout << "FBX: " << scene->meshes.count << " meshes, "
              << scene->materials.count << " materials, "
              << scene->textures.count << " textures, "
              << scene->anim_stacks.count << " animations" << std::endl;

    // --- Materials ---
    uint32_t numMats = static_cast<uint32_t>(scene->materials.count);
    materialColors.resize(numMats, glm::vec4(0.8f, 0.8f, 0.8f, 0.5f));
    materialMetallic.resize(numMats, 0.0f);
    materialTextureIndex.resize(numMats, -1);
    materialNormalTexIndex.resize(numMats, -1);
    materialRoughMetalTexIndex.resize(numMats, -1);
    materialEmissiveTexIndex.resize(numMats, -1);

    for (uint32_t mi = 0; mi < numMats; mi++) {
        const ufbx_material* mat = scene->materials.data[mi];

        // Base color
        ufbx_vec4 color = mat->pbr.base_color.value_vec4;
        float metallic = float(mat->pbr.metalness.value_real);

        // Read roughness from model's PBR data, handling glossiness conversion
        float roughness = 0.5f;  // sensible default
        if (mat->pbr.roughness.has_value) {
            roughness = float(mat->pbr.roughness.value_real);
            // If the model stores roughness as glossiness, invert it
            if (mat->features.roughness_as_glossiness.enabled) {
                roughness = 1.0f - roughness;
            }
        } else if (mat->pbr.glossiness.has_value) {
            // Explicit glossiness map — convert to roughness
            roughness = 1.0f - float(mat->pbr.glossiness.value_real);
        } else if (mat->pbr.roughness.texture || mat->pbr.glossiness.texture) {
            // Has a roughness/glossiness texture but no scalar — use 1.0 (texture controls it)
            roughness = 1.0f;
        }
        // Physical minimum: no real dielectric surface has roughness < 0.04
        roughness = std::max(roughness, 0.04f);

        materialColors[mi] = glm::vec4(float(color.x), float(color.y), float(color.z), roughness);
        materialMetallic[mi] = metallic;

        // Diffuse/albedo texture
        // Load all PBR texture maps
        materialTextureIndex[mi] = loadUfbxTexture(mat->pbr.base_color, modelDir, albedoTextures);
        materialNormalTexIndex[mi] = loadUfbxTexture(mat->pbr.normal_map, modelDir, normalTextures);
        materialEmissiveTexIndex[mi] = loadUfbxTexture(mat->pbr.emission_color, modelDir, emissiveTextures);

        // Roughness/metallic — try dedicated maps, then combined ORM
        int roughTexIdx = loadUfbxTexture(mat->pbr.roughness, modelDir, roughMetalTextures);
        if (roughTexIdx < 0) {
            // Try glossiness (inverted roughness)
            roughTexIdx = loadUfbxTexture(mat->pbr.glossiness, modelDir, roughMetalTextures);
        }
        materialRoughMetalTexIndex[mi] = roughTexIdx;

        // Also try metalness as a separate texture
        if (roughTexIdx < 0) {
            roughTexIdx = loadUfbxTexture(mat->pbr.metalness, modelDir, roughMetalTextures);
            materialRoughMetalTexIndex[mi] = roughTexIdx;
        }

        // Check all available PBR texture maps
        std::string texMaps;
        if (mat->pbr.base_color.texture) texMaps += "diffuse ";
        if (mat->pbr.normal_map.texture) texMaps += "normal ";
        if (mat->pbr.roughness.texture) texMaps += "roughness ";
        if (mat->pbr.metalness.texture) texMaps += "metallic ";
        if (mat->pbr.opacity.texture) texMaps += "opacity ";
        if (mat->pbr.emission_color.texture) texMaps += "emissive ";
        if (mat->pbr.ambient_occlusion.texture) texMaps += "ao ";
        if (mat->pbr.specular_color.texture) texMaps += "specular ";
        if (mat->pbr.glossiness.texture) texMaps += "gloss ";
        // Also check FBX-specific maps
        if (mat->fbx.diffuse_color.texture) texMaps += "fbx_diffuse ";
        if (mat->fbx.specular_color.texture) texMaps += "fbx_specular ";
        if (mat->fbx.normal_map.texture) texMaps += "fbx_normal ";
        if (texMaps.empty()) texMaps = "none";

        std::cout << "  Material " << mi << " (" << mat->name.data << "): "
                  << "maps=[" << texMaps << "] rough=" << roughness
                  << std::endl;
    }

    // --- Meshes ---
    // Build global bone index map from all skin clusters
    std::unordered_map<uint32_t, int> clusterToJoint;  // cluster typed_id → joint index
    int nextJointIdx = 0;

    // First pass: collect all skin clusters across all meshes
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        for (size_t si = 0; si < mesh->skin_deformers.count; si++) {
            const ufbx_skin_deformer* skin = mesh->skin_deformers.data[si];
            for (size_t ci = 0; ci < skin->clusters.count; ci++) {
                const ufbx_skin_cluster* cluster = skin->clusters.data[ci];
                if (clusterToJoint.find(cluster->typed_id) == clusterToJoint.end()) {
                    clusterToJoint[cluster->typed_id] = nextJointIdx++;
                }
            }
        }
    }

    // Second pass: extract geometry
    uint32_t globalVertexOffset = 0;
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];

        // Get the skin deformer (if any)
        const ufbx_skin_deformer* skin = nullptr;
        if (mesh->skin_deformers.count > 0) {
            skin = mesh->skin_deformers.data[0];
        }

        // Find material index for this mesh
        uint32_t matIdx = 0;
        if (mesh->materials.count > 0 && mesh->materials.data[0]) {
            matIdx = mesh->materials.data[0]->typed_id;
        }

        // Get the node's geometry transform for this mesh
        glm::mat4 geoTransform(1.0f);
        if (mesh->instances.count > 0) {
            geoTransform = toGlm(mesh->instances.data[0]->geometry_to_node);
        }

        // Triangulate and extract vertices
        size_t numTriIndices = mesh->max_face_triangles * 3;
        std::vector<uint32_t> triIndices(numTriIndices);

        for (size_t fi = 0; fi < mesh->faces.count; fi++) {
            ufbx_face face = mesh->faces.data[fi];
            uint32_t numTris = ufbx_triangulate_face(triIndices.data(), numTriIndices, mesh, face);

            // Per-face material
            uint32_t faceMat = matIdx;
            if (fi < mesh->face_material.count) {
                uint32_t matSlot = mesh->face_material.data[fi];
                if (matSlot < mesh->materials.count && mesh->materials.data[matSlot]) {
                    faceMat = mesh->materials.data[matSlot]->typed_id;
                }
            }

            for (uint32_t ti = 0; ti < numTris; ti++) {
                uint32_t i0 = triIndices[ti * 3 + 0];
                uint32_t i1 = triIndices[ti * 3 + 1];
                uint32_t i2 = triIndices[ti * 3 + 2];

                for (uint32_t idx : {i0, i1, i2}) {
                    Vertex v{};
                    uint32_t vertIdx = mesh->vertex_indices.data[idx];

                    v.position = toGlm(mesh->vertex_position.values.data[mesh->vertex_position.indices.data[idx]]);

                    if (mesh->vertex_normal.exists)
                        v.normal = toGlm(mesh->vertex_normal.values.data[mesh->vertex_normal.indices.data[idx]]);

                    if (mesh->vertex_uv.exists)
                        v.texCoord = toGlm2(mesh->vertex_uv.values.data[mesh->vertex_uv.indices.data[idx]]);

                    if (mesh->vertex_tangent.exists) {
                        auto t = mesh->vertex_tangent.values.data[mesh->vertex_tangent.indices.data[idx]];
                        v.tangent = glm::vec4(float(t.x), float(t.y), float(t.z), 1.0f);
                    }

                    v.color = glm::vec3(materialColors[std::min(faceMat, numMats - 1)]);

                    // Apply geometry transform to position/normal
                    if (mesh->instances.count > 0 && mesh->instances.data[0]->has_geometry_transform) {
                        v.position = glm::vec3(geoTransform * glm::vec4(v.position, 1.0f));
                        v.normal = glm::normalize(glm::mat3(geoTransform) * v.normal);
                    }

                    // Bone weights from skin deformer
                    if (skin && vertIdx < skin->vertices.count) {
                        const ufbx_skin_vertex& sv = skin->vertices.data[vertIdx];
                        for (uint32_t wi = 0; wi < std::min(sv.num_weights, 4u); wi++) {
                            const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + wi];
                            int jointIdx = clusterToJoint[skin->clusters.data[sw.cluster_index]->typed_id];
                            v.boneIndices[wi] = jointIdx;
                            v.boneWeights[wi] = float(sw.weight);
                        }
                        // Normalize weights
                        float wSum = v.boneWeights.x + v.boneWeights.y + v.boneWeights.z + v.boneWeights.w;
                        if (wSum > 0.0f) v.boneWeights /= wSum;
                    }

                    indices.push_back(static_cast<uint32_t>(vertices.size()));
                    vertices.push_back(v);
                }

                materialPerTriangle.push_back(faceMat);
            }
        }
    }

    // Skeletal animation removed — always free the ufbx scene now that
    // geometry has been extracted. Bone weights remain in the Vertex struct
    // as inert data (static rendering ignores them).
    (void)nextJointIdx;
    ufbx_free_scene(scene);

    std::cout << "FBX loaded: " << filename << " ("
              << vertices.size() << " vertices, "
              << indices.size() << " indices, "
              << numMats << " materials, "
              << albedoTextures.size() << " textures)" << std::endl;
    return true;
}

} // namespace ohao
