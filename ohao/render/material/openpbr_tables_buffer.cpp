#include "render/material/openpbr_tables_buffer.hpp"
#include "render/material/openpbr_tables.hpp"

#include <cstring>

namespace ohao::render {

bool OpenPBRTablesBuffer::create(GpuAllocator& allocator) {
    if (m_buffer.buffer != VK_NULL_HANDLE) return true;
    m_allocator = &allocator;
    m_buffer = allocator.createBuffer(sizeof(kOpenPBRTables), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      AllocationUsage::CpuToGpu, true);
    if (!m_buffer.isValid() || !m_buffer.getMappedData()) {
        allocator.destroyBuffer(m_buffer);
        m_buffer = GpuBuffer{};
        return false;
    }
    std::memcpy(m_buffer.getMappedData(), kOpenPBRTables, sizeof(kOpenPBRTables));
    allocator.flushBuffer(m_buffer);
    return true;
}

void OpenPBRTablesBuffer::destroy() {
    if (m_buffer.buffer != VK_NULL_HANDLE) m_allocator->retire(m_buffer);
}

} // namespace ohao::render
