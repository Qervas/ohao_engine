#include "optix_denoiser.hpp"
#include <iostream>

// OptiX denoiser requires the OptiX SDK headers.
// If not found, the denoiser gracefully reports unavailable.
// Install OptiX SDK and set OPTIX_PATH in CMake to enable.

#if __has_include(<optix.h>)
#define OHAO_HAS_OPTIX 1
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>
#include <cuda_runtime.h>
#include <cuda.h>
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#endif
#else
#define OHAO_HAS_OPTIX 0
#endif

namespace ohao {

OptiXDenoiser::~OptiXDenoiser() {
    destroy();
}

bool OptiXDenoiser::init(VkDevice device, VkPhysicalDevice physicalDevice,
                          VkInstance instance, uint32_t width, uint32_t height) {
#if !OHAO_HAS_OPTIX
    std::cout << "[Denoiser] OptiX SDK not found — denoiser unavailable" << std::endl;
    std::cout << "[Denoiser] Install OptiX SDK and rebuild to enable" << std::endl;
    m_available = false;
    return false;
#else
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;

    if (!initCuda()) {
        std::cerr << "[Denoiser] Failed to init CUDA" << std::endl;
        return false;
    }

    if (!initOptix()) {
        std::cerr << "[Denoiser] Failed to init OptiX" << std::endl;
        return false;
    }

    if (!createSharedBuffers()) {
        std::cerr << "[Denoiser] Failed to create shared buffers" << std::endl;
        return false;
    }

    m_available = true;
    std::cout << "[Denoiser] OptiX denoiser initialized (" << width << "x" << height << ")" << std::endl;
    return true;
#endif
}

#if OHAO_HAS_OPTIX

bool OptiXDenoiser::initCuda() {
    // Initialize CUDA
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        std::cerr << "[Denoiser] cuInit failed: " << res << std::endl;
        return false;
    }

    // Get CUDA device matching our Vulkan physical device
    CUdevice cudaDevice;
    res = cuDeviceGet(&cudaDevice, 0);  // TODO: match by UUID with Vulkan device
    if (res != CUDA_SUCCESS) return false;

    // Create CUDA context
    res = cuCtxCreate(reinterpret_cast<CUcontext*>(&m_cudaContext), 0, cudaDevice);
    if (res != CUDA_SUCCESS) return false;

    // Create CUDA stream
    cudaError_t err = cudaStreamCreate(reinterpret_cast<cudaStream_t*>(&m_cudaStream));
    if (err != cudaSuccess) return false;

    std::cout << "[Denoiser] CUDA initialized" << std::endl;
    return true;
}

bool OptiXDenoiser::initOptix() {
    // Initialize OptiX
    OptixResult res = optixInit();
    if (res != OPTIX_SUCCESS) {
        std::cerr << "[Denoiser] optixInit failed: " << res << std::endl;
        return false;
    }

    // Create OptiX context from CUDA context
    OptixDeviceContextOptions options = {};
    res = optixDeviceContextCreate(
        reinterpret_cast<CUcontext>(m_cudaContext),
        &options,
        reinterpret_cast<OptixDeviceContext*>(&m_optixContext));
    if (res != OPTIX_SUCCESS) {
        std::cerr << "[Denoiser] optixDeviceContextCreate failed: " << res << std::endl;
        return false;
    }

    // Create denoiser
    OptixDenoiserOptions denoiserOptions = {};
    denoiserOptions.guideAlbedo = 1;  // Use albedo guide for edge preservation
    denoiserOptions.guideNormal = 1;  // Use normal guide for edge preservation

    OptixDenoiserModelKind modelKind = OPTIX_DENOISER_MODEL_KIND_HDR;
    res = optixDenoiserCreate(
        reinterpret_cast<OptixDeviceContext>(m_optixContext),
        modelKind,
        &denoiserOptions,
        reinterpret_cast<OptixDenoiser*>(&m_optixDenoiser));
    if (res != OPTIX_SUCCESS) {
        std::cerr << "[Denoiser] optixDenoiserCreate failed: " << res << std::endl;
        return false;
    }

    // Compute memory requirements
    OptixDenoiserSizes sizes = {};
    res = optixDenoiserComputeMemoryResources(
        reinterpret_cast<OptixDenoiser>(m_optixDenoiser),
        m_width, m_height, &sizes);
    if (res != OPTIX_SUCCESS) return false;

    // Allocate state + scratch on CUDA
    cudaMalloc(reinterpret_cast<void**>(&m_stateBuffer.cudaPtr), sizes.stateSizeInBytes);
    m_stateBuffer.size = sizes.stateSizeInBytes;
    cudaMalloc(reinterpret_cast<void**>(&m_scratchBuffer.cudaPtr), sizes.withoutOverlapScratchSizeInBytes);
    m_scratchBuffer.size = sizes.withoutOverlapScratchSizeInBytes;

    // Setup denoiser
    res = optixDenoiserSetup(
        reinterpret_cast<OptixDenoiser>(m_optixDenoiser),
        reinterpret_cast<CUstream>(m_cudaStream),
        m_width, m_height,
        m_stateBuffer.cudaPtr, m_stateBuffer.size,
        m_scratchBuffer.cudaPtr, m_scratchBuffer.size);
    if (res != OPTIX_SUCCESS) {
        std::cerr << "[Denoiser] optixDenoiserSetup failed: " << res << std::endl;
        return false;
    }

    std::cout << "[Denoiser] OptiX denoiser created (HDR mode)" << std::endl;
    return true;
}

bool OptiXDenoiser::createSharedBuffers() {
    VkDeviceSize imageSize = m_width * m_height * 4 * sizeof(float);  // RGBA32F
    if (!createSharedBuffer(m_inputBuffer, imageSize)) return false;
    if (!createSharedBuffer(m_outputBuffer, imageSize)) return false;
    if (!createSharedBuffer(m_albedoGuide, imageSize)) return false;
    if (!createSharedBuffer(m_normalGuide, imageSize)) return false;
    return true;
}

bool OptiXDenoiser::createSharedBuffer(SharedBuffer& buf, VkDeviceSize size) {
    buf.size = size;

    // Create Vulkan buffer with external memory flag
    VkExternalMemoryBufferCreateInfo extInfo{};
    extInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
#ifdef _WIN32
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.pNext = &extInfo;
    bufInfo.size = size;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buf.vkBuffer) != VK_SUCCESS) return false;

    // Allocate with exportable memory
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, buf.vkBuffer, &memReqs);

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
#ifdef _WIN32
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &buf.vkMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, buf.vkBuffer, buf.vkMemory, 0);

    // Get OS handle for CUDA import
#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory = buf.vkMemory;
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)
        vkGetDeviceProcAddr(m_device, "vkGetMemoryWin32HandleKHR");
    if (!vkGetMemoryWin32HandleKHR) return false;
    vkGetMemoryWin32HandleKHR(m_device, &handleInfo, &buf.handle);

    // Import into CUDA
    cudaExternalMemoryHandleDesc extMemDesc{};
    extMemDesc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
    extMemDesc.handle.win32.handle = buf.handle;
    extMemDesc.size = memReqs.size;
    cudaExternalMemory_t cudaExtMem;
    cudaImportExternalMemory(&cudaExtMem, &extMemDesc);

    cudaExternalMemoryBufferDesc bufDesc{};
    bufDesc.size = size;
    bufDesc.offset = 0;
    cudaExternalMemoryGetMappedBuffer(reinterpret_cast<void**>(&buf.cudaPtr), cudaExtMem, &bufDesc);
#endif

    return buf.cudaPtr != 0;
}

void OptiXDenoiser::denoise(VkCommandBuffer cmd, VkQueue queue,
                             VkImage noisyImage, VkImage outputImage,
                             VkImage albedoImage, VkImage normalImage) {
    if (!m_available) return;

    VkDeviceSize imageSize = m_width * m_height * 4 * sizeof(float);

    // 1. Copy noisy image + guide images to shared buffers
    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {m_width, m_height, 1};
    vkCmdCopyImageToBuffer(cmd, noisyImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_inputBuffer.vkBuffer, 1, &copyRegion);

    // Copy guide buffers if provided
    if (albedoImage != VK_NULL_HANDLE) {
        vkCmdCopyImageToBuffer(cmd, albedoImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_albedoGuide.vkBuffer, 1, &copyRegion);
    }
    if (normalImage != VK_NULL_HANDLE) {
        vkCmdCopyImageToBuffer(cmd, normalImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_normalGuide.vkBuffer, 1, &copyRegion);
    }

    // Submit and wait — CUDA needs all data
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // 2. Run OptiX denoiser on CUDA
    OptixDenoiserParams params = {};
    params.blendFactor = 0.0f;  // 0 = full denoise, 1 = no denoise

    OptixImage2D inputLayer = {};
    inputLayer.data = m_inputBuffer.cudaPtr;
    inputLayer.width = m_width;
    inputLayer.height = m_height;
    inputLayer.rowStrideInBytes = m_width * 4 * sizeof(float);
    inputLayer.pixelStrideInBytes = 4 * sizeof(float);
    inputLayer.format = OPTIX_PIXEL_FORMAT_FLOAT4;

    OptixImage2D outputLayer = inputLayer;
    outputLayer.data = m_outputBuffer.cudaPtr;

    OptixDenoiserGuideLayer guide = {};
    // Albedo guide
    guide.albedo = inputLayer;  // same format
    guide.albedo.data = m_albedoGuide.cudaPtr;
    // Normal guide
    guide.normal = inputLayer;
    guide.normal.data = m_normalGuide.cudaPtr;

    OptixDenoiserLayer layer = {};
    layer.input = inputLayer;
    layer.output = outputLayer;

    OptixResult denoiseResult = optixDenoiserInvoke(
        reinterpret_cast<OptixDenoiser>(m_optixDenoiser),
        reinterpret_cast<CUstream>(m_cudaStream),
        &params,
        m_stateBuffer.cudaPtr, m_stateBuffer.size,
        &guide, &layer, 1,
        0, 0,
        m_scratchBuffer.cudaPtr, m_scratchBuffer.size);

    if (denoiseResult != OPTIX_SUCCESS) {
        std::cerr << "[Denoiser] optixDenoiserInvoke failed: " << denoiseResult << std::endl;
    }

    cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(m_cudaStream));

    // 3. Copy denoised result back to Vulkan
    // Need to re-record command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition output image to TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = outputImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copyBack{};
    copyBack.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyBack.imageExtent = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(cmd, m_outputBuffer.vkBuffer, outputImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyBack);

    // SUBMIT the copy-back! (was missing — denoised data never reached the image)
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo2{};
    submitInfo2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo2.commandBufferCount = 1;
    submitInfo2.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo2, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

#else // !OHAO_HAS_OPTIX

bool OptiXDenoiser::initCuda() { return false; }
bool OptiXDenoiser::initOptix() { return false; }
bool OptiXDenoiser::createSharedBuffers() { return false; }
bool OptiXDenoiser::createSharedBuffer(SharedBuffer&, VkDeviceSize) { return false; }
void OptiXDenoiser::denoise(VkCommandBuffer, VkQueue, VkImage, VkImage, VkImage, VkImage) {}

#endif // OHAO_HAS_OPTIX

void OptiXDenoiser::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    // TODO: recreate buffers and denoiser setup
    m_width = width;
    m_height = height;
}

uint32_t OptiXDenoiser::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

void OptiXDenoiser::destroySharedBuffer(SharedBuffer& buf) {
    if (buf.vkBuffer) { vkDestroyBuffer(m_device, buf.vkBuffer, nullptr); buf.vkBuffer = VK_NULL_HANDLE; }
    if (buf.vkMemory) { vkFreeMemory(m_device, buf.vkMemory, nullptr); buf.vkMemory = VK_NULL_HANDLE; }
#if OHAO_HAS_OPTIX
    if (buf.cudaPtr) { cudaFree(reinterpret_cast<void*>(buf.cudaPtr)); buf.cudaPtr = 0; }
#endif
}

void OptiXDenoiser::destroy() {
    if (!m_device) return;
#if OHAO_HAS_OPTIX
    if (m_optixDenoiser) { optixDenoiserDestroy(reinterpret_cast<OptixDenoiser>(m_optixDenoiser)); m_optixDenoiser = nullptr; }
    if (m_optixContext) { optixDeviceContextDestroy(reinterpret_cast<OptixDeviceContext>(m_optixContext)); m_optixContext = nullptr; }
    if (m_stateBuffer.cudaPtr) { cudaFree(reinterpret_cast<void*>(m_stateBuffer.cudaPtr)); m_stateBuffer.cudaPtr = 0; }
    if (m_scratchBuffer.cudaPtr) { cudaFree(reinterpret_cast<void*>(m_scratchBuffer.cudaPtr)); m_scratchBuffer.cudaPtr = 0; }
#endif
    destroySharedBuffer(m_inputBuffer);
    destroySharedBuffer(m_outputBuffer);
    destroySharedBuffer(m_albedoGuide);
    destroySharedBuffer(m_normalGuide);
    m_available = false;
}

} // namespace ohao
