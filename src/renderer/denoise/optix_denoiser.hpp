#pragma once

// OptiX Denoiser wrapper for OHAO Engine
//
// Uses NVIDIA OptiX AI denoiser to clean noisy path-traced images.
// Requires: OptiX SDK headers, CUDA runtime, Vulkan-CUDA interop.
//
// Usage:
//   OptiXDenoiser denoiser;
//   denoiser.init(vkDevice, physicalDevice, width, height);
//   denoiser.denoise(cmd, noisyImage, albedoImage, normalImage, outputImage);
//
// The denoiser works on linear buffers shared between Vulkan and CUDA.
// Images are copied to shared buffers, denoised on CUDA, copied back.

#include <vulkan/vulkan.h>
#include <cstdint>

// CUDA types — use actual headers if available, else forward declare
#if __has_include(<cuda.h>)
#include <cuda.h>
#else
typedef void* CUcontext;
typedef void* CUstream;
typedef unsigned long long CUdeviceptr;
#endif

namespace ohao {

class OptiXDenoiser {
public:
    OptiXDenoiser() = default;
    ~OptiXDenoiser();

    // Initialize — creates CUDA context, OptiX context, denoiser, shared buffers
    // Returns false if OptiX is not available (graceful fallback)
    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              VkInstance instance, uint32_t width, uint32_t height);

    // Denoise a noisy RGBA32F image
    // noisyImage: input (Vulkan VkImage, RGBA32F, TRANSFER_SRC layout)
    // outputImage: result (Vulkan VkImage, RGBA32F, will be transitioned)
    // albedoImage/normalImage: optional guide buffers (VK_NULL_HANDLE to skip)
    void denoise(VkCommandBuffer cmd, VkQueue queue,
                 VkImage noisyImage, VkImage outputImage,
                 VkImage albedoImage = VK_NULL_HANDLE,
                 VkImage normalImage = VK_NULL_HANDLE);

    void resize(uint32_t width, uint32_t height);
    bool isAvailable() const { return m_available; }
    void destroy();

private:
    bool m_available = false;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;

    // CUDA context
    CUcontext m_cudaContext = nullptr;
    CUstream m_cudaStream = nullptr;

    // OptiX handles (stored as void* to avoid optix.h dependency in header)
    void* m_optixContext = nullptr;    // OptixDeviceContext
    void* m_optixDenoiser = nullptr;   // OptixDenoiser

    // Shared Vulkan-CUDA buffers (external memory)
    struct SharedBuffer {
        VkBuffer vkBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vkMemory = VK_NULL_HANDLE;
        CUdeviceptr cudaPtr = 0;
        VkDeviceSize size = 0;
#ifdef _WIN32
        void* handle = nullptr;  // HANDLE for Win32 sharing
#else
        int fd = -1;             // file descriptor for POSIX sharing
#endif
    };

    SharedBuffer m_inputBuffer;
    SharedBuffer m_outputBuffer;
    SharedBuffer m_albedoGuide;
    SharedBuffer m_normalGuide;
    SharedBuffer m_scratchBuffer;
    SharedBuffer m_stateBuffer;

    // Helpers
    bool initCuda();
    bool initOptix();
    bool createSharedBuffers();
    bool createSharedBuffer(SharedBuffer& buf, VkDeviceSize size);
    void destroySharedBuffer(SharedBuffer& buf);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

} // namespace ohao
