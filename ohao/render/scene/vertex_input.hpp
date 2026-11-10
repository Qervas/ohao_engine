#pragma once
// How a raster pipeline reads asset::Vertex (asset/model.hpp): one binding, eight attributes at their offsets.
#include <vulkan/vulkan_core.h>

#include <vector>

namespace ohao::render {

[[nodiscard]] std::vector<VkVertexInputBindingDescription> vertexBindings();
[[nodiscard]] std::vector<VkVertexInputAttributeDescription> vertexAttributes();

} // namespace ohao::render
