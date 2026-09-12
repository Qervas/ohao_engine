#include "diff/wavefront/scatter_sinks.hpp"

#include <cstddef>

namespace ohao::diff {

bool ScatterSinks::createSet(GpuAllocator& allocator, ScatterSinkSet& set,
                             std::uint32_t capacity, std::uint32_t width, std::uint32_t height,
                             const Strides& strides) {
    const auto paths = static_cast<VkDeviceSize>(capacity);
    const VkDeviceSize filmBytes =
        static_cast<VkDeviceSize>(width) * height * 3u * sizeof(float);

    // HOST-READABLE and persistently mapped: the frozen-direction measurement
    // reads this back and compares two renders' ray origins, directions and
    // hit distances BIT FOR BIT.
    set.trace = allocator.createBuffer(paths * strides.traceFloats * sizeof(float),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
    // Bound because a descriptor set must cover every binding the shader
    // statically uses; never read, so device-local is enough.
    set.env = allocator.createBuffer(paths * strides.envFloats * sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuOnly);
    set.nee = allocator.createBuffer(paths * strides.neeFloats * sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuOnly);
    // TRANSFER_DST as well: the film is zeroed with a fill before each run.
    set.film = allocator.createBuffer(
        filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);

    return set.trace.isValid() && set.env.isValid() && set.nee.isValid() && set.film.isValid();
}

bool ScatterSinks::create(GpuAllocator& allocator, std::uint32_t capacity, std::uint32_t width,
                          std::uint32_t height, const Strides& strides) {
    destroy(allocator);
    if (capacity == 0u || width == 0u || height == 0u || !strides.valid()) return false;
    if (!createSet(allocator, m_forward, capacity, width, height, strides) ||
        !createSet(allocator, m_replay, capacity, width, height, strides)) {
        // Release whatever did get allocated rather than leaving a half-built
        // object whose valid() is false but whose buffers are live.
        destroy(allocator);
        return false;
    }
    return true;
}

void ScatterSinks::destroy(GpuAllocator& allocator) {
    for (ScatterSinkSet* set : {&m_forward, &m_replay}) {
        if (set->trace.isValid()) allocator.destroyBuffer(set->trace);
        if (set->env.isValid()) allocator.destroyBuffer(set->env);
        if (set->nee.isValid()) allocator.destroyBuffer(set->nee);
        if (set->film.isValid()) allocator.destroyBuffer(set->film);
    }
}

bool ScatterSinks::valid() const noexcept {
    return m_forward.trace.isValid() && m_forward.env.isValid() && m_forward.nee.isValid() &&
           m_forward.film.isValid() && m_replay.trace.isValid() && m_replay.env.isValid() &&
           m_replay.nee.isValid() && m_replay.film.isValid();
}

}  // namespace ohao::diff
