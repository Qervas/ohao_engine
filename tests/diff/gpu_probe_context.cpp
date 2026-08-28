#include "gpu_probe_context.hpp"

#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"
#include "render/rt/rt_acceleration_structure.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ohao::diff {
namespace {

// loadSpv (search a few candidate locations for the compiled SPV) now lives
// in ohao::diff::loadSpv (compute_pipeline.hpp/.cpp) -- this file used to
// keep a byte-identical private copy, which is called at every `loadSpv(...)`
// site below via unqualified lookup into the enclosing ohao::diff namespace.

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

    // Synchronization validation finds read-after-write and write-after-write
    // hazards that plain validation does not. Enabled here deliberately before
    // the wavefront stages exist: this subsystem hand-writes its barriers, and
    // the engine's RenderGraph -- the alternative -- reports 29 such hazards.
    const VkValidationFeatureEnableEXT enabledFeatures[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validationFeatures{};

    if (m_validationEnabled) {
        instanceLayers.push_back(kValidationLayerName);
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        std::printf("[GpuProbeContext] validation layer enabled: %s\n", kValidationLayerName);

        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = enabledFeatures;
        std::printf("[GpuProbeContext] synchronization validation enabled\n");
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
    if (m_validationEnabled) {
        validationFeatures.pNext = &debugCreateInfo;
        instanceInfo.pNext = &validationFeatures;
    }

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

bool GpuProbeContext::dispatchStorageBufferCompute(const char* spvName, VkBuffer buffer,
                                                   const void* pushData, uint32_t pushSize,
                                                   uint32_t groupCountX) {
    // Task 1 (Stage 0b-2a) lifted the hand-rolled shader-module ->
    // descriptor-set-layout -> pipeline-layout -> pipeline -> descriptor-
    // pool -> descriptor-set sequence (52 such calls existed across this
    // file before the extraction) into ohao::diff::ComputePipeline. Task 4
    // goes one step further: WavefrontStage wraps that same ComputePipeline
    // plus the bind/push/dispatch sequence below, so this is now a client of
    // the library's dispatchable-stage abstraction rather than assembling
    // that sequence by hand from ComputePipeline's primitives itself.
    WavefrontStage stage;
    const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!stage.build(m_device, spvName, bindingTypes, pushSize)) {
        // build() already released anything it partially created.
        return false;
    }

    const VkBuffer buffers[] = {buffer};
    bool ok = stage.bindBuffers(m_device, buffers);
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] dispatchStorageBufferCompute: bindBuffers failed\n");
    }

    if (ok) {
        stage.setPushConstants(pushData, pushSize);
        stage.setGroupCount(WavefrontStage::Fixed{groupCountX});

        runImmediate([&](VkCommandBuffer cmd) {
            stage.record(cmd);

            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &barrier, 0, nullptr);
        });
    }

    // Every path -- success or any failure above -- destroys the stage
    // once, unconditionally, here; destroy() is idempotent so this is safe
    // even though build() may already have partially cleaned up on failure.
    stage.destroy(m_device);

    return ok;
}

bool GpuProbeContext::runAtomicProbe(GradientArena& arena, uint32_t targetIndex,
                                     uint32_t invocations) {
    struct PushConstants {
        uint32_t targetIndex;
        uint32_t invocationCount;
    } push{targetIndex, invocations};
    return dispatchStorageBufferCompute("diff_atomic_probe.comp.spv", arena.buffer(),
                                        &push, sizeof(push), (invocations + 63u) / 64u);
}

bool GpuProbeContext::runRngParityProbe(uint32_t pixelIndex, uint32_t sampleIndex,
                                        uint32_t iterationSeed, uint32_t drawCount,
                                        GradientArena& scratch, std::size_t blockIndex,
                                        std::vector<float>& outDraws) {
    struct PushConstants {
        uint32_t pixelIndex;
        uint32_t sampleIndex;
        uint32_t iterationSeed;
        uint32_t drawCount;
    } push{pixelIndex, sampleIndex, iterationSeed, drawCount};
    // rng_probe.comp writes from invocation 0 only, so one group suffices.
    if (!dispatchStorageBufferCompute("diff_rng_probe.comp.spv", scratch.buffer(),
                                      &push, sizeof(push), 1u)) {
        return false;
    }
    outDraws = scratch.readback(allocator(), blockIndex);
    return outDraws.size() >= drawCount;
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
    // This context enables VK_KHR_ray_query but NOT VK_KHR_ray_tracing_pipeline,
    // so the post-build barrier must name COMPUTE_SHADER only. Naming the
    // ray-tracing stage without its extension is invalid usage.
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
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

bool GpuProbeContext::runWavefrontGenerateProbe(WavefrontBuffers& buffers, uint32_t width,
                                                uint32_t height,
                                                const WavefrontGenerateCamera& camera,
                                                std::vector<uint32_t>& outQueue0) {
    // Push constants: must byte-match shaders/diff/wf_generate.comp's Push
    // block exactly (80 bytes -- same four vec3+pad quads as
    // visibility_probe.comp's Push, with a trailing width/height/tanHalfFov/
    // capacity quad in place of visibility_probe's width/height/tanHalfFov/pad).
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
        uint32_t capacity;
    };
    static_assert(sizeof(PushConstants) == 80,
                 "PushConstants must match wf_generate.comp's Push block layout");

    outQueue0.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: buffers not built\n");
        return false;
    }

    // --- state (0), queue (1), counter (2), via the wavefront execution
    // library (ComputePipeline/WavefrontStage) rather than a hand-rolled
    // shader-module -> layout -> pipeline -> pool -> set sequence. ---
    WavefrontStage generate;
    const VkDescriptorType bindingTypes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    ok = generate.build(m_device, "diff_wf_generate.comp.spv", bindingTypes, sizeof(PushConstants));
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: generate build\n");
        return false;
    }
    const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                           buffers.counterBuffer()};
    ok = generate.bindBuffers(m_device, stateQueueCounter);
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: bindBuffers failed\n");
        generate.destroy(m_device);
        return false;
    }

    // --- Readback buffer for queue 0 (host-visible, this function's own) ---
    GpuBuffer queueReadback;
    const VkDeviceSize queue0Bytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queue0Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        PushConstants push{};
        push.origin[0] = camera.origin[0]; push.origin[1] = camera.origin[1]; push.origin[2] = camera.origin[2];
        push.forward[0] = camera.forward[0]; push.forward[1] = camera.forward[1]; push.forward[2] = camera.forward[2];
        push.right[0] = camera.right[0]; push.right[1] = camera.right[1]; push.right[2] = camera.right[2];
        push.up[0] = camera.up[0]; push.up[1] = camera.up[1]; push.up[2] = camera.up[2];
        push.width = width;
        push.height = height;
        push.tanHalfFov = camera.tanHalfFov;
        push.capacity = capacity;
        generate.setPushConstants(&push, sizeof(push));

        // Genuinely 2-D: wf_generate.comp is local_size(8,8), and this probe
        // covers a width x height pixel grid, not just local_size_y rows of
        // it -- so the 3-D-widened WavefrontStage::Fixed is used here (see
        // wavefront_stage.hpp), unlike the fused-loop probe's 1-D
        // Fixed{width/8} (which is restricted to height == 8).
        const uint32_t groupsX = (width + 7) / 8;
        const uint32_t groupsY = (height + 7) / 8;
        generate.setGroupCount(WavefrontStage::Fixed{groupsX, groupsY, 1});

        runImmediate([&](VkCommandBuffer cmd) {
            generate.record(cmd);

            // The dispatch wrote (shader-write) the state and queue buffers
            // and read-modify-wrote (atomicAdd) the counter buffer. Two
            // different consumers follow: the caller reads state/counter
            // back through WavefrontBuffers' own persistently-mapped host
            // pointers (needs HOST_READ), and this function copies queue 0
            // out via vkCmdCopyBuffer (needs TRANSFER_READ) because
            // WavefrontBuffers exposes only a raw VkBuffer for it. One
            // barrier naming both destination stages/accesses covers both.
            VkBufferMemoryBarrier postDispatch[3]{};
            VkBuffer writtenBuffers[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                          buffers.counterBuffer()};
            for (int i = 0; i < 3; ++i) {
                postDispatch[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postDispatch[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                postDispatch[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                postDispatch[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postDispatch[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postDispatch[i].buffer = writtenBuffers[i];
                postDispatch[i].offset = 0;
                postDispatch[i].size = VK_WHOLE_SIZE;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 3, postDispatch, 0, nullptr);

            // Copy queue 0 (the first `capacity` uints of the queue buffer)
            // into this function's own host-visible buffer.
            VkBufferCopy region{};
            region.srcOffset = 0;
            region.dstOffset = 0;
            region.size = queue0Bytes;
            vkCmdCopyBuffer(cmd, buffers.queueBuffer(), queueReadback.buffer, 1, &region);

            VkBufferMemoryBarrier postCopy{};
            postCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            postCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.buffer = queueReadback.buffer;
            postCopy.offset = 0;
            postCopy.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 0, nullptr, 1, &postCopy, 0, nullptr);
        });

        m_allocator.invalidateBuffer(queueReadback);
        const auto* mapped = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueue0.assign(mapped, mapped + capacity);
        }
    }

    // --- Cleanup ---
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    generate.destroy(m_device);

    return ok;
}

bool GpuProbeContext::runWavefrontLayoutProbe(WavefrontBuffers& buffers) {
    struct PushConstants {
        uint32_t capacity;
    };
    const PushConstants push{buffers.layout().capacity()};
    // Single storage buffer (state, binding 0) + push constants + one
    // invocation: exactly what dispatchStorageBufferCompute already does for
    // runAtomicProbe/runRngParityProbe.
    return dispatchStorageBufferCompute("diff_wf_layout_probe.comp.spv", buffers.stateBuffer(),
                                        &push, sizeof(push), /*groupCountX=*/1u);
}

bool GpuProbeContext::runWavefrontIntersectProbe(WavefrontBuffers& buffers, float planeDistance,
                                                 float quadMinY, std::vector<uint32_t>& outQueue1) {
    outQueue1.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: buffers not built\n");
        return false;
    }

    // --- Quad geometry: x in [-1,1], y in [quadMinY,1] at z = -planeDistance,
    // the same shape runVisibilityProbe builds. ---
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

    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: failed to create quad "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                              "RTAccelerationStructure::init failed\n");
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }

    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- prepare_indirect (counter only) and intersect (state, queue,
    // counter, AS), via the wavefront execution library rather than a
    // hand-rolled shader-module -> layout -> pipeline -> pool -> set
    // sequence for each. Reuses WavefrontLoop::PrepareIndirectPush/
    // IntersectPush -- byte-identical to this function's former private
    // PrepPush/IntersectPush -- rather than redeclaring them. ---
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    const VkDescriptorType counterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType intersectBindingTypes[4] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};

    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", counterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", intersectBindingTypes,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer()};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 3, accel.getTLAS())) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontIntersectProbe: descriptor binding\n");
            ok = false;
        }
    }

    // --- Readback buffer for queue ring 1 (host-visible, this function's own) ---
    GpuBuffer queueReadback;
    const VkDeviceSize queue1Bytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queue1Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        const WavefrontLoop::IntersectPush intersectPush{capacity,
                                                          /*srcQueueBase=*/0u,
                                                          WavefrontBuffers::kCurrentCountSlot,
                                                          /*dstQueueBase=*/capacity,
                                                          WavefrontBuffers::kNextCountSlot,
                                                          WavefrontBuffers::kCanarySlot};
        intersect.setPushConstants(&intersectPush, sizeof(intersectPush));

        runImmediate([&](VkCommandBuffer cmd) {
            // --- prepare_indirect: counter[kCurrentCountSlot] ->
            // counter[argsSlot..+2] -> the COMPUTE_SHADER -> DRAW_INDIRECT /
            // INDIRECT_COMMAND_READ barrier ordering that write before
            // vkCmdDispatchIndirect reads it (the one barrier in this
            // subsystem anything here is proven to detect the absence of --
            // see task-5-report.md) -> intersect, dispatched indirectly from
            // the triple just made visible. Shared with
            // WavefrontLoop::recordCompactingStage and
            // runWavefrontScatterProbe -- see recordIndirectSizedDispatch's
            // doc comment in wavefront_loop.hpp for the full account of why
            // each piece is required, including the kIndirectArgsSlot
            // disjointness invariant the lone INDIRECT_COMMAND_READ
            // dstAccessMask depends on. ---
            recordIndirectSizedDispatch(cmd, buffers.counterBuffer(),
                                        WavefrontBuffers::kCurrentCountSlot, prepareIndirect,
                                        intersect);

            // intersect's writes (state Alive/HitT, queue ring 1, counter
            // slots next-count/canary) must become visible to what reads
            // them next: this function's own host readback of state/counter
            // through WavefrontBuffers' persistently-mapped pointers
            // (HOST_READ), and the vkCmdCopyBuffer below that pulls queue
            // ring 1 out into a buffer this function owns (TRANSFER_READ).
            VkBufferMemoryBarrier postIntersect[2]{};
            VkBuffer writtenBuffers[2] = {buffers.stateBuffer(), buffers.counterBuffer()};
            for (int i = 0; i < 2; ++i) {
                postIntersect[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postIntersect[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                postIntersect[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                postIntersect[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postIntersect[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postIntersect[i].buffer = writtenBuffers[i];
                postIntersect[i].offset = 0;
                postIntersect[i].size = VK_WHOLE_SIZE;
            }
            VkBufferMemoryBarrier queueToTransfer{};
            queueToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            queueToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            queueToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            queueToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.buffer = buffers.queueBuffer();
            queueToTransfer.offset = 0;
            queueToTransfer.size = VK_WHOLE_SIZE;

            VkBufferMemoryBarrier postDispatch[3] = {postIntersect[0], postIntersect[1],
                                                     queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 3, postDispatch, 0, nullptr);

            // Copy queue ring 1 (elements [capacity, 2*capacity)) into this
            // function's own host-visible buffer.
            VkBufferCopy region{};
            region.srcOffset = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
            region.dstOffset = 0;
            region.size = queue1Bytes;
            vkCmdCopyBuffer(cmd, buffers.queueBuffer(), queueReadback.buffer, 1, &region);

            VkBufferMemoryBarrier postCopy{};
            postCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            postCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.buffer = queueReadback.buffer;
            postCopy.offset = 0;
            postCopy.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 0, nullptr, 1, &postCopy, 0, nullptr);
        });

        m_allocator.invalidateBuffer(queueReadback);
        const auto* mapped = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueue1.assign(mapped, mapped + capacity);
        }
    }

    // --- Cleanup, reverse order. ---
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

bool GpuProbeContext::runWavefrontScatterProbe(WavefrontBuffers& buffers, uint32_t srcQueueBase,
                                               uint32_t srcCountSlot, uint32_t dstQueueBase,
                                               uint32_t dstCountSlot, float albedo,
                                               uint32_t iterationSeed,
                                               std::vector<uint32_t>& outQueueDst,
                                               std::vector<float>& outDebugDraws) {
    outQueueDst.clear();
    outDebugDraws.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: buffers not built\n");
        return false;
    }

    // --- prepare_indirect (counter only) and scatter (state, queue,
    // counter, debug draws), via the wavefront execution library rather
    // than a hand-rolled shader-module -> layout -> pipeline -> pool -> set
    // sequence for each. Reuses WavefrontLoop::PrepareIndirectPush/
    // ScatterPush -- byte-identical to this function's former private
    // PrepPush/ScatterPush -- rather than redeclaring them. ---
    WavefrontStage prepareIndirect;
    WavefrontStage scatter;
    const VkDescriptorType counterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType scatterBindingTypes[4] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (!prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", counterOnly,
                               sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                              "build\n");
        return false;
    }
    if (!scatter.build(m_device, "diff_wf_scatter.comp.spv", scatterBindingTypes,
                       sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter build\n");
        prepareIndirect.destroy(m_device);
        return false;
    }

    // --- Readback buffers, owned by this function: dst queue ring (via
    // vkCmdCopyBuffer, same reasoning as runWavefrontIntersectProbe -- the
    // queue buffer is only exposed as a raw VkBuffer) and DebugDraws (host-
    // visible directly, since this buffer is allocated by this function and
    // never shared, so it can just be GpuToCpu-mapped like
    // runVisibilityProbe's hit buffer -- no copy needed). ---
    GpuBuffer queueReadback;
    const VkDeviceSize queueBytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queueBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    GpuBuffer debugDrawsBuffer;
    const VkDeviceSize debugDrawsBytes = static_cast<VkDeviceSize>(capacity) * 3u * sizeof(float);
    if (ok) {
        debugDrawsBuffer = m_allocator.createBuffer(debugDrawsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    AllocationUsage::GpuToCpu,
                                                    /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: debug draws buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer scatterBuffers[4] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                            buffers.counterBuffer(), debugDrawsBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !scatter.bindBuffers(m_device, scatterBuffers)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontScatterProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        const WavefrontLoop::ScatterPush scatterPush{capacity,      srcQueueBase, srcCountSlot,
                                                      dstQueueBase,  dstCountSlot, albedo,
                                                      iterationSeed};
        scatter.setPushConstants(&scatterPush, sizeof(scatterPush));
        const VkDeviceSize dstSlotOffset =
            static_cast<VkDeviceSize>(dstCountSlot) * sizeof(uint32_t);

        runImmediate([&](VkCommandBuffer cmd) {
            // --- Clear dstCountSlot to 0 before anything reads or writes it.
            // Unlike wf_intersect's checks (always a fresh, zeroed
            // WavefrontBuffers, ring0 -> ring1 exactly once), a multi-bounce
            // caller ping-pongs the SAME two physical rings across many
            // scatter calls, so this slot generally holds a stale prior
            // count -- reusing it as the atomicAdd base would silently
            // corrupt every compaction offset after the first entry. ---
            vkCmdFillBuffer(cmd, buffers.counterBuffer(), dstSlotOffset, sizeof(uint32_t), 0u);

            VkBufferMemoryBarrier fillBarrier{};
            fillBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            fillBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fillBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fillBarrier.buffer = buffers.counterBuffer();
            fillBarrier.offset = 0;
            fillBarrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                 &fillBarrier, 0, nullptr);

            // --- prepare_indirect: counter[srcCountSlot] ->
            // counter[argsSlot..+2] -> the COMPUTE_SHADER -> DRAW_INDIRECT /
            // INDIRECT_COMMAND_READ barrier task-5-report.md documents as
            // load-bearing (wf_prepare_indirect's write of the dispatch-args
            // triple must be visible to vkCmdDispatchIndirect's read before
            // that read happens -- INDIRECT_COMMAND_READ, not HOST_READ or
            // SHADER_READ; this is the one barrier in this subsystem
            // anything here is proven to detect the absence of) -> scatter,
            // dispatched indirectly from the triple just made visible,
            // re-queuing into (dstQueueBase, dstCountSlot). Shared with
            // WavefrontLoop::recordCompactingStage and
            // runWavefrontIntersectProbe -- see recordIndirectSizedDispatch's
            // doc comment in wavefront_loop.hpp for the full account. ---
            recordIndirectSizedDispatch(cmd, buffers.counterBuffer(), srcCountSlot, prepareIndirect,
                                        scatter);

            // scatter's writes (state origin/dir/throughput/bounce, queue
            // dst ring, counter dstCountSlot, DebugDraws) must become
            // visible to what reads them next: this function's own host
            // readback of state/counter through WavefrontBuffers'
            // persistently-mapped pointers and of DebugDraws through this
            // function's own mapped buffer (HOST_READ), and the
            // vkCmdCopyBuffer below that pulls the dst queue ring out
            // (TRANSFER_READ).
            VkBufferMemoryBarrier postScatter[3]{};
            VkBuffer writtenBuffers[3] = {buffers.stateBuffer(), buffers.counterBuffer(),
                                          debugDrawsBuffer.buffer};
            for (int i = 0; i < 3; ++i) {
                postScatter[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postScatter[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                postScatter[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                postScatter[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postScatter[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postScatter[i].buffer = writtenBuffers[i];
                postScatter[i].offset = 0;
                postScatter[i].size = VK_WHOLE_SIZE;
            }
            VkBufferMemoryBarrier queueToTransfer{};
            queueToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            queueToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            queueToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            queueToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.buffer = buffers.queueBuffer();
            queueToTransfer.offset = 0;
            queueToTransfer.size = VK_WHOLE_SIZE;

            VkBufferMemoryBarrier postDispatch[4] = {postScatter[0], postScatter[1], postScatter[2],
                                                     queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 4, postDispatch, 0, nullptr);

            // Copy the dst queue ring (elements [dstQueueBase,
            // dstQueueBase+capacity)) into this function's own host-visible
            // buffer.
            VkBufferCopy region{};
            region.srcOffset = static_cast<VkDeviceSize>(dstQueueBase) * sizeof(uint32_t);
            region.dstOffset = 0;
            region.size = queueBytes;
            vkCmdCopyBuffer(cmd, buffers.queueBuffer(), queueReadback.buffer, 1, &region);

            VkBufferMemoryBarrier postCopy{};
            postCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            postCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.buffer = queueReadback.buffer;
            postCopy.offset = 0;
            postCopy.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 0, nullptr, 1, &postCopy, 0, nullptr);
        });

        m_allocator.invalidateBuffer(queueReadback);
        const auto* mappedQueue = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mappedQueue == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueueDst.assign(mappedQueue, mappedQueue + capacity);
        }

        m_allocator.invalidateBuffer(debugDrawsBuffer);
        const auto* mappedDebug = static_cast<const float*>(debugDrawsBuffer.getMappedData());
        if (mappedDebug == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: debug draws buffer "
                                  "not mapped, cannot read back\n");
            ok = false;
        } else {
            outDebugDraws.assign(mappedDebug, mappedDebug + (static_cast<std::size_t>(capacity) * 3u));
        }
    }

    // --- Cleanup, reverse order. ---
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    scatter.destroy(m_device);
    prepareIndirect.destroy(m_device);

    return ok;
}

// --- Fused-loop probe scene constants -------------------------------------
//
// wf_scatter.comp's placeholder BSDF samples a cosine hemisphere about a
// HARDCODED (0,0,1) normal and offsets the new origin along that normal, so
// every scattered ray has dir.z > 0 and every path marches monotonically in
// +Z. One quad is therefore hit at bounce 0 and missed by every bounce after
// it. A staircase of quads perpendicular to Z, entered from -Z, is what
// keeps every path alive for the whole loop.
namespace {

/// One quad per bounce, plus slack so that a hit point landing slightly past
/// its intended plane (float error in a near-grazing t) still finds another
/// plane ahead of it instead of falling out of the scene.
constexpr uint32_t kFusedLoopPlaneCount = 8;

/// z of the nearest-to-the-camera plane, and the gap between planes.
///
/// The gap is the whole safety argument. wf_intersect.comp traces with
/// tmax = 1000, and the shallowest direction the RNG can produce is
/// dir.z = sqrt(1 - u1max) = sqrt(2^-24) = 2^-12, since diffRngNext1D
/// returns (state >> 8) * 2^-24 and so cannot exceed 1 - 2^-24. Crossing a
/// gap g at that angle costs t = g / 2^-12 = 4096*g and drifts the same
/// distance sideways. With g = 0.05 that is t <= 205 (well inside tmax) and
/// a lateral drift under 205 per SCATTERED bounce (every bounce after the
/// first -- the first intersect traces the camera's own primary ray, which
/// has a fixed, known direction, not the RNG's worst case).
///
/// That per-bounce figure is NOT "covered many times over" by the
/// half-extent below once it accumulates: at this repo's own probe's 4
/// bounces (3 scattered intersects), worst case is roughly
/// 3 * 204.4 + a small primary-ray footprint =~ 615 units against a 1024
/// half-extent -- about a 1.7x margin, not an "many times over" one. At 6
/// bounces the worst case is already ~1022, i.e. AT the half-extent; at 7 it
/// exceeds it. See kFusedLoopSceneSafeForBounces() below and the runtime guard in
/// runWavefrontFusedLoopProbe that keeps a raised maxBounces from silently
/// turning "every path survives" from a guarantee into a probability. A
/// larger gap would eventually push a grazing ray past tmax and silently
/// kill it; a much smaller one would collide with wf_scatter.comp's 1e-4
/// origin offset.
constexpr float kFusedLoopFirstPlaneZ = -2.0f;
constexpr float kFusedLoopPlaneGap = 0.05f;

/// Half-extent of each quad in x and y. Big enough that no path can drift
/// off the side of the staircase (see the gap note above), which is what
/// makes "every one of the capacity paths survives every bounce" a fact
/// rather than a probability -- PROVIDED maxBounces stays within what
/// kFusedLoopSceneSafeForBounces() allows; see the runtime guard below. The
/// planes are exactly axis-aligned, so their plane equation is exact
/// regardless of how large they are.
constexpr float kFusedLoopHalfExtent = 1024.0f;

/// wf_generate.comp's local_size_y. WavefrontStage's Fixed group-count
/// source now supports a genuine 3-D dispatch (groups/groupsY/groupsZ, not
/// just groups), but this probe's own generate dispatch (see
/// generate.setGroupCount below) only ever sets `groups` and leaves
/// `groupsY`/`groupsZ` at Fixed's default of 1 -- i.e. it still dispatches
/// (groupCountX, 1, 1) -- so a fixed dispatch of that shader covers exactly
/// this many pixel rows. See the height check below. Widening this probe to
/// a genuine 2-D dispatch (e.g. 64x48) is a possible follow-up; it is not
/// done here because this probe's expected values (throughput, per-bounce
/// PathRng parity, live counts) are all calibrated to 512 paths at the
/// current resolution.
constexpr uint32_t kFusedLoopGenerateLocalY = 8;

/// The shallowest z-direction the RNG can produce, per the gap-note
/// derivation above: dir.z = sqrt(1 - u1max) = sqrt(2^-24) = 2^-12.
constexpr float kFusedLoopMinDirZ = 1.0f / 4096.0f;

/// wf_scatter.comp's ray-origin epsilon offset along the normal, subtracted
/// from the gap before computing worst-case crossing distance (matches the
/// gap-note derivation: "a much smaller [gap] would collide with
/// wf_scatter.comp's 1e-4 origin offset").
constexpr float kFusedLoopScatterOriginOffset = 1e-4f;

/// Generous, deliberately loose bound on how far the camera's own primary
/// ray footprint (its origin/frustum spread across `width*height` pixels)
/// can be from the staircase's centerline, added once to the worst-case
/// scattered-bounce drift below. It is not derived as tightly as the
/// per-bounce figure because the scattered-bounce term dominates for any
/// maxBounces worth guarding.
constexpr float kFusedLoopPrimarySpreadSlack = 2.0f;

/// Worst-case total lateral drift (in scene units) a path can accumulate
/// over `maxBounces` fused bounces, given the RNG's shallowest possible
/// direction every time. A run of `maxBounces` bounces contains
/// `maxBounces - 1` SCATTERED intersects (the first intersect traces the
/// camera's own primary ray, not a scattered one), each of which can drift
/// up to `(kFusedLoopPlaneGap - kFusedLoopScatterOriginOffset) /
/// kFusedLoopMinDirZ` units sideways -- see the gap-note derivation above.
///
/// `maxBounces` is a runtime parameter of `runWavefrontFusedLoopProbe` (see
/// its signature), not a value known at this translation unit's compile
/// time, so this cannot be enforced with a `static_assert`: there is no
/// compile-time-constant bounce count for a `static_assert` to check against
/// here. It is `constexpr` so it folds to a compile-time constant at any
/// call site that DOES pass a compile-time-constant `maxBounces` (as
/// diff_gpu_probe.cpp's checks 16-18 do, with kBounces = 4), but the guard
/// itself is evaluated at runtime in `runWavefrontFusedLoopProbe`, against
/// whatever `maxBounces` the caller actually passed, so that raising it past
/// what this scene can guarantee fails the probe loudly instead of quietly
/// turning "all paths survive" from a fact into a flaky probability.
constexpr float kFusedLoopWorstCaseDrift(uint32_t maxBounces) {
    const uint32_t scatteredBounces = maxBounces > 0u ? maxBounces - 1u : 0u;
    const float perBounceDrift =
        (kFusedLoopPlaneGap - kFusedLoopScatterOriginOffset) / kFusedLoopMinDirZ;
    return static_cast<float>(scatteredBounces) * perBounceDrift + kFusedLoopPrimarySpreadSlack;
}

/// True iff this staircase scene (kFusedLoopHalfExtent, kFusedLoopPlaneGap,
/// kFusedLoopPlaneCount) can GUARANTEE -- not merely make likely -- that
/// every path survives `maxBounces` fused bounces. Two independent
/// necessary conditions: the worst-case lateral drift must stay inside the
/// half-extent, and there must be enough planes for `maxBounces` bounces to
/// each hit one (a run of `maxBounces` bounces uses planes
/// 0 .. maxBounces - 1, so it needs at least `maxBounces` of them).
constexpr bool kFusedLoopSceneSafeForBounces(uint32_t maxBounces) {
    return kFusedLoopWorstCaseDrift(maxBounces) <= kFusedLoopHalfExtent &&
           maxBounces <= kFusedLoopPlaneCount;
}

}  // namespace

bool GpuProbeContext::runWavefrontFusedLoopProbe(WavefrontBuffers& buffers, uint32_t width,
                                                 uint32_t height, uint32_t maxBounces, float albedo,
                                                 uint32_t iterationSeed,
                                                 std::vector<std::vector<float>>& outDrawsPerBounce,
                                                 std::vector<uint32_t>& outLiveCountPerRun,
                                                 std::vector<uint32_t>& outFinalQueue) {
    // Push constants for wf_generate.comp -- byte-identical to
    // runWavefrontGenerateProbe's (80 bytes, see that function's comment).
    struct GeneratePush {
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
        uint32_t capacity;
    };
    static_assert(sizeof(GeneratePush) == 80,
                 "GeneratePush must match wf_generate.comp's Push block layout");

    outDrawsPerBounce.clear();
    outLiveCountPerRun.clear();
    outFinalQueue.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: buffers not built\n");
        return false;
    }
    // requires height == kFusedLoopGenerateLocalY: WavefrontStage::Fixed can
    // dispatch a genuine 3-D group count now (see kFusedLoopGenerateLocalY's
    // comment above), but this probe's own generate dispatch still only sets
    // groupCountX and leaves groupCountY/Z at 1, so one dispatch covers
    // exactly one row of local_size_y=8 pixels. This guard's height == 8
    // requirement is therefore still necessary, not merely historical -- but
    // it is NOT changed here even though the underlying 1-D limitation is
    // gone, because this probe's expected values (throughput, per-bounce
    // PathRng parity, live counts) are all calibrated to a single 512-path
    // run at this resolution; widening the dispatch to cover a genuine
    // 64x48 image is a possible follow-up, not done in this change.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalY) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontFusedLoopProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y -- this probe's generate dispatch is "
                     "1-D, one row of pixels per group), width a non-zero multiple of %u, "
                     "width*height == capacity (%u), and maxBounces > 0; got %ux%u, "
                     "maxBounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalY, capacity, width, height,
                     maxBounces);
        return false;
    }
    // This scene's "every path survives every bounce" guarantee is only a
    // guarantee up to a bounce count the geometry was sized for -- see
    // kFusedLoopWorstCaseDrift's doc comment. Fail loudly here, rather than
    // silently letting a raised maxBounces turn check 16's hard `==
    // kCapacity` equality (and the throughput/RNG checks built on top of it)
    // from a certainty into something that merely happens to pass on this
    // run. This is a runtime check, not a static_assert, because maxBounces
    // is a runtime parameter of this function with no compile-time-constant
    // value in this translation unit to assert against.
    if (!kFusedLoopSceneSafeForBounces(maxBounces)) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontFusedLoopProbe: maxBounces=%u exceeds what "
                     "the staircase scene can GUARANTEE to survive -- worst-case lateral drift is "
                     "%.1f against a half-extent of %.1f (kFusedLoopHalfExtent), and/or maxBounces "
                     "exceeds the %u available planes (kFusedLoopPlaneCount). Raise "
                     "kFusedLoopHalfExtent/kFusedLoopPlaneCount to match, or lower maxBounces; do "
                     "not proceed and rely on this run happening to pass\n",
                     maxBounces, static_cast<double>(kFusedLoopWorstCaseDrift(maxBounces)),
                     static_cast<double>(kFusedLoopHalfExtent), kFusedLoopPlaneCount);
        return false;
    }

    // --- Staircase geometry: kFusedLoopPlaneCount quads perpendicular to Z. ---
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    positions.reserve(static_cast<std::size_t>(kFusedLoopPlaneCount) * 12u);
    indices.reserve(static_cast<std::size_t>(kFusedLoopPlaneCount) * 6u);
    for (uint32_t p = 0; p < kFusedLoopPlaneCount; ++p) {
        const float z = kFusedLoopFirstPlaneZ + kFusedLoopPlaneGap * static_cast<float>(p);
        const float e = kFusedLoopHalfExtent;
        const float quad[12] = {-e, -e, z, e, -e, z, e, e, z, -e, e, z};
        positions.insert(positions.end(), std::begin(quad), std::end(quad));
        const uint32_t base = p * 4u;
        const uint32_t tris[6] = {base + 0u, base + 1u, base + 2u,
                                  base + 0u, base + 2u, base + 3u};
        indices.insert(indices.end(), std::begin(tris), std::end(tris));
    }

    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        std::span<const uint32_t>(indices),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: failed to create "
                              "staircase vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: "
                              "RTAccelerationStructure::init failed\n");
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- Buffers this function owns: the live queue ring readback (the queue
    // is only exposed as a raw VkBuffer, so it has to be copied out) and
    // wf_scatter.comp's probe-only DebugDraws sink (allocated here, never
    // shared, so it is mapped directly). ---
    GpuBuffer queueReadback;
    const VkDeviceSize queueBytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queueBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }
    GpuBuffer debugDrawsBuffer;
    const VkDeviceSize debugDrawsBytes = static_cast<VkDeviceSize>(capacity) * 3u * sizeof(float);
    if (ok) {
        debugDrawsBuffer =
            m_allocator.createBuffer(debugDrawsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: debug draws buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // --- The four stages. Built exactly once each: ComputePipeline::build
    // has no re-entrancy guard, so a second build() without an intervening
    // destroy() would leak every Vulkan object the first one created. ---
    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatter;

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kIntersectBindings[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    const VkDescriptorType kScatterBindings[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatter.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                             sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: scatter build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer scatterBuffers[4] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                            buffers.counterBuffer(), debugDrawsBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, stateQueueCounter) ||
            !intersect.bindAccelerationStructure(m_device, 3, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontFusedLoopProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        // Camera sits at -Z looking along +Z, i.e. INTO the staircase, so
        // that the primary ray hits the nearest plane (kFusedLoopFirstPlaneZ)
        // and every scattered +Z ray then walks up the remaining planes. A
        // camera looking down -Z (runWavefrontGenerateProbe's default) would
        // hit the LAST plane first and have nothing ahead of it.
        GeneratePush genPush{};
        genPush.origin[0] = 0.0f;
        genPush.origin[1] = 0.0f;
        genPush.origin[2] = -10.0f;
        genPush.forward[0] = 0.0f;
        genPush.forward[1] = 0.0f;
        genPush.forward[2] = 1.0f;
        genPush.right[0] = 1.0f;
        genPush.right[1] = 0.0f;
        genPush.right[2] = 0.0f;
        genPush.up[0] = 0.0f;
        genPush.up[1] = 1.0f;
        genPush.up[2] = 0.0f;
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = 0.2f;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        // Fixed dispatch, used here as 1-D: groupsY/groupsZ are left at
        // Fixed's default of 1 (Fixed itself now supports setting them to
        // get a genuine 3-D dispatch -- see kFusedLoopGenerateLocalY's
        // comment above -- this call site just doesn't use that), so this is
        // (width/8, 1, 1) groups x local_size (8,8), covering exactly
        // width x 8 pixels. See the height check above.
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalY});

        WavefrontLoop loop;
        loop.setConfig(WavefrontLoop::Config{albedo, iterationSeed});
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);
        loop.setScatter(scatter);

        outDrawsPerBounce.resize(maxBounces);
        outLiveCountPerRun.resize(maxBounces);

        for (uint32_t bounces = 1; ok && bounces <= maxBounces; ++bounces) {
            const WavefrontLoop::Ring finalRing = WavefrontLoop::finalLiveRing(capacity, bounces);

            runImmediate([&](VkCommandBuffer cmd) {
                // Fresh state for every run: zero() ends with its own
                // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier, so
                // generate's first read of the counter is ordered against it.
                buffers.zero(cmd);

                // THE fused loop -- one command buffer, no vkQueueWaitIdle
                // anywhere inside it. Everything that orders the stages
                // against each other is a barrier recorded by
                // WavefrontLoop::record.
                loop.record(cmd, buffers, bounces);

                // Everything below is this probe's own readback plumbing,
                // deliberately NOT part of WavefrontLoop::record (only the
                // caller knows what consumes the loop's output).
                VkBufferMemoryBarrier toHost[3]{};
                const VkBuffer hostRead[3] = {buffers.stateBuffer(), buffers.counterBuffer(),
                                              debugDrawsBuffer.buffer};
                for (int i = 0; i < 3; ++i) {
                    toHost[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].buffer = hostRead[i];
                    toHost[i].offset = 0;
                    toHost[i].size = VK_WHOLE_SIZE;
                }
                VkBufferMemoryBarrier queueToTransfer{};
                queueToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                queueToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                queueToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                queueToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                queueToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                queueToTransfer.buffer = buffers.queueBuffer();
                queueToTransfer.offset = 0;
                queueToTransfer.size = VK_WHOLE_SIZE;

                const VkBufferMemoryBarrier post[4] = {toHost[0], toHost[1], toHost[2],
                                                       queueToTransfer};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 4, post, 0, nullptr);

                VkBufferCopy region{};
                region.srcOffset = static_cast<VkDeviceSize>(finalRing.queueBase) * sizeof(uint32_t);
                region.dstOffset = 0;
                region.size = queueBytes;
                vkCmdCopyBuffer(cmd, buffers.queueBuffer(), queueReadback.buffer, 1, &region);

                VkBufferMemoryBarrier postCopy{};
                postCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                postCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postCopy.buffer = queueReadback.buffer;
                postCopy.offset = 0;
                postCopy.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &postCopy, 0,
                                     nullptr);
            });

            m_allocator.invalidateBuffer(debugDrawsBuffer);
            const auto* mappedDebug = static_cast<const float*>(debugDrawsBuffer.getMappedData());
            if (mappedDebug == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: debug draws "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
                break;
            }
            outDrawsPerBounce[bounces - 1].assign(
                mappedDebug, mappedDebug + (static_cast<std::size_t>(capacity) * 3u));
            outLiveCountPerRun[bounces - 1] =
                buffers.readbackCounter(m_allocator, finalRing.countSlot);

            m_allocator.invalidateBuffer(queueReadback);
            const auto* mappedQueue = static_cast<const uint32_t*>(queueReadback.getMappedData());
            if (mappedQueue == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: queue readback "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
                break;
            }
            outFinalQueue.assign(mappedQueue, mappedQueue + capacity);
        }
    }

    // --- Cleanup, reverse order. ---
    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
