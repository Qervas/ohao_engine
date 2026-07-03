#include "render/rt/denoise/dlss_rr.hpp"

// When OHAO_DLSS=OFF this TU compiles to no-op stubs (same pattern as
// nrd_denoise.cpp under OHAO_NRD). No NGX headers, no NGX symbols.
#ifdef OHAO_DLSS_ENABLED

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_params_dlssd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace ohao {
namespace {

// Placeholder project UUID + engine version (no app-id whitelist needed for the
// custom-engine ProjectID init form — same as tools/dlss_probe).
constexpr const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
constexpr const char* kEngineVer = "1.0.0";

std::wstring widen(const char* s) {
    // Paths here are ASCII; a byte-wise widen is sufficient.
    std::wstring w;
    if (!s) return w;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
        w.push_back(static_cast<wchar_t>(*p));
    return w;
}

// Load a compiled SPV. Mirrors NrdCompositor::readShaderSpv — tries the raw name
// then the two build-output locations OHAO copies shaders into.
std::vector<char> readShaderSpv(const std::string& name) {
    const std::string searchPaths[] = {
        name,
        "build/shaders/" + name,
        "build/bin/shaders/" + name,
        "bin/shaders/" + name,
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = static_cast<size_t>(file.tellg());
            std::vector<char> buf(size);
            file.seekg(0);
            file.read(buf.data(), size);
            return buf;
        }
    }
    return {};
}

const char* ngxResultName(NVSDK_NGX_Result r) {
    switch (r) {
        case NVSDK_NGX_Result_Success:                        return "Success";
        case NVSDK_NGX_Result_Fail:                           return "Fail";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:       return "FAIL_FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:             return "FAIL_PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:      return "FAIL_FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:           return "FAIL_FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:          return "FAIL_InvalidParameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:     return "FAIL_ScratchBufferTooSmall";
        case NVSDK_NGX_Result_FAIL_NotInitialized:            return "FAIL_NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:    return "FAIL_UnsupportedInputFormat";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:             return "FAIL_RWFlagMissing";
        case NVSDK_NGX_Result_FAIL_MissingInput:              return "FAIL_MissingInput";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                 return "FAIL_OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:            return "FAIL_OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:         return "FAIL_UnsupportedFormat";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "FAIL_UnableToWriteToAppDataPath";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter:      return "FAIL_UnsupportedParameter";
        case NVSDK_NGX_Result_FAIL_Denied:                    return "FAIL_Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:            return "FAIL_NotImplemented";
        default:                                              return "Unknown";
    }
}

}  // namespace

DlssRR::DlssRR() = default;

DlssRR::~DlssRR() {
    shutdown();
}

bool DlssRR::initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                        VkDevice device, const char* snippetDir, const char* appDataDir) {
    if (m_initialized) return true;
    m_device = device;

    if (appDataDir && *appDataDir) mkdir(appDataDir, 0755);  // NGX needs a writable dir

    const std::wstring snippetDirW = widen(snippetDir);
    const std::wstring appDataDirW = widen(appDataDir);

    // Tell NGX where the dlssd snippet .so lives (belt-and-suspenders alongside
    // LD_LIBRARY_PATH) and turn logging on so failures surface a reason.
    const wchar_t* pathList[1] = { snippetDirW.c_str() };
    NVSDK_NGX_FeatureCommonInfo commonInfo = {};
    commonInfo.PathListInfo.Path               = pathList;
    commonInfo.PathListInfo.Length             = 1;
    commonInfo.LoggingInfo.LoggingCallback     = nullptr;
    commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    commonInfo.LoggingInfo.DisableOtherLoggingSinks = false;

    NVSDK_NGX_Result initRes = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVer,
        appDataDirW.c_str(), instance, physicalDevice, device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &commonInfo, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(initRes)) {
        std::cerr << "[DLSS-RR] NGX init FAILED: result=0x" << std::hex << (unsigned)initRes
                  << std::dec << " (" << ngxResultName(initRes) << ")\n";
        return false;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Result capRes = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(capRes) || !params) {
        std::cerr << "[DLSS-RR] GetCapabilityParameters FAILED: result=0x" << std::hex
                  << (unsigned)capRes << std::dec << " (" << ngxResultName(capRes) << ")\n";
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
        return false;
    }

    int ssdAvailable = 0;
    params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &ssdAvailable);
    if (!ssdAvailable) {
        int      initResultRaw = 0, needsDriver = 0;
        unsigned minMaj = 0, minMin = 0;
        params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &initResultRaw);
        params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
        params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &minMaj);
        params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minMin);
        std::cerr << "[DLSS-RR] SuperSamplingDenoising NOT available: FeatureInitResult=0x"
                  << std::hex << (unsigned)initResultRaw << std::dec << " ("
                  << ngxResultName((NVSDK_NGX_Result)initResultRaw) << ")";
        if (needsDriver) std::cerr << " — NeedsUpdatedDriver, require >= " << minMaj << "." << minMin;
        std::cerr << "\n";
        NVSDK_NGX_VULKAN_DestroyParameters(params);
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
        return false;
    }

    m_ngxParams   = params;
    m_initialized = true;
    std::cout << "[DLSS-RR] NGX init OK" << std::endl;
    return true;
}

bool DlssRR::createFeature(VkCommandBuffer cmd,
                           uint32_t renderW, uint32_t renderH,
                           uint32_t outW, uint32_t outH) {
    if (!m_initialized || !m_ngxParams) {
        std::cerr << "[DLSS-RR] createFeature called before successful initialize()\n";
        return false;
    }
    if (m_featureHandle) return true;  // already created

    auto* params = static_cast<NVSDK_NGX_Parameter*>(m_ngxParams);

    NVSDK_NGX_DLSSD_Create_Params createParams = {};
    createParams.InDenoiseMode   = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;   // required for Ray Reconstruction
    // Packed: perceptual roughness rides in the normal buffer's w channel (the
    // raygen's DLSSRR path writes vec4(worldN, roughness) to binding 7). Matches
    // nvpro-samples vk_denoise_dlssrr, which feeds one buffer to both pInNormals
    // and pInRoughness. (Phase 1 chose Unpacked; Phase 2 reconciles to Packed.)
    createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    createParams.InUseHWDepth    = NVSDK_NGX_DLSS_Depth_Type_Linear;        // OHAO depth AOV is linear view-Z (R32F), not HW depth
    createParams.InWidth         = renderW;
    createParams.InHeight        = renderH;
    createParams.InTargetWidth   = outW;
    createParams.InTargetHeight  = outH;
    // DLAA == native-resolution (no upscale) — correct for a pure denoiser where render==out.
    createParams.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    // Phase 1: HDR radiance input. MVLowRes is REQUIRED by the DLSS-D model
    // ("Low resolution Motion Vectors required" at create otherwise); for a
    // native-res denoiser (render==out) low-res MVs equal full-res MVs, so this
    // is semantically correct. Depth-inversion / auto-exposure flags are
    // finalized in Phase 2 when the guide buffers are wired.
    createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    createParams.InEnableOutputSubrects = false;

    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(
        m_device, cmd, /*creationNodeMask*/ 0x1, /*visibilityNodeMask*/ 0x1,
        &handle, params, &createParams);
    if (NVSDK_NGX_FAILED(r) || !handle) {
        std::cerr << "[DLSS-RR] feature creation FAILED: result=0x" << std::hex << (unsigned)r
                  << std::dec << " (" << ngxResultName(r) << ")\n";
        return false;
    }

    m_featureHandle = handle;
    m_renderW = renderW; m_renderH = renderH; m_outW = outW; m_outH = outH;
    std::cout << "[DLSS-RR] feature created " << renderW << "x" << renderH
              << " (out " << outW << "x" << outH << ")" << std::endl;
    return true;
}

bool DlssRR::evaluate(VkCommandBuffer cmd, const EvalInputs& in) {
    if (!m_featureHandle || !m_ngxParams) return false;
    auto* params = static_cast<NVSDK_NGX_Parameter*>(m_ngxParams);

    VkImageSubresourceRange range{};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;

    // Wrap a VkImage/VkImageView as an NGX resource. readWrite=true for COLOR_OUT.
    auto wrap = [&](VkImageView view, VkImage image, VkFormat fmt,
                    uint32_t w, uint32_t h, bool readWrite) {
        return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, range, fmt, w, h, readWrite);
    };

    NVSDK_NGX_Resource_VK colorIn   = wrap(in.colorInView,    in.colorInImage,    in.colorInFormat,    in.renderW, in.renderH, false);
    NVSDK_NGX_Resource_VK colorOut  = wrap(in.colorOutView,   in.colorOutImage,   in.colorOutFormat,   in.renderW, in.renderH, true);
    NVSDK_NGX_Resource_VK diffAlb   = wrap(in.diffAlbedoView, in.diffAlbedoImage, in.diffAlbedoFormat, in.renderW, in.renderH, false);
    NVSDK_NGX_Resource_VK specAlb   = wrap(in.specAlbedoView, in.specAlbedoImage, in.specAlbedoFormat, in.renderW, in.renderH, false);
    NVSDK_NGX_Resource_VK normRough = wrap(in.normalRoughView,in.normalRoughImage,in.normalRoughFormat,in.renderW, in.renderH, false);
    NVSDK_NGX_Resource_VK depth     = wrap(in.depthView,      in.depthImage,      in.depthFormat,      in.renderW, in.renderH, false);
    NVSDK_NGX_Resource_VK motion    = wrap(in.motionView,     in.motionImage,     in.motionFormat,     in.renderW, in.renderH, false);

    NVSDK_NGX_VK_DLSSD_Eval_Params evalParams = {};
    evalParams.pInColor          = &colorIn;
    evalParams.pInOutput         = &colorOut;
    evalParams.pInDiffuseAlbedo  = &diffAlb;
    evalParams.pInSpecularAlbedo = &specAlb;
    // Packed roughness mode: the same buffer feeds normals AND roughness — the
    // world-space normal is xyz and perceptual roughness is w (matches the
    // NVSDK_NGX_DLSS_Roughness_Mode_Packed create param + raygen DLSS write).
    evalParams.pInNormals   = &normRough;
    evalParams.pInRoughness = &normRough;
    evalParams.pInDepth          = &depth;
    evalParams.pInMotionVectors  = &motion;

    // Jitter offset must be in input/render pixel space; DLSS wants the negative
    // of the projection jitter (per nvpro-samples vk_denoise_dlssrr).
    evalParams.InJitterOffsetX = -in.jitterX;
    evalParams.InJitterOffsetY = -in.jitterY;
    // OHAO writes motion = currPix - prevPix; DLSS wants prevPix - currPix, so the
    // scale flips the sign (caller passes -1, -1).
    evalParams.InMVScaleX = in.mvScaleX;
    evalParams.InMVScaleY = in.mvScaleY;

    evalParams.InRenderSubrectDimensions.Width  = in.renderW;
    evalParams.InRenderSubrectDimensions.Height = in.renderH;

    // DLSS-RR wants row-major, left-multiply matrices. glm gives column-major,
    // right-multiply; the two transposes cancel, so passing glm's raw pointers
    // is correct (per the nvpro-samples note).
    evalParams.pInWorldToViewMatrix = const_cast<float*>(in.worldToView);
    evalParams.pInViewToClipMatrix  = const_cast<float*>(in.viewToClip);

    evalParams.InReset = in.reset ? 1 : 0;

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT(
        cmd, static_cast<NVSDK_NGX_Handle*>(m_featureHandle), params, &evalParams);
    if (NVSDK_NGX_FAILED(r)) {
        std::cerr << "[DLSS-RR] evaluate FAILED: result=0x" << std::hex << (unsigned)r
                  << std::dec << " (" << ngxResultName(r) << ")\n";
        return false;
    }
    return true;
}

bool DlssRR::createTonemapPipeline() {
    if (m_tmPipeline) return true;

    // 1. Descriptor set layout: binding 0 = HDR in (storage), binding 1 = LDR out (storage).
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_tmSetLayout) != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_tmSetLayout;
    if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_tmPipelineLayout) != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkCreatePipelineLayout failed\n";
        return false;
    }

    auto spv = readShaderSpv("compute_dlss_tonemap.comp.spv");
    if (spv.empty()) {
        std::cerr << "[DLSS-RR] tonemap: failed to load compute_dlss_tonemap.comp.spv\n";
        return false;
    }
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &smInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkCreateShaderModule failed\n";
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName  = "main";
    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage  = stage;
    pipeInfo.layout = m_tmPipelineLayout;
    VkResult pr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_tmPipeline);
    vkDestroyShaderModule(m_device, shaderModule, nullptr);
    if (pr != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkCreateComputePipelines failed: " << int(pr) << "\n";
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_tmDescriptorPool) != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkCreateDescriptorPool failed\n";
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_tmDescriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_tmSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_tmDescriptorSet) != VK_SUCCESS) {
        std::cerr << "[DLSS-RR] tonemap: vkAllocateDescriptorSets failed\n";
        return false;
    }
    std::cout << "[DLSS-RR] tonemap pipeline ready" << std::endl;
    return true;
}

void DlssRR::tonemap(VkCommandBuffer cmd, VkImageView hdrInView, VkImageView ldrOutView,
                     uint32_t width, uint32_t height) {
    if (!m_tmPipeline) return;

    VkDescriptorImageInfo infos[2] = {};
    infos[0].imageView   = hdrInView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[1].imageView   = ldrOutView;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_tmDescriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tmPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tmPipelineLayout, 0, 1,
                            &m_tmDescriptorSet, 0, nullptr);
    vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
}

void DlssRR::releaseFeature() {
    if (m_featureHandle) {
        NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_featureHandle));
        m_featureHandle = nullptr;
    }
}

void DlssRR::shutdown() {
    releaseFeature();
    if (m_tmDescriptorPool) { vkDestroyDescriptorPool(m_device, m_tmDescriptorPool, nullptr); m_tmDescriptorPool = VK_NULL_HANDLE; }
    if (m_tmPipeline)       { vkDestroyPipeline(m_device, m_tmPipeline, nullptr);             m_tmPipeline = VK_NULL_HANDLE; }
    if (m_tmPipelineLayout) { vkDestroyPipelineLayout(m_device, m_tmPipelineLayout, nullptr); m_tmPipelineLayout = VK_NULL_HANDLE; }
    if (m_tmSetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_tmSetLayout, nullptr); m_tmSetLayout = VK_NULL_HANDLE; }
    m_tmDescriptorSet = VK_NULL_HANDLE;
    if (m_ngxParams) {
        NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(m_ngxParams));
        m_ngxParams = nullptr;
    }
    if (m_initialized && m_device) {
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
    }
    m_initialized = false;
    m_device = VK_NULL_HANDLE;
}

}  // namespace ohao

#else  // !OHAO_DLSS_ENABLED — no-op stubs so the TU links when DLSS is disabled.

namespace ohao {
DlssRR::DlssRR() = default;
DlssRR::~DlssRR() = default;
bool DlssRR::initialize(VkInstance, VkPhysicalDevice, VkDevice, const char*, const char*) { return false; }
bool DlssRR::createFeature(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
bool DlssRR::evaluate(VkCommandBuffer, const EvalInputs&) { return false; }
bool DlssRR::createTonemapPipeline() { return false; }
void DlssRR::tonemap(VkCommandBuffer, VkImageView, VkImageView, uint32_t, uint32_t) {}
void DlssRR::releaseFeature() {}
void DlssRR::shutdown() {}
}  // namespace ohao

#endif  // OHAO_DLSS_ENABLED
