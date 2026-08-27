#include "gpu_probe_context.hpp"

#include "render/rt/rt_acceleration_structure.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ohao::diff {
namespace {

// Search a few candidate locations for the compiled SPV -- the exact relative
// path depends on whether the probe is launched from the repo root or from
// its own output directory.
std::vector<uint32_t> loadSpv(const char* filename) {
    const std::vector<std::string> searchPaths = {
        std::string("bin/shaders/") + filename,
        std::string("build/Release/bin/shaders/") + filename,
        std::string("build/Debug/bin/shaders/") + filename,
        std::string("build/shaders/") + filename,
        std::string("shaders/") + filename,
    };

    std::ifstream file;
    for (const auto& p : searchPaths) {
        file.open(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) break;
    }
    if (!file.is_open()) {
        std::fprintf(stderr, "[GpuProbeContext] could not find shader SPV: %s\n", filename);
        return {};
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
    return buffer;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        const char* label = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                                 ? "ERROR"
                                 : "WARNING";
        std::fprintf(stderr, "[validation][%s] %s\n", label,
                     callbackData->pMessage ? callbackData->pMessage : "(no message)");
    }
    return VK_FALSE;
}

}  // namespace

bool GpuProbeContext::init() {
    // --- Instance layers: enable VK_LAYER_KHRONOS_validation best-effort ---
    // This is the check that would have caught an invalid device extension
    // combination automatically -- without it, the driver may silently
    // tolerate requests that are not actually spec-valid.
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

    const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, kValidationLayerName) == 0) {
            m_validationEnabled = true;
            break;
        }
    }

    std::vector<const char*> instanceLayers;
    std::vector<const char*> instanceExtensions;
    if (m_validationEnabled) {
        instanceLayers.push_back(kValidationLayerName);
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        std::printf("[GpuProbeContext] validation layer enabled: %s\n", kValidationLayerName);
    } else {
        std::fprintf(stderr,
                     "[diff_gpu_probe] WARNING: validation layers unavailable -- GPU "
                     "correctness checks are weaker\n");
    }

    // --- Instance ---
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "diff_gpu_probe";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "OHAO Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // Chain a debug messenger create-info onto the instance create-info so
    // vkCreateInstance/vkDestroyInstance themselves are covered too, not just
    // the window between vkCreateDebugUtilsMessengerEXT and teardown.
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = instanceLayers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    if (m_validationEnabled) instanceInfo.pNext = &debugCreateInfo;

    if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateInstance failed\n");
        return false;
    }

    // --- Install the persistent debug messenger (covers everything after
    // instance creation: physical device queries, device creation, all
    // subsequent GPU work) ---
    if (m_validationEnabled) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            if (createFn(m_instance, &debugCreateInfo, nullptr, &m_debugMessenger) != VK_SUCCESS) {
                std::fprintf(stderr,
                             "[GpuProbeContext] vkCreateDebugUtilsMessengerEXT failed -- "
                             "continuing without a persistent messenger\n");
            }
        } else {
            std::fprintf(stderr,
                         "[GpuProbeContext] vkGetInstanceProcAddr could not resolve "
                         "vkCreateDebugUtilsMessengerEXT\n");
        }
    }

    // --- Physical device: prefer the first discrete GPU ---
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::fprintf(stderr, "[GpuProbeContext] no Vulkan-capable device found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physicalDevice = d;
            break;
        }
    }
    if (m_physicalDevice == VK_NULL_HANDLE) {
        // No discrete GPU -- fall back to the first reported device rather
        // than failing outright.
        m_physicalDevice = devices[0];
    }

    // --- Queue family: any queue advertising compute ---
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    bool foundQueue = false;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            m_queueFamily = i;
            foundQueue = true;
            break;
        }
    }
    if (!foundQueue) {
        std::fprintf(stderr, "[GpuProbeContext] no compute-capable queue family found\n");
        return false;
    }

    // --- Logical device ---
    // The differentiable-renderer extensions are enabled unconditionally
    // here -- this binary exists to fail loudly on hardware that cannot run
    // the subsystem, so it must not fall back to the conditional probing
    // device_setup.cpp uses for the engine proper.
    //
    // VK_KHR_ray_query requires VK_KHR_acceleration_structure, which itself
    // requires VK_KHR_buffer_device_address, VK_KHR_deferred_host_operations,
    // and (for the SPIR-V it consumes) VK_KHR_spirv_1_4 +
    // VK_KHR_shader_float_controls. VK_EXT_descriptor_indexing is required by
    // acceleration_structure's update-after-bind descriptor usage. Task 6
    // builds a BLAS/TLAS against this exact context, so the whole chain is
    // enabled now rather than left for a confusing failure later. See
    // ohao/gpu/vulkan/device_setup.cpp lines ~129-152 for the engine's own
    // (superset) list this mirrors.
    const char* deviceExtensions[] = {
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    atomicFloatFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &atomicFloatFeatures;
    asFeatures.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = &asFeatures;
    rayQueryFeatures.rayQuery = VK_TRUE;

    // bufferDeviceAddress is required by acceleration structures; also needed
    // directly for Task 6. descriptorIndexing is required by the validation
    // layer whenever VK_EXT_descriptor_indexing is enabled alongside a
    // VkPhysicalDeviceVulkan12Features struct (VUID-VkDeviceCreateInfo-
    // ppEnabledExtensionNames-02833) -- device_setup.cpp sets this bit too.
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &rayQueryFeatures;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_queueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &features2;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(sizeof(deviceExtensions) / sizeof(deviceExtensions[0]));
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceCreateInfo.pEnabledFeatures = nullptr;  // using pNext features2 chain instead

    if (vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateDevice failed (ray query / acceleration "
                              "structure / shader atomic float extensions likely unsupported)\n");
        return false;
    }

    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    // --- Command pool ---
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateCommandPool failed\n");
        return false;
    }

    // --- Allocator ---
    if (!m_allocator.initialize(m_instance, m_physicalDevice, m_device)) {
        std::fprintf(stderr, "[GpuProbeContext] GpuAllocator::initialize failed\n");
        return false;
    }

    return true;
}

void GpuProbeContext::shutdown() {
    if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);

    m_allocator.shutdown();

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    m_physicalDevice = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
}

void GpuProbeContext::runImmediate(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkAllocateCommandBuffers failed\n");
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    fn(cmd);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

bool GpuProbeContext::runAtomicProbe(GradientArena& arena, uint32_t targetIndex,
                                     uint32_t invocations) {
    struct PushConstants {
        uint32_t targetIndex;
        uint32_t invocationCount;
    };

    const std::vector<uint32_t> spv = loadSpv("diff_atomic_probe.comp.spv");
    if (spv.empty()) return false;

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spv.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateShaderModule failed\n");
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateDescriptorSetLayout failed\n");
        vkDestroyShaderModule(m_device, shaderModule, nullptr);
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreatePipelineLayout failed\n");
        vkDestroyDescriptorSetLayout(m_device, setLayout, nullptr);
        vkDestroyShaderModule(m_device, shaderModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool pipelineOk =
        vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) ==
        VK_SUCCESS;
    if (!pipelineOk) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateComputePipelines failed\n");
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.maxSets = 1;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    bool ok = pipelineOk;
    if (ok) {
        ok = vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &descPool) == VK_SUCCESS;
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] vkCreateDescriptorPool failed\n");
    }

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = descPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &setLayout;
        ok = vkAllocateDescriptorSets(m_device, &descAllocInfo, &descSet) == VK_SUCCESS;
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] vkAllocateDescriptorSets failed\n");
    }

    if (ok) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = arena.buffer();
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        const PushConstants push{targetIndex, invocations};
        const uint32_t groupCount = (invocations + 63) / 64;

        runImmediate([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                     &descSet, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                                &push);
            vkCmdDispatch(cmd, groupCount, 1, 1);

            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = arena.buffer();
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &barrier, 0, nullptr);
        });
    }

    if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, descPool, nullptr);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, setLayout, nullptr);
    vkDestroyShaderModule(m_device, shaderModule, nullptr);

    return ok;
}

bool GpuProbeContext::runVisibilityProbe(float planeDistance, uint32_t width, uint32_t height,
                                         float tanHalfFov, std::vector<float>& outHits,
                                         float quadMinY) {
    // Push constants: must byte-match shaders/diff/visibility_probe.comp's Push
    // block exactly (80 bytes -- four vec3+pad quads plus a trailing quad of
    // width/height/tanHalfFov/pad).
    struct PushConstants {
        float origin[3];
        float pad0;
        float forward[3];
        float pad1;
        float right[3];
        float pad2;
        float up[3];
        float pad3;
        uint32_t width;
        uint32_t height;
        float tanHalfFov;
        float pad4;
    };
    static_assert(sizeof(PushConstants) == 80,
                 "PushConstants must match visibility_probe.comp's Push block layout");

    outHits.clear();

    // --- Quad geometry: x in [-1,1], y in [quadMinY,1] at z = -planeDistance ---
    const float d = planeDistance;
    const std::array<float, 12> positions = {
        -1.0f, quadMinY, -d,
         1.0f, quadMinY, -d,
         1.0f,  1.0f,    -d,
        -1.0f,  1.0f,    -d,
    };
    const std::array<uint32_t, 6> indices = {0, 1, 2, 0, 2, 3};

    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        std::span<const uint32_t>(indices),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    bool ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: failed to create quad "
                              "vertex/index buffers\n");
    }

    // --- BLAS/TLAS ---
    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool)) {
        std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: RTAccelerationStructure::init "
                              "failed (ray tracing not supported on this device)\n");
        ok = false;
    }

    BlasHandle blas = INVALID_BLAS;
    if (ok) {
        runImmediate([&](VkCommandBuffer cmd) {
            blas = accel.createBLASFromPositions(vertexBuffer.buffer,
                                                 static_cast<uint32_t>(positions.size() / 3),
                                                 indexBuffer.buffer,
                                                 static_cast<uint32_t>(indices.size()),
                                                 /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: createBLASFromPositions "
                                  "failed\n");
            ok = false;
        }
    }

    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: buildTLAS produced no "
                                  "TLAS\n");
            ok = false;
        }
    }

    // --- Hit buffer (host-visible so we can read it back directly) ---
    const VkDeviceSize hitsSize =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * sizeof(float);
    GpuBuffer hitsBuffer;
    if (ok) {
        hitsBuffer = m_allocator.createBuffer(hitsSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              AllocationUsage::GpuToCpu,
                                              /*persistentlyMapped=*/true);
        if (!hitsBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: hit buffer allocation "
                                  "failed\n");
            ok = false;
        }
    }

    // --- Shader module / pipeline ---
    const std::vector<uint32_t> spv = ok ? loadSpv("diff_visibility_probe.comp.spv")
                                          : std::vector<uint32_t>{};
    if (ok && spv.empty()) ok = false;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (ok) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spv.size() * sizeof(uint32_t);
        moduleInfo.pCode = spv.data();
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: vkCreateShaderModule "
                                  "failed\n");
            ok = false;
        }
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: "
                                  "vkCreateDescriptorSetLayout failed\n");
            ok = false;
        }
    }

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (ok) {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipelineLayout) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: vkCreatePipelineLayout "
                                  "failed\n");
            ok = false;
        }
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = shaderModule;
        stageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = pipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                     &pipeline) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: "
                                  "vkCreateComputePipelines failed\n");
            ok = false;
        }
    }

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo descPoolInfo{};
        descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descPoolInfo.maxSets = 1;
        descPoolInfo.poolSizeCount = 2;
        descPoolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &descPool) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: vkCreateDescriptorPool "
                                  "failed\n");
            ok = false;
        }
    }

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = descPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &setLayout;
        if (vkAllocateDescriptorSets(m_device, &descAllocInfo, &descSet) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: "
                                  "vkAllocateDescriptorSets failed\n");
            ok = false;
        }
    }

    if (ok) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = hitsBuffer.buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkAccelerationStructureKHR tlas = accel.getTLAS();
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].pNext = &asWrite;
        writes[1].dstSet = descSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

        PushConstants push{};
        push.origin[0] = 0.0f; push.origin[1] = 0.0f; push.origin[2] = 0.0f;
        push.forward[0] = 0.0f; push.forward[1] = 0.0f; push.forward[2] = -1.0f;
        push.right[0] = 1.0f; push.right[1] = 0.0f; push.right[2] = 0.0f;
        push.up[0] = 0.0f; push.up[1] = 1.0f; push.up[2] = 0.0f;
        push.width = width;
        push.height = height;
        push.tanHalfFov = tanHalfFov;

        const uint32_t groupsX = (width + 7) / 8;
        const uint32_t groupsY = (height + 7) / 8;

        runImmediate([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                    &descSet, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);

            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = hitsBuffer.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &barrier, 0, nullptr);
        });

        m_allocator.invalidateBuffer(hitsBuffer);
        const auto* mapped = static_cast<const float*>(hitsBuffer.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runVisibilityProbe: hit buffer not mapped, "
                                  "cannot read back\n");
            ok = false;
        } else {
            outHits.assign(mapped, mapped + (static_cast<std::size_t>(width) * height));
        }
    }

    // --- Cleanup ---
    if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, descPool, nullptr);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, setLayout, nullptr);
    if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, shaderModule, nullptr);
    if (hitsBuffer.isValid()) m_allocator.destroyBuffer(hitsBuffer);
    // accel (BLAS/TLAS/scratch/instance buffers) is destroyed by its own
    // destructor when it goes out of scope here.
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
