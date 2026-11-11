#pragma once
// The vertex every mesh is made of: cooked files hold arrays of it, and a raster pipeline reads it through
// render/scene/vertex_input.hpp.
#include "core/concepts.hpp"

#include <cstddef>
#include <glm/glm.hpp>

namespace ohao {

struct Vertex{

    glm::vec3 position{};
    glm::vec3 color{};
    glm::vec3 normal{};
    glm::vec2 texCoord{};
    glm::vec2 texCoord1{0.0f, 0.0f};    // Second UV set (GLTF texCoord: 1)
    glm::vec4 tangent{0.0f, 0.0f, 0.0f, 1.0f};       // xyz = tangent direction, w = handedness (+1/-1)
    glm::ivec4 boneIndices{0, 0, 0, 0};  // Up to 4 bone influences per vertex
    glm::vec4 boneWeights{1.0f, 0.0f, 0.0f, 0.0f};   // Corresponding weights (sum to 1.0)
};   // a raster pipeline reads it through render/scene/vertex_input.hpp
static_assert(GpuPod<Vertex>, "Vertex must be GPU-uploadable POD");
static_assert(offsetof(Vertex, position) == 0, "Vertex.position must be at offset 0");
static_assert(offsetof(Vertex, color) == sizeof(glm::vec3), "Vertex.color offset");
static_assert(offsetof(Vertex, normal) == 2 * sizeof(glm::vec3), "Vertex.normal offset");

} // namespace ohao
