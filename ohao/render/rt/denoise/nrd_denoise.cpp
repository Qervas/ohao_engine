// Sub-plan 4.C T3b: wire NrdDenoiser to NVIDIA's NRDIntegration helper so
// denoise() records real REBLUR_DIFFUSE_SPECULAR compute dispatches.
//
// When OHAO_NRD=OFF this TU compiles to an empty stub (see bottom).

#include "render/rt/denoise/nrd_denoise.hpp"

#ifdef OHAO_NRD_ENABLED

#include <NRD.h>
#include <iostream>
#include <cstring>

// NRDIntegration + NRI are linked into ohao_renderer only when
// OHAO_NRD_INTEGRATION_AVAILABLE is defined (see ohao/render/CMakeLists.txt).
// If absent (e.g. NRI fetch failed at configure time), we keep the old
// behaviour: CreateInstance / SetCommonSettings still work; denoise() logs
// and returns false.
#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
#   include <NRI.h>
#   include <Extensions/NRIHelper.h>
#   include <Extensions/NRIDeviceCreation.h>
#   include <Extensions/NRIRayTracing.h>   // AccelerationStructureBits (used by WrapperVK's AS desc)
#   include <Extensions/NRIWrapperVK.h>
#   include "NRDIntegration.h"
#endif

namespace ohao {

struct NrdDenoiser::Impl {
    VkDevice         device                   = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice           = VK_NULL_HANDLE;
    VkInstance       instance                 = VK_NULL_HANDLE;
    uint32_t         graphicsQueueFamilyIndex = 0;
    uint32_t         width                    = 0;
    uint32_t         height                   = 0;
    NrdInputResources resources {}; // consumed by denoise() for NRI snapshot wrapping

#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    // Persistent integration — created in initialize(), destroyed in shutdown().
    nrd::Integration integration {};
    bool integrationReady = false;
    // Storage for the extension name lists the app passed (we copy into our
    // own storage so VKExtensions' pointer stays stable for the integration's
    // lifetime; DeviceCreationVKDesc doesn't deep-copy).
    std::vector<const char*> instanceExtensionsStorage;
    std::vector<const char*> deviceExtensionsStorage;
#else
    // Legacy 4.B fallback: raw NRD instance, no NRI integration.
    nrd::Instance* rawInstance = nullptr;
#endif
};

NrdDenoiser::NrdDenoiser()  : m_impl(std::make_unique<Impl>()) {}
NrdDenoiser::~NrdDenoiser() { shutdown(); }

bool NrdDenoiser::initialize(VkInstance                      instance,
                              VkDevice                        device,
                              VkPhysicalDevice                physicalDevice,
                              uint32_t                        graphicsQueueFamilyIndex,
                              const std::vector<const char*>& instanceExtensions,
                              const std::vector<const char*>& deviceExtensions,
                              uint32_t                        width,
                              uint32_t                        height) {
    m_impl->instance                 = instance;
    m_impl->device                   = device;
    m_impl->physicalDevice           = physicalDevice;
    m_impl->graphicsQueueFamilyIndex = graphicsQueueFamilyIndex;
    m_impl->width                    = width;
    m_impl->height                   = height;

#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    // Copy extension name lists into persistent storage so VKExtensions'
    // pointers remain stable if the caller's originals go out of scope.
    //
    // Filter out ray-tracing extensions. NRI's ResolveDispatchTable eagerly
    // tries to resolve vkCmdTraceRaysIndirect2KHR if it sees
    // VK_KHR_ray_tracing_pipeline in the desired list, and that function
    // lives in VK_KHR_ray_tracing_maintenance1 which we don't enable. NRD's
    // REBLUR is compute-only, doesn't touch any RT function — so omitting
    // these from the NRI-facing list lets NRI skip the RT path entirely.
    auto isRtExt = [](const char* e) {
        return std::strcmp(e, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0 ||
               std::strcmp(e, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0 ||
               std::strcmp(e, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0;
    };
    m_impl->instanceExtensionsStorage.assign(instanceExtensions.begin(), instanceExtensions.end());
    m_impl->deviceExtensionsStorage.clear();
    m_impl->deviceExtensionsStorage.reserve(deviceExtensions.size());
    for (const char* e : deviceExtensions) {
        if (!isRtExt(e)) m_impl->deviceExtensionsStorage.push_back(e);
    }

    // Describe our pre-existing Vulkan device to NRI.
    nri::QueueFamilyVKDesc queueFamily {};
    queueFamily.queueNum    = 1;
    queueFamily.queueType   = nri::QueueType::GRAPHICS;
    queueFamily.familyIndex = graphicsQueueFamilyIndex;

    nri::DeviceCreationVKDesc vkDesc {};
    vkDesc.vkInstance                       = reinterpret_cast<VKHandle>(instance);
    vkDesc.vkDevice                         = reinterpret_cast<VKHandle>(device);
    vkDesc.vkPhysicalDevice                 = reinterpret_cast<VKHandle>(physicalDevice);
    vkDesc.queueFamilies                    = &queueFamily;
    vkDesc.queueFamilyNum                   = 1;
    vkDesc.minorVersion                     = 3;       // Vulkan 1.3 (matches appInfo.apiVersion)
    vkDesc.vkExtensions.instanceExtensions  = m_impl->instanceExtensionsStorage.data();
    vkDesc.vkExtensions.instanceExtensionNum= static_cast<uint32_t>(m_impl->instanceExtensionsStorage.size());
    vkDesc.vkExtensions.deviceExtensions    = m_impl->deviceExtensionsStorage.data();
    vkDesc.vkExtensions.deviceExtensionNum  = static_cast<uint32_t>(m_impl->deviceExtensionsStorage.size());
    // vkBindingOffsets, enableNRIValidation, etc. left at defaults.

    nrd::DenoiserDesc denoiserDescs[1] = {};
    denoiserDescs[0].identifier = 0;
    denoiserDescs[0].denoiser   = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

    nrd::InstanceCreationDesc instanceCreationDesc = {};
    instanceCreationDesc.denoisers    = denoiserDescs;
    instanceCreationDesc.denoisersNum = 1;

    nrd::IntegrationCreationDesc integrationDesc = {};
    std::snprintf(integrationDesc.name, sizeof(integrationDesc.name), "OHAO_REBLUR");
    integrationDesc.resourceWidth   = static_cast<uint16_t>(width);
    integrationDesc.resourceHeight  = static_cast<uint16_t>(height);
    integrationDesc.queuedFrameNum  = 1;      // single-shot T3b — no multi-frame overlap
    integrationDesc.enableWholeLifetimeDescriptorCaching = false;
    integrationDesc.autoWaitForIdle = true;

    nrd::Result r = m_impl->integration.RecreateVK(integrationDesc, instanceCreationDesc, vkDesc);
    if (r != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] Integration::RecreateVK failed: " << int(r) << std::endl;
        return false;
    }
    m_impl->integrationReady = true;
    std::cout << "[NRD] integration ready @ " << width << "x" << height
              << " (NRI-backed REBLUR_DIFFUSE_SPECULAR)" << std::endl;
    return true;
#else
    // Legacy 4.B path: headless NRD instance, denoise() unavailable.
    nrd::DenoiserDesc denoiserDescs[1] = {};
    denoiserDescs[0].identifier = 0;
    denoiserDescs[0].denoiser   = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

    nrd::InstanceCreationDesc desc = {};
    desc.denoisers    = denoiserDescs;
    desc.denoisersNum = 1;

    nrd::Result result = nrd::CreateInstance(desc, m_impl->rawInstance);
    if (result != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] CreateInstance failed: " << int(result) << std::endl;
        return false;
    }
    std::cout << "[NRD] initialized for " << width << "x" << height
              << " (no NRDIntegration — denoise() will no-op)" << std::endl;
    return true;
#endif
}

void NrdDenoiser::shutdown() {
    if (!m_impl) return;
#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    if (m_impl->integrationReady) {
        m_impl->integration.Destroy();
        m_impl->integrationReady = false;
    }
#else
    if (m_impl->rawInstance) {
        nrd::DestroyInstance(*m_impl->rawInstance);
        m_impl->rawInstance = nullptr;
    }
#endif
}

bool NrdDenoiser::setCommonSettings(const NrdCameraInputs& in) {
#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    if (!m_impl->integrationReady) return false;
#else
    if (!m_impl->rawInstance) return false;
#endif

    nrd::CommonSettings s = {};
    std::memcpy(s.viewToClipMatrix,      in.projMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrix,     in.viewMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrixPrev, in.viewMatrixPrev.data(),     sizeof(float) * 16);
    // 4.E T2: use explicit prev proj instead of mirroring current. NRD v4.17
    // CommonSettings::viewToClipMatrixPrev verified present in NRDSettings.h.
    // On the first frame callers pass identity here (no history yet).
    std::memcpy(s.viewToClipMatrixPrev,  in.projMatrixPrev.data(),     sizeof(float) * 16);
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

#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    nrd::Result r = m_impl->integration.SetCommonSettings(s);
#else
    nrd::Result r = nrd::SetCommonSettings(*m_impl->rawInstance, s);
#endif
    if (r != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] SetCommonSettings failed: " << int(r) << std::endl;
        return false;
    }
    return true;
}

void NrdDenoiser::setInputResources(const NrdInputResources& resources) {
    m_impl->resources = resources;
}

bool NrdDenoiser::denoise(VkCommandBuffer cmd) {
#ifdef OHAO_NRD_INTEGRATION_AVAILABLE
    if (!m_impl->integrationReady) {
        std::cerr << "[NRD] denoise() called before integration ready" << std::endl;
        return false;
    }

    // Must be called once per frame before Denoise.
    m_impl->integration.NewFrame();

    // Build ResourceSnapshot from VkImage handles + VkFormats the caller set.
    // NRD's DenoiseVK path wraps each VkImage into an nri::Texture internally
    // (see NRDIntegration.hpp L679-715), then calls plain Denoise() with the
    // wrapped resources. After Denoise, NRI destroys the wrappers for us.
    nrd::ResourceSnapshot snapshot;
    // All AOVs are in VK_IMAGE_LAYOUT_GENERAL right after the ray trace
    // (they're written as STORAGE images from the raygen shader). Declare the
    // initial state as SHADER_RESOURCE_STORAGE; NRD's internal _Dispatch will
    // transition inputs to SHADER_RESOURCE (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    // as needed before each pipeline that reads them, and leave outputs in
    // SHADER_RESOURCE_STORAGE. No manual pre-barrier needed at call site.
    const nri::AccessLayoutStage kInitialGeneral =
        {nri::AccessBits::SHADER_RESOURCE_STORAGE,
         nri::Layout::SHADER_RESOURCE_STORAGE,
         nri::StageBits::COMPUTE_SHADER};
    auto setSlot = [&](nrd::ResourceType slot, const NrdInputResource& src) {
        nrd::Resource r {};
        r.vk.image  = reinterpret_cast<VKNonDispatchableHandle>(src.image);
        r.vk.format = static_cast<VKEnum>(src.format);
        r.state     = kInitialGeneral;
        snapshot.SetResource(slot, r);
    };
    setSlot(nrd::ResourceType::IN_MV,                    m_impl->resources.motionVector);
    setSlot(nrd::ResourceType::IN_VIEWZ,                 m_impl->resources.viewZ);
    setSlot(nrd::ResourceType::IN_NORMAL_ROUGHNESS,      m_impl->resources.normalRoughness);
    setSlot(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, m_impl->resources.diffRadianceHitDist);
    setSlot(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, m_impl->resources.specRadianceHitDist);
    setSlot(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, m_impl->resources.outDiffRadianceHitDist);
    setSlot(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, m_impl->resources.outSpecRadianceHitDist);

    // restoreInitialState=true: after Denoise(), NRD reverts IN_* back to
    // SHADER_RESOURCE_STORAGE (i.e. VK_IMAGE_LAYOUT_GENERAL) so downstream
    // readbacks (VulkanRenderer::readbackDiffuseRadiance etc.) find them
    // in their expected initial layout.
    snapshot.restoreInitialState = true;

    // Wrap the raw VkCommandBuffer for NRI.
    nri::CommandBufferVKDesc cmdDesc {};
    cmdDesc.vkCommandBuffer = reinterpret_cast<VKHandle>(cmd);
    cmdDesc.queueType       = nri::QueueType::GRAPHICS;

    const nrd::Identifier identifier = 0;
    m_impl->integration.DenoiseVK(&identifier, 1, cmdDesc, snapshot);
    return true;
#else
    (void)cmd;
    std::cerr << "[NRD] denoise() no-op: OhaoNRDIntegration not linked" << std::endl;
    return false;
#endif
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED not defined

// When OHAO_NRD=OFF, NrdDenoiser compiles to a no-op stub. This is needed
// because PathTracer holds an unconditional `std::unique_ptr<NrdDenoiser>`
// (for ABI parity across the ohao_renderer / ohao_gpu_vulkan split). The
// unique_ptr's default_delete instantiation requires NrdDenoiser's dtor to
// be linkable even when no NrdDenoiser is ever constructed. Methods that
// return values still return sentinel failure values; in practice they're
// never called on the OFF path (guarded by OHAO_NRD_ENABLED in callers).

namespace ohao {

struct NrdDenoiser::Impl {};  // empty stub

NrdDenoiser::NrdDenoiser()  = default;
NrdDenoiser::~NrdDenoiser() = default;

bool NrdDenoiser::initialize(VkInstance, VkDevice, VkPhysicalDevice, uint32_t,
                              const std::vector<const char*>&,
                              const std::vector<const char*>&,
                              uint32_t, uint32_t) { return false; }
void NrdDenoiser::shutdown() {}
bool NrdDenoiser::setCommonSettings(const NrdCameraInputs&) { return false; }
void NrdDenoiser::setInputResources(const NrdInputResources&) {}
bool NrdDenoiser::denoise(VkCommandBuffer) { return false; }

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
