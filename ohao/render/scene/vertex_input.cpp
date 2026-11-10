#include "render/scene/vertex_input.hpp"

#include "asset/model.hpp"

#include <cstddef>

namespace ohao::render {

std::vector<VkVertexInputBindingDescription> vertexBindings() {
    return {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
}

std::vector<VkVertexInputAttributeDescription> vertexAttributes() {
    return {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},        // UV set 0
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord1)},       // UV set 1
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},   // xyz direction, w handedness
        {6, 0, VK_FORMAT_R32G32B32A32_SINT, offsetof(Vertex, boneIndices)},
        {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, boneWeights)},
    };
}

} // namespace ohao::render
