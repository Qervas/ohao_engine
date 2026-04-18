#include "render/rt/denoise/optix_denoise.hpp"

#ifndef OHAO_HAS_OPTIX

// Stub: when CUDA/OptiX SDK are absent, the call always fails. Caller
// (VulkanRenderer::getPixels) will fall back to OIDN.

#include <iostream>

namespace ohao {

bool optixDenoise(float* /*beauty*/, const float* /*albedo*/, const float* /*normal*/,
                  uint32_t /*width*/, uint32_t /*height*/, bool /*hdr*/) {
    static bool warned = false;
    if (!warned) {
        std::cerr << "[OptiX] not compiled in (set OPTIX_ROOT + rebuild) — "
                     "falling back to OIDN\n";
        warned = true;
    }
    return false;
}

} // namespace ohao

#else // OHAO_HAS_OPTIX defined — full impl follows

#include <cuda.h>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

#include <cstring>
#include <iostream>
#include <mutex>

namespace ohao {

namespace {

// --- Helpers ------------------------------------------------------------

#define CUDA_CHECK(call)                                                         \
    do {                                                                          \
        cudaError_t err = (call);                                                 \
        if (err != cudaSuccess) {                                                 \
            std::cerr << "[OptiX] CUDA error " << cudaGetErrorName(err)           \
                      << " at " << __FILE__ << ":" << __LINE__ << '\n';           \
            return false;                                                         \
        }                                                                         \
    } while (0)

#define OPTIX_CHECK(call)                                                         \
    do {                                                                          \
        OptixResult res = (call);                                                 \
        if (res != OPTIX_SUCCESS) {                                               \
            std::cerr << "[OptiX] OptiX error " << static_cast<int>(res)          \
                      << " at " << __FILE__ << ":" << __LINE__ << '\n';           \
            return false;                                                         \
        }                                                                         \
    } while (0)

void optixLogCallback(unsigned int level, const char* tag, const char* msg, void*) {
    // level 0 = disable, 1 = fatal, 2 = error, 3 = warning, 4 = print
    if (level <= 3) {
        std::cerr << "[OptiX][" << tag << "] " << msg << '\n';
    }
}

// --- PIMPL state --------------------------------------------------------

struct OptixDenoiserState {
    bool               initialized = false;
    CUcontext          cudaCtx     = nullptr;
    OptixDeviceContext context     = nullptr;
    OptixDenoiser      denoiser    = nullptr;
    CUstream           stream      = 0;

    // Sized to current (w, h). Reallocated on resolution change.
    uint32_t     lastWidth  = 0;
    uint32_t     lastHeight = 0;
    CUdeviceptr  scratch    = 0;  size_t scratchSize = 0;
    CUdeviceptr  state      = 0;  size_t stateSize   = 0;
    CUdeviceptr  inBeauty   = 0;
    CUdeviceptr  inAlbedo   = 0;
    CUdeviceptr  inNormal   = 0;
    CUdeviceptr  outBeauty  = 0;

    ~OptixDenoiserState() { destroy(); }

    void destroy() {
        if (!initialized) return;
        if (inBeauty)  cudaFree(reinterpret_cast<void*>(inBeauty));
        if (inAlbedo)  cudaFree(reinterpret_cast<void*>(inAlbedo));
        if (inNormal)  cudaFree(reinterpret_cast<void*>(inNormal));
        if (outBeauty) cudaFree(reinterpret_cast<void*>(outBeauty));
        if (scratch)   cudaFree(reinterpret_cast<void*>(scratch));
        if (state)     cudaFree(reinterpret_cast<void*>(state));
        if (denoiser)  optixDenoiserDestroy(denoiser);
        if (context)   optixDeviceContextDestroy(context);
        if (stream)    cudaStreamDestroy(stream);
        initialized = false;
        inBeauty = inAlbedo = inNormal = outBeauty = scratch = state = 0;
        denoiser = nullptr;
        context  = nullptr;
        stream   = 0;
        lastWidth = lastHeight = 0;
    }
};

OptixDenoiserState& getState() {
    static OptixDenoiserState s;
    return s;
}

std::mutex& getMutex() {
    static std::mutex m;
    return m;
}

// --- One-time init -------------------------------------------------------

bool ensureInitialized(OptixDenoiserState& s) {
    if (s.initialized) return true;

    // CUDA init — implicit primary context via cudaFree(0)
    CUDA_CHECK(cudaFree(0));
    CUcontext curCtx = nullptr;
    cuCtxGetCurrent(&curCtx);
    s.cudaCtx = curCtx;

    // OptiX init
    OPTIX_CHECK(optixInit());
    OptixDeviceContextOptions devOpts{};
    devOpts.logCallbackFunction = &optixLogCallback;
    devOpts.logCallbackLevel    = 4;
    OPTIX_CHECK(optixDeviceContextCreate(s.cudaCtx, &devOpts, &s.context));

    // Denoiser — HDR model with albedo + normal guides.
    // NOTE: In OptiX 9.1, denoiseAlpha is a field of OptixDenoiserOptions
    // (passed to optixDenoiserCreate), not OptixDenoiserParams.
    OptixDenoiserOptions dnOpts{};
    dnOpts.guideAlbedo  = 1;
    dnOpts.guideNormal  = 1;
    dnOpts.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
    OPTIX_CHECK(optixDenoiserCreate(s.context, OPTIX_DENOISER_MODEL_KIND_HDR,
                                     &dnOpts, &s.denoiser));

    CUDA_CHECK(cudaStreamCreate(&s.stream));

    s.initialized = true;
    return true;
}

// --- Resolution-dependent buffers ----------------------------------------

bool ensureResolutionBuffers(OptixDenoiserState& s, uint32_t w, uint32_t h) {
    if (s.lastWidth == w && s.lastHeight == h) return true;

    // Free previous
    if (s.inBeauty)  { cudaFree(reinterpret_cast<void*>(s.inBeauty));  s.inBeauty = 0; }
    if (s.inAlbedo)  { cudaFree(reinterpret_cast<void*>(s.inAlbedo));  s.inAlbedo = 0; }
    if (s.inNormal)  { cudaFree(reinterpret_cast<void*>(s.inNormal));  s.inNormal = 0; }
    if (s.outBeauty) { cudaFree(reinterpret_cast<void*>(s.outBeauty)); s.outBeauty = 0; }
    if (s.scratch)   { cudaFree(reinterpret_cast<void*>(s.scratch));   s.scratch = 0; }
    if (s.state)     { cudaFree(reinterpret_cast<void*>(s.state));     s.state = 0; }

    // Compute memory requirements
    OptixDenoiserSizes sizes{};
    OPTIX_CHECK(optixDenoiserComputeMemoryResources(s.denoiser, w, h, &sizes));
    s.stateSize   = sizes.stateSizeInBytes;
    s.scratchSize = sizes.withoutOverlapScratchSizeInBytes;

    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.state),   s.stateSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.scratch), s.scratchSize));

    const size_t pixBytes = static_cast<size_t>(w) * h * 3 * sizeof(float);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inBeauty),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inAlbedo),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inNormal),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.outBeauty), pixBytes));

    // Setup denoiser for this resolution
    OPTIX_CHECK(optixDenoiserSetup(s.denoiser, s.stream, w, h,
                                    s.state,   s.stateSize,
                                    s.scratch, s.scratchSize));

    s.lastWidth  = w;
    s.lastHeight = h;
    return true;
}

// --- Build an OptixImage2D descriptor ------------------------------------
OptixImage2D makeImage(CUdeviceptr data, uint32_t w, uint32_t h) {
    OptixImage2D img{};
    img.data               = data;
    img.width              = w;
    img.height             = h;
    img.rowStrideInBytes   = w * 3 * sizeof(float);
    img.pixelStrideInBytes = 3 * sizeof(float);
    img.format             = OPTIX_PIXEL_FORMAT_FLOAT3;
    return img;
}

} // namespace

// --- Public entry --------------------------------------------------------

bool optixDenoise(float* beauty, const float* albedo, const float* normal,
                  uint32_t width, uint32_t height, bool /*hdr*/) {
    std::lock_guard<std::mutex> lock(getMutex());
    OptixDenoiserState& s = getState();

    if (!ensureInitialized(s))                      return false;
    if (!ensureResolutionBuffers(s, width, height)) return false;

    const size_t pixBytes = static_cast<size_t>(width) * height * 3 * sizeof(float);

    // H2D
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inBeauty), beauty, pixBytes,
                                cudaMemcpyHostToDevice, s.stream));
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inAlbedo), albedo, pixBytes,
                                cudaMemcpyHostToDevice, s.stream));
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inNormal), normal, pixBytes,
                                cudaMemcpyHostToDevice, s.stream));

    // Build layers
    OptixDenoiserGuideLayer guide{};
    guide.albedo = makeImage(s.inAlbedo, width, height);
    guide.normal = makeImage(s.inNormal, width, height);

    OptixDenoiserLayer layer{};
    layer.input  = makeImage(s.inBeauty,  width, height);
    layer.output = makeImage(s.outBeauty, width, height);

    // NOTE: In OptiX 9.1, OptixDenoiserParams does NOT have a denoiseAlpha field.
    // Alpha mode is set via OptixDenoiserOptions at denoiser creation time.
    // hdrIntensity and hdrAverageColor are CUdeviceptr (0 = auto-compute).
    OptixDenoiserParams params{};
    params.hdrIntensity      = 0;  // null → auto-exposure
    params.blendFactor       = 0.0f;
    params.hdrAverageColor   = 0;  // null → auto-compute (AOV mode only)
    params.temporalModeUsePreviousLayers = 0;

    OPTIX_CHECK(optixDenoiserInvoke(s.denoiser, s.stream,
                                     &params,
                                     s.state,   s.stateSize,
                                     &guide,
                                     &layer, 1,
                                     0, 0,
                                     s.scratch, s.scratchSize));

    CUDA_CHECK(cudaStreamSynchronize(s.stream));

    // D2H
    CUDA_CHECK(cudaMemcpy(beauty, reinterpret_cast<void*>(s.outBeauty), pixBytes,
                           cudaMemcpyDeviceToHost));

    std::cout << "[OptiX] Denoised " << width << "x" << height << '\n';
    return true;
}

} // namespace ohao

#endif // OHAO_HAS_OPTIX
