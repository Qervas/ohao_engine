#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdlib>

namespace ohao {

/// DLSS quality preset — selects the internal RENDER scale (fraction of the
/// output/display resolution the path tracer actually traces) and the matching
/// NGX NVSDK_NGX_PerfQuality_Value. DLAA = native res (no upscale); the rest
/// render at a lower res and let DLSS-RR reconstruct to the output res.
enum class DlssQuality : uint32_t {
    DLAA = 0,          // 1.0   — native res, pure denoise (no upscale, no perf win)
    Quality,           // 0.667 — MaxQuality
    Balanced,          // 0.58  — Balanced
    Performance,       // 0.5   — MaxPerf (half linear res → ~4x fewer traced pixels)
    UltraPerformance,  // 0.333 — UltraPerformance
};

/// Render-resolution scale (linear, per-axis) for each preset.
inline float dlssRenderScale(DlssQuality q) {
    switch (q) {
        case DlssQuality::DLAA:             return 1.0f;
        case DlssQuality::Quality:          return 0.667f;
        case DlssQuality::Balanced:         return 0.58f;
        case DlssQuality::Performance:      return 0.5f;
        case DlssQuality::UltraPerformance: return 0.333f;
    }
    return 1.0f;
}

inline const char* dlssQualityName(DlssQuality q) {
    switch (q) {
        case DlssQuality::DLAA:             return "dlaa";
        case DlssQuality::Quality:          return "quality";
        case DlssQuality::Balanced:         return "balanced";
        case DlssQuality::Performance:      return "performance";
        case DlssQuality::UltraPerformance: return "ultraperf";
    }
    return "dlaa";
}

// Portable case-insensitive compare (avoids strcasecmp/_stricmp header split).
inline bool dlssIEq(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? char(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? char(*b + 32) : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}

inline DlssQuality parseDlssQuality(const char* s) {
    if (!s || !*s) return DlssQuality::DLAA;
    if (dlssIEq(s, "quality"))                            return DlssQuality::Quality;
    if (dlssIEq(s, "balanced"))                           return DlssQuality::Balanced;
    if (dlssIEq(s, "performance") || dlssIEq(s, "perf"))  return DlssQuality::Performance;
    if (dlssIEq(s, "ultraperf") || dlssIEq(s, "ultra") ||
        dlssIEq(s, "ultraperformance"))                   return DlssQuality::UltraPerformance;
    return DlssQuality::DLAA;   // "dlaa" or anything unrecognized
}

/// Reads OHAO_DLSS_QUALITY once (cached for the process). Default DLAA so the
/// existing native-res behavior is unchanged unless the env var is set.
inline DlssQuality dlssQualityFromEnv() {
    static const DlssQuality q = parseDlssQuality(std::getenv("OHAO_DLSS_QUALITY"));
    return q;
}

/// DlssRR — NVIDIA DLSS Ray Reconstruction (NGX feature "dlssd") wrapper.
///
/// Phase 1 scope (the FOUNDATION): NGX init + DLSS-RR feature creation only.
/// Guide-buffer wiring and the denoise dispatch land in Phase 2 — evaluate() is a
/// documented stub here.
///
/// Modeled on the house denoiser classes (NrdCompositor / AtrousDenoiser): a thin
/// RAII object with initialize()/shutdown(). NGX handles are stored as opaque
/// void* so this header stays free of NGX includes; the .cpp casts them back.
///
/// Requires OHAO_DLSS=ON at CMake time. When OHAO_DLSS=OFF the whole
/// implementation (dlss_rr.cpp) compiles to no-op stubs and initialize()
/// returns false, so callers degrade to raw (no-denoise) output.
class DlssRR {
public:
    DlssRR();
    ~DlssRR();

    DlssRR(const DlssRR&)            = delete;
    DlssRR& operator=(const DlssRR&) = delete;

    /// Initialize NGX for Vulkan on the given (NVIDIA) device, register the
    /// dlssd snippet search path, fetch the capability parameters, and verify
    /// SuperSamplingDenoising.Available == 1. Returns false (and logs the
    /// decoded NVSDK_NGX_Result) if NGX init fails or DLSS-RR is unavailable.
    ///   snippetDir  — dir holding libnvidia-ngx-dlssd.so.* (PathListInfo)
    ///   appDataDir  — writable dir NGX uses for logs/cache
    bool initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                    VkDevice device, const char* snippetDir, const char* appDataDir);

    /// Create the DLSS-RR feature. render==out (DlssQuality::DLAA) is a pure
    /// denoiser; render<out (Quality/Balanced/Performance/UltraPerformance)
    /// enables DLSS upscaling — the guide buffers are read at render res and
    /// COLOR_OUT is written at out res. `quality` selects the matching NGX
    /// NVSDK_NGX_PerfQuality_Value. The NGX create records GPU setup work into
    /// `cmd`; the CALLER owns submitting that command buffer. Returns false
    /// (+ decoded result) on failure. Stores the feature handle.
    bool createFeature(VkCommandBuffer cmd,
                       uint32_t renderW, uint32_t renderH,
                       uint32_t outW, uint32_t outH,
                       DlssQuality quality = DlssQuality::DLAA);

    /// Guide buffers + camera state for one DLSS-RR denoise dispatch. Plain
    /// Vulkan handles only — keeps this header free of NGX includes. Every image
    /// is wrapped into an NVSDK_NGX_Resource_VK inside evaluate().
    struct EvalInputs {
        // COLOR_IN — noisy 1-spp HDR LINEAR radiance (RGBA32F accum buffer).
        VkImage colorInImage = VK_NULL_HANDLE;   VkImageView colorInView = VK_NULL_HANDLE;   VkFormat colorInFormat = VK_FORMAT_UNDEFINED;
        // COLOR_OUT — denoised HDR result DLSS writes (RGBA16F, readWrite).
        VkImage colorOutImage = VK_NULL_HANDLE;  VkImageView colorOutView = VK_NULL_HANDLE;  VkFormat colorOutFormat = VK_FORMAT_UNDEFINED;
        // DIFFUSE_ALBEDO / SPECULAR_ALBEDO (RGBA8 demod guides).
        VkImage diffAlbedoImage = VK_NULL_HANDLE; VkImageView diffAlbedoView = VK_NULL_HANDLE; VkFormat diffAlbedoFormat = VK_FORMAT_UNDEFINED;
        VkImage specAlbedoImage = VK_NULL_HANDLE; VkImageView specAlbedoView = VK_NULL_HANDLE; VkFormat specAlbedoFormat = VK_FORMAT_UNDEFINED;
        // NORMAL+ROUGHNESS packed (world normal xyz [-1,1], perceptual roughness w).
        VkImage normalRoughImage = VK_NULL_HANDLE; VkImageView normalRoughView = VK_NULL_HANDLE; VkFormat normalRoughFormat = VK_FORMAT_UNDEFINED;
        // LINEARDEPTH (R32F view-space Z, positive with distance).
        VkImage depthImage = VK_NULL_HANDLE;     VkImageView depthView = VK_NULL_HANDLE;     VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        // MOTION (RG16F, pixel-space; MVScale flips OHAO's sign to DLSS convention).
        VkImage motionImage = VK_NULL_HANDLE;    VkImageView motionView = VK_NULL_HANDLE;    VkFormat motionFormat = VK_FORMAT_UNDEFINED;

        uint32_t renderW = 0, renderH = 0;         // guide-buffer (input) resolution
        uint32_t outW = 0, outH = 0;               // COLOR_OUT (target) resolution; 0 ⇒ same as render
        float    jitterX = 0.0f, jitterY = 0.0f;   // sub-pixel jitter (render-res pixels); negated inside.
        float    mvScaleX = 1.0f, mvScaleY = 1.0f;
        const float* worldToView = nullptr;        // column-major glm view (world→view)
        const float* viewToClip  = nullptr;        // column-major glm proj (view→clip)
        bool     reset = false;                    // 1 only on genuine reset (first frame / resize)
    };

    /// Phase 2–4: wrap the guide buffers as NGX resources, fill
    /// NVSDK_NGX_VK_DLSSD_Eval_Params, and record NGX_VULKAN_EVALUATE_DLSSD_EXT
    /// into `cmd`. Requires a created feature; no-op otherwise. Returns false
    /// (and logs the decoded NVSDK_NGX_Result) on eval failure.
    bool evaluate(VkCommandBuffer cmd, const EvalInputs& in);

    /// Create the HDR→LDR tonemap compute pipeline (ACES + gamma, matching the
    /// raygen "None" curve). Called once after the feature is created. Returns
    /// false on shader/pipeline creation failure.
    bool createTonemapPipeline();

    /// Record the tonemap: reads the DLSS HDR COLOR_OUT (storage image) and
    /// writes the RGBA8 LDR output. Both views must already be in GENERAL layout.
    void tonemap(VkCommandBuffer cmd, VkImageView hdrInView, VkImageView ldrOutView,
                 uint32_t width, uint32_t height);

    /// Release the DLSS-RR feature handle (safe to call if never created).
    void releaseFeature();

    /// Release the feature, destroy capability parameters, shut down NGX.
    /// Safe to call multiple times.
    void shutdown();

    bool isInitialized()   const { return m_initialized; }
    bool isFeatureCreated() const { return m_featureHandle != nullptr; }

private:
    VkDevice m_device        = VK_NULL_HANDLE;
    bool     m_initialized   = false;
    void*    m_ngxParams     = nullptr;  // NVSDK_NGX_Parameter*
    void*    m_featureHandle = nullptr;  // NVSDK_NGX_Handle*
    uint32_t m_renderW = 0, m_renderH = 0, m_outW = 0, m_outH = 0;

    // HDR→LDR tonemap compute pipeline (DLSS COLOR_OUT → RGBA8 beauty).
    VkDescriptorSetLayout m_tmSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_tmPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_tmPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      m_tmDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_tmDescriptorSet = VK_NULL_HANDLE;
};

}  // namespace ohao
