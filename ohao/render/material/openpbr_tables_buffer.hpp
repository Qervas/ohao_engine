#pragma once

// OpenPBR's energy tables (render/material/openpbr_tables.hpp) as a storage buffer, written once:
// what material/openpbr.slang's OpenPBRTables reads in the path tracer, deferred lighting and the
// transparent pass.

#include <vulkan/vulkan.h>

namespace ohao::render {

class OpenPBRTablesBuffer {
public:
    OpenPBRTablesBuffer() = default;
    OpenPBRTablesBuffer(const OpenPBRTablesBuffer&) = delete;
    OpenPBRTablesBuffer& operator=(const OpenPBRTablesBuffer&) = delete;
    ~OpenPBRTablesBuffer() { destroy(); }

    // Host-visible, coherent; false (logged by the caller) when it cannot be made.
    [[nodiscard]] bool create(VkDevice device, VkPhysicalDevice physical);
    void destroy();
    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer; }

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkBuffer m_buffer{VK_NULL_HANDLE};
    VkDeviceMemory m_memory{VK_NULL_HANDLE};
};

} // namespace ohao::render
