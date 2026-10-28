#include "renderer_impl.hpp"
#include "core/log.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#include <cstring>  // strcmp for DLSS dedup + optional-extension probing

namespace ohao {

bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "OHAO Offscreen Renderer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "OHAO Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,  // Vulkan 1.3 for RT + buffer device address
    };

    VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };

    if (m_presentTarget) {
        for (const char* e : m_presentTarget->instanceExtensions) {
            bool have = false;
            for (const char* x : m_enabledInstanceExtensions) have |= std::strcmp(x, e) == 0;
            if (!have) m_enabledInstanceExtensions.push_back(e);
        }
        createInfo.enabledExtensionCount = static_cast<uint32_t>(m_enabledInstanceExtensions.size());
        createInfo.ppEnabledExtensionNames = m_enabledInstanceExtensions.data();
    }

    // Validation layers — enable with OHAO_VALIDATION=1 env var. Unset, empty or "0" is
    // off (a test can turn it off for one renderer by setting "0"). "sync" adds the
    // layer's synchronization validation (hazards between commands: missing barriers).
    const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* validationVar = std::getenv("OHAO_VALIDATION");
    bool wantValidation = validationVar && *validationVar && std::strcmp(validationVar, "0") != 0;
    const bool wantSyncValidation = wantValidation && std::strcmp(validationVar, "sync") == 0;
    const VkValidationFeatureEnableEXT syncFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncFeature;
    if (wantValidation) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> layers(ln);
        vkEnumerateInstanceLayerProperties(&ln, layers.data());
        bool installed = false;
        for (const auto& l : layers)
            if (std::strcmp(l.layerName, validationLayers[0]) == 0) installed = true;
        if (!installed) {
            OHAO_LOG(Error, validation,
                     "validation requested but VK_LAYER_KHRONOS_validation is not installed");
            wantValidation = false;
        }
    }
    if (wantValidation) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = validationLayers;
        m_enabledInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (wantSyncValidation) {
            m_enabledInstanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            validationFeatures.pNext = createInfo.pNext;
            createInfo.pNext = &validationFeatures;
        }
        createInfo.enabledExtensionCount = static_cast<uint32_t>(m_enabledInstanceExtensions.size());
        createInfo.ppEnabledExtensionNames = m_enabledInstanceExtensions.data();
        OHAO_LOG(Info, validation, "Validation layers ENABLED{}", wantSyncValidation ? " (+ synchronization)" : "");
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
        OHAO_LOG(Debug, gpu, "DLSS: instance ext requested: {}",
                 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
#endif

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        return false;
    }

    if (wantValidation) {
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        // Every message goes to the `validation` channel as "<VUID> | <text>", stamped with
        // the frame it happened in; tests/validation/check_validation.py reads these lines.
        mi.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                VkDebugUtilsMessageTypeFlagsEXT,
                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                void* user) -> VkBool32 {
            auto* log = static_cast<ValidationLog*>(user);
            log->count.fetch_add(1);
            const char* msg = (data && data->pMessage) ? data->pMessage : "";
            const char* vuid = (data && data->pMessageIdName) ? data->pMessageIdName : "-";
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                OHAO_LOG(Error, validation, "{} | {}", vuid, msg);
            else
                OHAO_LOG(Warn, validation, "{} | {}", vuid, msg);
            std::lock_guard<std::mutex> lock(log->mutex);
            if (log->first.size() < 256) log->first.emplace_back(msg);
            return VK_FALSE;
        };
        mi.pUserData = &m_validationMessages;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        m_validationEnabled = create && create(m_instance, &mi, nullptr, &m_debugMessenger) == VK_SUCCESS;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        OHAO_LOG(Error, gpu, "no Vulkan-capable GPU found");
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
        OHAO_LOG(Debug, gpu, "found GPU: {} (type={})", props.deviceName,
                 static_cast<int>(props.deviceType));

        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                if (m_surface != VK_NULL_HANDLE) {
                    VkBool32 canPresent = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &canPresent);
                    if (!canPresent) {
                        OHAO_LOG(Debug, gpu, "graphics queue family {} cannot present to this surface", i);
                        continue;
                    }
                }
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
        OHAO_LOG(Info, gpu, "selected GPU: {}", props.deviceName);
        return true;
    }

    return false;
}

bool VulkanRenderer::createLogicalDevice() {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_graphicsQueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

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
                OHAO_LOG(Debug, gpu, "DLSS: enabling device ext: {}", e);
            } else {
                OHAO_LOG(Warn, gpu,
                         "DLSS: device ext NOT supported by selected GPU (DLSS-RR will be unavailable): {}", e);
            }
        }
    }
#endif

    // --- Ray query + buffer float atomics: optional capabilities ---
    //
    // Enabled when present, because inline ray tracing from compute and
    // float atomicAdd into a storage buffer are both wanted by consumers that
    // are not the RT pipeline. A device lacking either still creates
    // successfully and simply cannot run those.
    bool rayQuerySupported = false;
    bool bufferAtomicAddSupported = false;
    {
        uint32_t availCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> avail(availCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, avail.data());
        auto extSupported = [&](const char* n) {
            for (const auto& e : avail) if (std::strcmp(e.extensionName, n) == 0) return true;
            return false;
        };
        const bool rqExt = extSupported(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        const bool afExt = extSupported(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
        const bool msExt = extSupported(VK_EXT_MESH_SHADER_EXTENSION_NAME);

        VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderProbe{};
        meshShaderProbe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatProbe{};
        atomicFloatProbe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
        atomicFloatProbe.pNext = msExt ? &meshShaderProbe : nullptr;

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryProbe{};
        rayQueryProbe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryProbe.pNext = &atomicFloatProbe;

        VkPhysicalDeviceFeatures2 probeFeatures2{};
        probeFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        probeFeatures2.pNext = &rayQueryProbe;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &probeFeatures2);

        rayQuerySupported = rqExt && (rayQueryProbe.rayQuery == VK_TRUE);
        m_rayQueryEnabled = rayQuerySupported;
        bufferAtomicAddSupported = afExt && (atomicFloatProbe.shaderBufferFloat32AtomicAdd == VK_TRUE);
        m_meshShaderEnabled = msExt && meshShaderProbe.taskShader == VK_TRUE && meshShaderProbe.meshShader == VK_TRUE;
        if (m_meshShaderEnabled) {
            m_enabledDeviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
            OHAO_LOG(Debug, gpu, "caps: enabling optional device ext: {}", VK_EXT_MESH_SHADER_EXTENSION_NAME);
        } else {
            OHAO_LOG(Info, gpu, "caps: no task/mesh shaders ({}): meshlets draw through the compute fallback",
                     VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }

        if (rayQuerySupported) {
            m_enabledDeviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            OHAO_LOG(Debug, gpu, "caps: enabling optional device ext: {}", VK_KHR_RAY_QUERY_EXTENSION_NAME);
        } else {
            OHAO_LOG(Warn, gpu, "caps: optional device ext NOT supported by selected GPU: {}",
                     VK_KHR_RAY_QUERY_EXTENSION_NAME);
        }
        if (bufferAtomicAddSupported) {
            m_enabledDeviceExtensions.push_back(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
            OHAO_LOG(Debug, gpu, "caps: enabling optional device ext: {}",
                     VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
        } else {
            OHAO_LOG(Warn, gpu, "caps: optional device ext NOT supported by selected GPU: {}",
                     VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
        }

        // Push descriptors: NRD's NRI wrapper records vkCmdPushDescriptorSet (core only in
        // Vulkan 1.4; this device is 1.3) and resolves it as the KHR entry point. Without
        // the extension enabled the validation layer hides both names and NRI aborts the
        // process at path tracer creation; the bare driver happened to hand them out.
        const auto enabled = [&](const char* n) {
            for (const char* e : m_enabledDeviceExtensions) if (std::strcmp(e, n) == 0) return true;
            return false;
        };
        if (extSupported(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) && !enabled(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
            m_enabledDeviceExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }

    // A window's presents can be confirmed (sub-project 9: when each frame was presented) with
    // VK_KHR_present_id and VK_KHR_present_wait; both or neither, feature bits included.
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures{};
    presentIdFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures{};
    presentWaitFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    m_presentWaitEnabled = false;
    if (m_surface != VK_NULL_HANDLE) {
        m_enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        uint32_t availCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> avail(availCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availCount, avail.data());
        const auto has = [&](const char* n) {
            for (const auto& e : avail) if (std::strcmp(e.extensionName, n) == 0) return true;
            return false;
        };
        if (has(VK_KHR_PRESENT_ID_EXTENSION_NAME) && has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            presentIdFeatures.pNext = &presentWaitFeatures;
            VkPhysicalDeviceFeatures2 probe{};
            probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            probe.pNext = &presentIdFeatures;
            vkGetPhysicalDeviceFeatures2(m_physicalDevice, &probe);
            m_presentWaitEnabled = presentIdFeatures.presentId == VK_TRUE && presentWaitFeatures.presentWait == VK_TRUE;
        }
        if (m_presentWaitEnabled) {
            m_enabledDeviceExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            m_enabledDeviceExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            OHAO_LOG(Info, gpu, "caps: present wait: presents are confirmed by the presentation engine");
        } else {
            OHAO_LOG(Info, gpu, "caps: no present wait ({} + {}): a present is stamped at its call",
                     VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    const std::vector<const char*>& deviceExtensions = m_enabledDeviceExtensions;

    // Vulkan 1.1 features: the meshlet task shader reads its draw's index (DrawIndex, the
    // DrawParameters capability).
    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;

    // Vulkan 1.2 features (buffer device address, descriptor indexing)
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features11;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    // GPU culling's draws: vkCmdDrawIndexedIndirectCount (the count from the GPU)
    features12.drawIndirectCount = VK_TRUE;

    // Vulkan 1.3 features. glslc lowers `discard` to OpDemoteToHelperInvocation for
    // SPIR-V 1.6, so shaders using discard (deferred lighting, sky, particles,
    // SVGF) need this enabled. It is mandatory on every Vulkan 1.3 device.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &features12;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    features13.synchronization2 = VK_TRUE;   // NRD's NRI records vkCmdPipelineBarrier2 (core in 1.3)

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

    // Inline ray tracing from compute, for consumers that need the whole
    // traversal inside a single function rather than split across a ray
    // tracing pipeline's stages.
    // Only linked into the pNext chain when rayQuerySupported (i.e. its
    // extension is actually enabled above) -- a feature struct in the chain
    // for an unenabled extension is invalid, regardless of the bit's value.
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    void* optionalChainTail = &rtFeatures;
    if (rayQuerySupported) {
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryFeatures.pNext = optionalChainTail;
        rayQueryFeatures.rayQuery = VK_TRUE;
        optionalChainTail = &rayQueryFeatures;
    }

    // Gradient scatter. Buffer atomics only -- gradients are never image-backed.
    // Same conditional-chain rule as above.
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    if (bufferAtomicAddSupported) {
        atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
        atomicFloatFeatures.pNext = optionalChainTail;
        atomicFloatFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;
        optionalChainTail = &atomicFloatFeatures;
    }

    // Task and mesh shaders (the G-buffer's meshlet path). Same conditional-chain rule.
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
    if (m_meshShaderEnabled) {
        meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        meshShaderFeatures.pNext = optionalChainTail;
        meshShaderFeatures.taskShader = VK_TRUE;
        meshShaderFeatures.meshShader = VK_TRUE;
        optionalChainTail = &meshShaderFeatures;
    }

    // Present id and present wait (a window, both supported). Same conditional-chain rule.
    if (m_presentWaitEnabled) {
        presentIdFeatures.presentId = VK_TRUE;
        presentWaitFeatures.presentWait = VK_TRUE;
        presentWaitFeatures.pNext = optionalChainTail;
        presentIdFeatures.pNext = &presentWaitFeatures;
        optionalChainTail = &presentIdFeatures;
    }

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = optionalChainTail;
    // Enable required device features (were previously in pEnabledFeatures)
    // Enable all features the renderer uses
    deviceFeatures2.features.samplerAnisotropy = VK_TRUE;
    deviceFeatures2.features.fillModeNonSolid = VK_TRUE;
    deviceFeatures2.features.geometryShader = VK_TRUE;
    deviceFeatures2.features.tessellationShader = VK_TRUE;
    deviceFeatures2.features.depthClamp = VK_TRUE;
    deviceFeatures2.features.wideLines = VK_TRUE;
    deviceFeatures2.features.multiDrawIndirect = VK_TRUE;
    deviceFeatures2.features.drawIndirectFirstInstance = VK_TRUE;   // firstInstance = the draw record
    // The transparent pass blends colour by the object's mode and its coverage mask by MAX.
    deviceFeatures2.features.independentBlend = VK_TRUE;
    deviceFeatures2.features.shaderInt64 = VK_TRUE;
    deviceFeatures2.features.fragmentStoresAndAtomics = VK_TRUE;
    deviceFeatures2.features.vertexPipelineStoresAndAtomics = VK_TRUE;

    // Device create info references pNext feature chain above — keep explicit assignment
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &deviceFeatures2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.pEnabledFeatures = nullptr;  // using pNext features2 chain instead
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "failed to create logical device with RT extensions");
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    if (m_presentWaitEnabled) {
        m_waitForPresent = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(m_device, "vkWaitForPresentKHR"));
        if (!m_waitForPresent) m_presentWaitEnabled = false;
    }
    OHAO_LOG(Info, gpu, "logical device created with RT extensions");
    return true;
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_graphicsQueueFamily,
    };

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VulkanRenderer::initializeFrameResources() {
    // Per-frame command buffer, fence and readback staging buffer.
    const size_t stagingBufferSize = static_cast<size_t>(m_width) * m_height * 4;   // RGBA8
    bool success = m_frameResources.initialize(m_device, m_physicalDevice, m_commandPool,
                                               stagingBufferSize);

    if (success) {
        OHAO_LOG(Debug, gpu, "multi-frame rendering enabled with {} frames in flight",
                 MAX_FRAMES_IN_FLIGHT);
    }

    return success;
}

} // namespace ohao
