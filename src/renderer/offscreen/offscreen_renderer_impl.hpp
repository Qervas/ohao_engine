#pragma once

#include "offscreen_renderer.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <array>

namespace ohao {

// Helper function to find suitable memory type
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

} // namespace ohao
