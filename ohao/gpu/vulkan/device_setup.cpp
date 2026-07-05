#include "renderer_impl.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#ifdef OHAO_DLSS_ENABLED
#include <cstring>  // strcmp for DLSS extension dedup
#endif

namespace ohao {

bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "OHAO Offscreen Renderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "OHAO Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;  // Vulkan 1.3 for RT + buffer device address

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // Validation layers — enable with OHAO_VALIDATION=1 env var
    const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
    if (std::getenv("OHAO_VALIDATION")) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = validationLayers;
        m_enabledInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.enabledExtensionCount = static_cast<uint32_t>(m_enabledInstanceExtensions.size());
        createInfo.ppEnabledExtensionNames = m_enabledInstanceExtensions.data();
        std::cout << "[OHAO] Validation layers ENABLED" << std::endl;
    }

#ifdef OHAO_DLSS_ENABLED
    // DLSS-RR (NGX RayReconstruction) requires VK_KHR_get_physical_device_properties2
    // at the instance level. It is core in Vulkan 1.1+, but NGX's requirement query
    // still lists it and the RTX driver advertises it, so enabling is safe.
    // (Re)publish the enabled-extension list AFTER any push so the data() pointer
    // is never stale — the validation branch above may already have set it.
    {
        bool has = false;
        for (const char* e : m_enabledInstanceExtensions)
            if (std::strcmp(e, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) { has = true; break; }
        if (!has) m_enabledInstanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        createInfo.enabledExtensionCount   = static_cast<uint32_t>(m_enabledInstanceExtensions.size());
        createInfo.ppEnabledExtensionNames = m_enabledInstanceExtensions.data();
        std::cout << "[DLSS] instance ext requested: "
                  << VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME << std::endl;
    }
#endif

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        std::cerr << "No Vulkan-capable GPU found" << std::endl;
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    // Prefer discrete GPU (NVIDIA) over integrated (Intel)
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    uint32_t bestQueueFamily = 0;
    bool foundDiscrete = false;

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        std::cout << "Found GPU: " << props.deviceName
                  << " (type=" << props.deviceType << ")" << std::endl;

        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                bool isDiscrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
                // Always prefer discrete over integrated
                if (!foundDiscrete || isDiscrete) {
                    bestDevice = device;
                    bestQueueFamily = i;
                    foundDiscrete = isDiscrete;
                }
                break;
            }
        }
    }

    if (bestDevice != VK_NULL_HANDLE) {
        m_physicalDevice = bestDevice;
        m_graphicsQueueFamily = bestQueueFamily;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        std::cout << "Selected GPU: " << props.deviceName << std::endl;
        return true;
    }

    return false;
}

bool VulkanRenderer::createLogicalDevice() {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    // Device extensions — RT requires acceleration structure + ray tracing pipeline.
    // Sub-plan 4.C T3b: stash the enabled list on the renderer so NRD's NRI
    // device wrapper (which needs to see the exact enabled list) can read it.
    m_enabledDeviceExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,  // required by AS
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,     // required by AS
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,        // required by AS
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,                 // required by RT pipeline
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,     // required by SPIR-V 1.4
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,            // Vulkan-CUDA interop
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,      // Win32 shared memory handles
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,         // POSIX shared memory handles
#endif
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,         // Vulkan-CUDA sync
#ifdef _WIN32
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,   // Win32 shared semaphores
#else
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,      // POSIX shared semaphores
#endif
    };

#ifdef OHAO_DLSS_ENABLED
    // DLSS-RR (NGX RayReconstruction) device extensions. VK_KHR_buffer_device_address
    // is already enabled above (RT path) — skip it. Add the rest only if the chosen
    // device advertises them; if the app ever runs on the Intel iGPU (no NVX exts),
    // we simply don't add them and DLSS feature creation later returns false + falls
    // back to raw output. buffer_device_address's feature bit is already VK_TRUE below.
    {
        const char* dlssDeviceExts[] = {
            VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
            VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };
        uint32_t availCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> avail(availCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, avail.data());
        auto supported = [&](const char* n) {
            for (const auto& e : avail) if (std::strcmp(e.extensionName, n) == 0) return true;
            return false;
        };
        auto already = [&](const char* n) {
            for (const char* e : m_enabledDeviceExtensions) if (std::strcmp(e, n) == 0) return true;
            return false;
        };
        for (const char* e : dlssDeviceExts) {
            if (already(e)) continue;
            if (supported(e)) {
                m_enabledDeviceExtensions.push_back(e);
                std::cout << "[DLSS] enabling device ext: " << e << std::endl;
            } else {
                std::cerr << "[DLSS] device ext NOT supported by selected GPU (DLSS-RR will be unavailable): "
                          << e << std::endl;
            }
        }
    }
#endif

    const std::vector<const char*>& deviceExtensions = m_enabledDeviceExtensions;

    // Vulkan 1.2 features (buffer device address, descriptor indexing)
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

    // Vulkan 1.3 features (dynamic rendering, synchronization2)
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &features12;

    // Acceleration structure features
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &features13;
    asFeatures.accelerationStructure = VK_TRUE;

    // Ray tracing pipeline features
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeatures.pNext = &asFeatures;
    rtFeatures.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = &rtFeatures;
    // Enable required device features (were previously in pEnabledFeatures)
    // Enable all features the renderer uses
    deviceFeatures2.features.samplerAnisotropy = VK_TRUE;
    deviceFeatures2.features.fillModeNonSolid = VK_TRUE;
    deviceFeatures2.features.geometryShader = VK_TRUE;
    deviceFeatures2.features.tessellationShader = VK_TRUE;
    deviceFeatures2.features.depthClamp = VK_TRUE;
    deviceFeatures2.features.wideLines = VK_TRUE;
    deviceFeatures2.features.multiDrawIndirect = VK_TRUE;
    deviceFeatures2.features.shaderInt64 = VK_TRUE;
    deviceFeatures2.features.fragmentStoresAndAtomics = VK_TRUE;
    deviceFeatures2.features.vertexPipelineStoresAndAtomics = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &deviceFeatures2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.pEnabledFeatures = nullptr;  // using pNext features2 chain instead
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        std::cerr << "[OHAO] Failed to create logical device with RT extensions" << std::endl;
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    std::cout << "[OHAO] Logical device created with RT extensions" << std::endl;
    return true;
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VulkanRenderer::createSyncObjects() {
    // Legacy single fence (kept for compatibility during transition)
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_renderFence) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VulkanRenderer::initializeFrameResources() {
    // Calculate buffer sizes
    size_t cameraBufferSize = sizeof(CameraUniformBuffer);
    size_t lightBufferSize = sizeof(LightUniformBuffer);
    size_t stagingBufferSize = m_width * m_height * 4; // RGBA

    // Initialize frame resources with per-frame buffers and descriptor sets
    bool success = m_frameResources.initialize(
        m_device,
        m_physicalDevice,
        m_commandPool,
        m_descriptorSetLayout,
        m_descriptorPool,
        m_shadowImageView,
        m_shadowSampler,
        cameraBufferSize,
        lightBufferSize,
        stagingBufferSize
    );

    if (success) {
        std::cout << "Multi-frame rendering enabled with "
                  << MAX_FRAMES_IN_FLIGHT << " frames in flight" << std::endl;
    }

    return success;
}

} // namespace ohao
