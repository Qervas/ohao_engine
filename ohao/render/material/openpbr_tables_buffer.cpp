#include "render/material/openpbr_tables_buffer.hpp"
#include "render/material/openpbr_tables.hpp"

#include <cstring>

namespace ohao::render {

bool OpenPBRTablesBuffer::create(VkDevice device, VkPhysicalDevice physical) {
    if (m_buffer != VK_NULL_HANDLE) return true;
    m_device = device;
    const VkDeviceSize bytes = sizeof(kOpenPBRTables);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &m_buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, m_buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & kWant) == kWant) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &m_memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(device, m_buffer, m_memory, 0) != VK_SUCCESS) return false;
    void* mapped = nullptr;
    if (vkMapMemory(device, m_memory, 0, bytes, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, kOpenPBRTables, bytes);
    vkUnmapMemory(device, m_memory);
    return true;
}

void OpenPBRTablesBuffer::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
    if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
    m_buffer = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
}

} // namespace ohao::render
