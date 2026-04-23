// Sub-plan 4.A T2: NRD wrapper real implementation.
// When OHAO_NRD=OFF this TU compiles to empty.

#include "render/rt/denoise/nrd_denoise.hpp"

#ifdef OHAO_NRD_ENABLED

#include <NRD.h>
#include <iostream>
#include <cstring>

namespace ohao {

struct NrdDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    nrd::Instance*   instance       = nullptr;
    NrdInputImages   inputs {};    // stored for 4.C dispatch
};

NrdDenoiser::NrdDenoiser()  : m_impl(std::make_unique<Impl>()) {}
NrdDenoiser::~NrdDenoiser() { shutdown(); }

bool NrdDenoiser::initialize(VkDevice         device,
                              VkPhysicalDevice physicalDevice,
                              uint32_t         width,
                              uint32_t         height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // NRD v4.17: DenoiserDesc holds only { identifier, denoiser }.
    // Render resolution is supplied per-frame via CommonSettings in 4.B+.
    nrd::DenoiserDesc denoiserDescs[1] = {};
    denoiserDescs[0].identifier        = 0;
    denoiserDescs[0].denoiser          = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

    nrd::InstanceCreationDesc desc = {};
    desc.denoisers                 = denoiserDescs;
    desc.denoisersNum              = 1;

    nrd::Result result = nrd::CreateInstance(desc, m_impl->instance);
    if (result != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] CreateInstance failed: " << int(result) << std::endl;
        return false;
    }

    std::cout << "[NRD] initialized for " << width << "x" << height << std::endl;
    return true;
}

void NrdDenoiser::shutdown() {
    if (m_impl && m_impl->instance) {
        nrd::DestroyInstance(*m_impl->instance);
        m_impl->instance = nullptr;
    }
}

bool NrdDenoiser::setCommonSettings(const NrdCameraInputs& in) {
    if (!m_impl->instance) return false;

    nrd::CommonSettings s = {};
    std::memcpy(s.viewToClipMatrix,      in.projMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrix,     in.viewMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrixPrev, in.viewMatrixPrev.data(),     sizeof(float) * 16);
    std::memcpy(s.motionVectorScale,     in.motionVectorScale.data(),  sizeof(float) * 3);
    std::memcpy(s.cameraJitter,          in.jitter.data(),             sizeof(float) * 2);
    std::memcpy(s.cameraJitterPrev,      in.jitterPrev.data(),         sizeof(float) * 2);
    s.frameIndex = in.frameIndex;
    s.isMotionVectorInWorldSpace = in.isMotionVectorInWorldSpace;

    // NRD asserts resourceSize/rectSize are non-zero. Default to the width/height
    // captured at initialize(). Dynamic resolution scaling (4.C+) can override.
    const uint16_t w = static_cast<uint16_t>(m_impl->width);
    const uint16_t h = static_cast<uint16_t>(m_impl->height);
    s.resourceSize[0]     = w; s.resourceSize[1]     = h;
    s.resourceSizePrev[0] = w; s.resourceSizePrev[1] = h;
    s.rectSize[0]         = w; s.rectSize[1]         = h;
    s.rectSizePrev[0]     = w; s.rectSizePrev[1]     = h;

    nrd::Result r = nrd::SetCommonSettings(*m_impl->instance, s);
    if (r != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] SetCommonSettings failed: " << int(r) << std::endl;
        return false;
    }
    return true;
}

void NrdDenoiser::setInputImages(const NrdInputImages& images) {
    m_impl->inputs = images;
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED not defined

// When OHAO_NRD=OFF, this TU compiles to an empty unit.
// Callers MUST guard `NrdDenoiser` instantiations with `#ifdef OHAO_NRD_ENABLED`
// or they'll hit link errors.

#endif  // OHAO_NRD_ENABLED
