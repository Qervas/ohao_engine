// PathTracer's images: the accumulation pair and its targets (PathTracer::Target), from the allocator.

#include "path_tracer.hpp"

namespace ohao {

namespace {

struct TargetDesc {
    const char* name;
    VkFormat format;
    VkImageUsageFlags usage;
    bool outputSized;   // the output resolution's, not the render resolution's
    bool dlssOnly;      // made only in a build with DLSS
};

// Written as storage by the trace, sampled by the denoisers, copied out by the readbacks.
constexpr VkImageUsageFlags kAov = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// In PathTracer::Target's order.
constexpr TargetDesc kTargets[] = {
    // In a DLSS upscaling preset the output is the full-size target the DLSS display pass writes; the raygen's own
    // write into it (the render resolution's corner) is replaced.
    {"pt.output", VK_FORMAT_R8G8B8A8_UNORM, kAov | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, false},
    {"pt.albedo", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.normal", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.motion", VK_FORMAT_R16G16_SFLOAT, kAov, false, false},
    {"pt.depth", VK_FORMAT_R32_SFLOAT, kAov, false, false},
    {"pt.roughness", VK_FORMAT_R16_SFLOAT, kAov, false, false},
    {"pt.diffuse", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.specular", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.diffuse.albedo", VK_FORMAT_R8G8B8A8_UNORM, kAov, false, false},
    {"pt.specular.color", VK_FORMAT_R8G8B8A8_UNORM, kAov, false, false},
    // NRD_NORMAL_ENCODING=2 (NRD_NORMAL_ENCODING_R10G10B10A2_UNORM, external/cmake/nrd.cmake): NRD's rotated oct,
    // the sign in roughness and a 2-bit material ID, as NRD.hlsli's _NRD_EncodeNormalRoughness101010.
    {"pt.normal.roughness", VK_FORMAT_A2B10G10R10_UNORM_PACK32, kAov, false, false},
    {"pt.nrd.diffuse", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.nrd.specular", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    {"pt.nrd.composed", VK_FORMAT_R32G32B32A32_SFLOAT, kAov, false, false},
    // What the first hit or miss emits (a light, an emissive surface, the sky), kept out of NRD's demodulated
    // channels and added back by nrd_compose.
    {"pt.emission", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, false},
    // DLSS writes it; the DLSS display pass reads it as a storage image and writes the output.
    {"pt.dlss.output", VK_FORMAT_R16G16B16A16_SFLOAT, kAov, true, true},
};
static_assert(std::size(kTargets) == static_cast<std::size_t>(PathTracer::Target::Count));

}  // namespace

bool PathTracer::createImages() {
    // SAMPLED: DLSS-RR's HDR COLOR_IN; TRANSFER_SRC: the HDR readback (`--hdr`) copies it.
    m_accum[0] = m_allocator->createImage({"pt.accum0", VK_FORMAT_R32G32B32A32_SFLOAT, m_width, m_height, kAov});
    m_accum[1] = m_allocator->createImage({"pt.accum1", VK_FORMAT_R32G32B32A32_SFLOAT, m_width, m_height, kAov});
    if (!m_accum[0] || !m_accum[1]) return false;
    m_accumWriteIndex = 0;
    m_accumLatest = 0;

    for (std::size_t t = 0; t < std::size(kTargets); ++t) {
        const TargetDesc& d = kTargets[t];
#ifndef OHAO_DLSS_ENABLED
        if (d.dlssOnly) continue;
#endif
        m_targets[t] = m_allocator->createImage(
            {d.name, d.format, d.outputSized ? m_outW : m_width, d.outputSized ? m_outH : m_height, d.usage});
        if (!m_targets[t]) return false;
    }
    return true;
}

void PathTracer::releaseTargets() {
    if (!m_accum[0]) return;
    destroyImages();
    resetAccumulation();
}

void PathTracer::destroyImages() {
    for (gpu::Image& image : m_accum) m_allocator->retire(image);
    for (gpu::Image& image : m_targets) m_allocator->retire(image);
}

} // namespace ohao
