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
    // Task 1 (Stage 0b-2a): this function used to hand-roll the whole
    // shader-module -> descriptor-set-layout -> pipeline-layout -> pipeline
    // -> descriptor-pool -> descriptor-set sequence (52 such calls existed
    // across this file before the extraction). It is now the first, and
    // simplest, client of ohao::diff::ComputePipeline, which lifted that
    // exact sequence -- including this function's reverse-order,
    // every-early-return-cleans-up-what-it-made failure discipline -- into
    // ohao/diff/wavefront/compute_pipeline.{hpp,cpp}.
    ComputePipeline pipeline;
    const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!pipeline.build(m_device, spvName, bindingTypes, pushSize)) {
        // build() already released anything it partially created.
        return false;
    }

    const VkBuffer buffers[] = {buffer};
    bool ok = pipeline.bindBuffers(m_device, buffers);
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] dispatchStorageBufferCompute: bindBuffers failed\n");
    }

    if (ok) {
        runImmediate([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline());
            VkDescriptorSet descSet = pipeline.descriptorSet();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1,
                                     &descSet, 0, nullptr);
            vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize,
                                pushData);
            vkCmdDispatch(cmd, groupCountX, 1, 1);

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

    // Every path -- success or any failure above -- destroys the pipeline
    // once, unconditionally, here; destroy() is idempotent so this is safe
    // even though build() may already have partially cleaned up on failure.
    pipeline.destroy(m_device);

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

    // --- Shader module ---
    const std::vector<uint32_t> spv = loadSpv("diff_wf_generate.comp.spv");
    if (spv.empty()) return false;

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spv.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: vkCreateShaderModule "
                              "failed\n");
        return false;
    }

    // --- Descriptor set layout: state (0), queues (1), counters (2) ---
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: "
                              "vkCreateDescriptorSetLayout failed\n");
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
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipelineLayout) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: "
                              "vkCreatePipelineLayout failed\n");
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
    ok = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) ==
         VK_SUCCESS;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: "
                              "vkCreateComputePipelines failed\n");
    }

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo descPoolInfo{};
        descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descPoolInfo.maxSets = 1;
        descPoolInfo.poolSizeCount = 1;
        descPoolInfo.pPoolSizes = &poolSize;
        ok = vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &descPool) == VK_SUCCESS;
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: "
                                  "vkCreateDescriptorPool failed\n");
        }
    }

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = descPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &setLayout;
        ok = vkAllocateDescriptorSets(m_device, &descAllocInfo, &descSet) == VK_SUCCESS;
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: "
                                  "vkAllocateDescriptorSets failed\n");
        }
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
        VkDescriptorBufferInfo stateInfo{};
        stateInfo.buffer = buffers.stateBuffer();
        stateInfo.offset = 0;
        stateInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo queueInfo{};
        queueInfo.buffer = buffers.queueBuffer();
        queueInfo.offset = 0;
        queueInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo counterInfo{};
        counterInfo.buffer = buffers.counterBuffer();
        counterInfo.offset = 0;
        counterInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &stateInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &queueInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &counterInfo;

        vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

        PushConstants push{};
        push.origin[0] = camera.origin[0]; push.origin[1] = camera.origin[1]; push.origin[2] = camera.origin[2];
        push.forward[0] = camera.forward[0]; push.forward[1] = camera.forward[1]; push.forward[2] = camera.forward[2];
        push.right[0] = camera.right[0]; push.right[1] = camera.right[1]; push.right[2] = camera.right[2];
        push.up[0] = camera.up[0]; push.up[1] = camera.up[1]; push.up[2] = camera.up[2];
        push.width = width;
        push.height = height;
        push.tanHalfFov = camera.tanHalfFov;
        push.capacity = capacity;

        const uint32_t groupsX = (width + 7) / 8;
        const uint32_t groupsY = (height + 7) / 8;

        runImmediate([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                    &descSet, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);

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
    if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, descPool, nullptr);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, setLayout, nullptr);
    if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, shaderModule, nullptr);

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

    // --- Shader modules ---
    const std::vector<uint32_t> prepSpv = ok ? loadSpv("diff_wf_prepare_indirect.comp.spv")
                                              : std::vector<uint32_t>{};
    if (ok && prepSpv.empty()) ok = false;
    const std::vector<uint32_t> intersectSpv = ok ? loadSpv("diff_wf_intersect.comp.spv")
                                                   : std::vector<uint32_t>{};
    if (ok && intersectSpv.empty()) ok = false;

    VkShaderModule prepModule = VK_NULL_HANDLE;
    VkShaderModule intersectModule = VK_NULL_HANDLE;
    if (ok) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = prepSpv.size() * sizeof(uint32_t);
        mi.pCode = prepSpv.data();
        if (vkCreateShaderModule(m_device, &mi, nullptr, &prepModule) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                                  "vkCreateShaderModule failed\n");
            ok = false;
        }
    }
    if (ok) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = intersectSpv.size() * sizeof(uint32_t);
        mi.pCode = intersectSpv.data();
        if (vkCreateShaderModule(m_device, &mi, nullptr, &intersectModule) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect "
                                  "vkCreateShaderModule failed\n");
            ok = false;
        }
    }

    // --- prepare_indirect: descriptor set layout (counter buffer only) ---
    VkDescriptorSetLayoutBinding prepBinding{};
    prepBinding.binding = 0;
    prepBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    prepBinding.descriptorCount = 1;
    prepBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout prepSetLayout = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &prepBinding;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &prepSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                                  "vkCreateDescriptorSetLayout failed\n");
            ok = false;
        }
    }

    struct PrepPush {
        uint32_t countSlot;
        uint32_t argsSlot;
    };

    VkPipelineLayout prepPipelineLayout = VK_NULL_HANDLE;
    if (ok) {
        VkPushConstantRange pr{};
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.offset = 0;
        pr.size = sizeof(PrepPush);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &prepSetLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &prepPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                                  "vkCreatePipelineLayout failed\n");
            ok = false;
        }
    }

    VkPipeline prepPipeline = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineShaderStageCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        si.module = prepModule;
        si.pName = "main";
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage = si;
        pi.layout = prepPipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &prepPipeline) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                                  "vkCreateComputePipelines failed\n");
            ok = false;
        }
    }

    // --- intersect: descriptor set layout (state, queue, counter, AS) ---
    VkDescriptorSetLayoutBinding intersectBindings[4]{};
    for (uint32_t i = 0; i < 3; ++i) {
        intersectBindings[i].binding = i;
        intersectBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        intersectBindings[i].descriptorCount = 1;
        intersectBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    intersectBindings[3].binding = 3;
    intersectBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    intersectBindings[3].descriptorCount = 1;
    intersectBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout intersectSetLayout = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings = intersectBindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &intersectSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect "
                                  "vkCreateDescriptorSetLayout failed\n");
            ok = false;
        }
    }

    struct IntersectPush {
        uint32_t capacity;
        uint32_t srcQueueBase;
        uint32_t srcCountSlot;
        uint32_t dstQueueBase;
        uint32_t dstCountSlot;
        uint32_t canarySlot;
    };

    VkPipelineLayout intersectPipelineLayout = VK_NULL_HANDLE;
    if (ok) {
        VkPushConstantRange pr{};
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.offset = 0;
        pr.size = sizeof(IntersectPush);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &intersectSetLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &intersectPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect "
                                  "vkCreatePipelineLayout failed\n");
            ok = false;
        }
    }

    VkPipeline intersectPipeline = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineShaderStageCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        si.module = intersectModule;
        si.pName = "main";
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage = si;
        pi.layout = intersectPipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &intersectPipeline) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect "
                                  "vkCreateComputePipelines failed\n");
            ok = false;
        }
    }

    // --- Descriptor pool/sets ---
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[0].descriptorCount = 4;  // 1 (prep counter) + 3 (intersect state/queue/counter)
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(m_device, &dpi, nullptr, &descPool) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                                  "vkCreateDescriptorPool failed\n");
            ok = false;
        }
    }

    VkDescriptorSet prepSet = VK_NULL_HANDLE;
    VkDescriptorSet intersectSet = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayout layouts[2] = {prepSetLayout, intersectSetLayout};
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = descPool;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(m_device, &dai, sets) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                                  "vkAllocateDescriptorSets failed\n");
            ok = false;
        } else {
            prepSet = sets[0];
            intersectSet = sets[1];
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
        VkDescriptorBufferInfo counterInfo{};
        counterInfo.buffer = buffers.counterBuffer();
        counterInfo.offset = 0;
        counterInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet prepWrite{};
        prepWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        prepWrite.dstSet = prepSet;
        prepWrite.dstBinding = 0;
        prepWrite.descriptorCount = 1;
        prepWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        prepWrite.pBufferInfo = &counterInfo;

        VkDescriptorBufferInfo stateInfo{};
        stateInfo.buffer = buffers.stateBuffer();
        stateInfo.offset = 0;
        stateInfo.range = VK_WHOLE_SIZE;
        VkDescriptorBufferInfo queueInfo{};
        queueInfo.buffer = buffers.queueBuffer();
        queueInfo.offset = 0;
        queueInfo.range = VK_WHOLE_SIZE;

        VkAccelerationStructureKHR tlas = accel.getTLAS();
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet intersectWrites[4]{};
        intersectWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        intersectWrites[0].dstSet = intersectSet;
        intersectWrites[0].dstBinding = 0;
        intersectWrites[0].descriptorCount = 1;
        intersectWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        intersectWrites[0].pBufferInfo = &stateInfo;

        intersectWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        intersectWrites[1].dstSet = intersectSet;
        intersectWrites[1].dstBinding = 1;
        intersectWrites[1].descriptorCount = 1;
        intersectWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        intersectWrites[1].pBufferInfo = &queueInfo;

        intersectWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        intersectWrites[2].dstSet = intersectSet;
        intersectWrites[2].dstBinding = 2;
        intersectWrites[2].descriptorCount = 1;
        intersectWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        intersectWrites[2].pBufferInfo = &counterInfo;

        intersectWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        intersectWrites[3].pNext = &asWrite;
        intersectWrites[3].dstSet = intersectSet;
        intersectWrites[3].dstBinding = 3;
        intersectWrites[3].descriptorCount = 1;
        intersectWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        vkUpdateDescriptorSets(m_device, 1, &prepWrite, 0, nullptr);
        vkUpdateDescriptorSets(m_device, 4, intersectWrites, 0, nullptr);

        const PrepPush prepPush{WavefrontBuffers::kCurrentCountSlot,
                                WavefrontBuffers::kIndirectArgsSlot};
        const IntersectPush intersectPush{capacity,
                                          /*srcQueueBase=*/0u,
                                          WavefrontBuffers::kCurrentCountSlot,
                                          /*dstQueueBase=*/capacity,
                                          WavefrontBuffers::kNextCountSlot,
                                          WavefrontBuffers::kCanarySlot};
        const VkDeviceSize indirectOffset =
            static_cast<VkDeviceSize>(WavefrontBuffers::kIndirectArgsSlot) * sizeof(uint32_t);

        runImmediate([&](VkCommandBuffer cmd) {
            // --- prepare_indirect: counter[countSlot] -> counter[argsSlot..+2] ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipelineLayout, 0, 1,
                                    &prepSet, 0, nullptr);
            vkCmdPushConstants(cmd, prepPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(prepPush), &prepPush);
            vkCmdDispatch(cmd, 1, 1, 1);

            // The dispatch-args triple wf_prepare_indirect just wrote must be
            // visible to vkCmdDispatchIndirect's read of the SAME buffer
            // before that read happens -- INDIRECT_COMMAND_READ, not
            // HOST_READ or SHADER_READ. This is the barrier the task brief
            // calls out as easy to miss and invalid to omit; see
            // task-5-report.md for the proof it is load-bearing.
            //
            // dstAccessMask names INDIRECT_COMMAND_READ alone -- not also
            // SHADER_READ/SHADER_WRITE for wf_intersect's own reads of
            // counter slots kCurrentCountSlot/kCanarySlot and atomicAdd on
            // kNextCountSlot. That is correct only because of an invariant
            // this barrier does not itself enforce: kIndirectArgsSlot (2-4)
            // is disjoint from kCurrentCountSlot (0), kNextCountSlot (1), and
            // kCanarySlot (5) (see wavefront_buffers.hpp). wf_intersect's
            // accesses to those other slots are ordered against
            // wf_prepare_indirect's write by program order within this same
            // command buffer plus the fact that vkCmdDispatchIndirect itself
            // does not begin shader invocations until its indirect-buffer
            // read completes -- they need no additional barrier here, but
            // only because they touch different bytes of this buffer than
            // wf_prepare_indirect wrote. Reusing WavefrontBuffers' reserved
            // counter slots for anything that overlaps kIndirectArgsSlot
            // would silently reintroduce a hazard this barrier does not
            // cover, and -- per this task's barrier-removal proof --
            // synchronization validation is not proven to catch it.
            VkBufferMemoryBarrier toIndirect{};
            toIndirect.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toIndirect.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toIndirect.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            toIndirect.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toIndirect.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toIndirect.buffer = buffers.counterBuffer();
            toIndirect.offset = 0;
            toIndirect.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 0, nullptr, 1, &toIndirect,
                                 0, nullptr);

            // --- intersect: consumes state/queue ring 0 (written by an
            // earlier, separately-submitted-and-waited dispatch, e.g.
            // wf_generate -- vkQueueWaitIdle already makes those writes
            // visible on this queue, so no additional barrier is needed for
            // them here), compacts survivors into ring 1, sized by the
            // indirect args buffer just made visible above. ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, intersectPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, intersectPipelineLayout, 0,
                                    1, &intersectSet, 0, nullptr);
            vkCmdPushConstants(cmd, intersectPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(intersectPush), &intersectPush);
            vkCmdDispatchIndirect(cmd, buffers.counterBuffer(), indirectOffset);

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

    // --- Cleanup ---
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, descPool, nullptr);
    if (intersectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, intersectPipeline, nullptr);
    if (intersectPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, intersectPipelineLayout, nullptr);
    if (intersectSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_device, intersectSetLayout, nullptr);
    if (intersectModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, intersectModule, nullptr);
    if (prepPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, prepPipeline, nullptr);
    if (prepPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, prepPipelineLayout, nullptr);
    if (prepSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, prepSetLayout, nullptr);
    if (prepModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, prepModule, nullptr);
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

    // --- Shader modules ---
    const std::vector<uint32_t> prepSpv = loadSpv("diff_wf_prepare_indirect.comp.spv");
    if (prepSpv.empty()) return false;
    const std::vector<uint32_t> scatterSpv = loadSpv("diff_wf_scatter.comp.spv");
    if (scatterSpv.empty()) return false;

    VkShaderModule prepModule = VK_NULL_HANDLE;
    VkShaderModule scatterModule = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = prepSpv.size() * sizeof(uint32_t);
        mi.pCode = prepSpv.data();
        if (vkCreateShaderModule(m_device, &mi, nullptr, &prepModule) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                                  "vkCreateShaderModule failed\n");
            ok = false;
        }
    }
    if (ok) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = scatterSpv.size() * sizeof(uint32_t);
        mi.pCode = scatterSpv.data();
        if (vkCreateShaderModule(m_device, &mi, nullptr, &scatterModule) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter "
                                  "vkCreateShaderModule failed\n");
            ok = false;
        }
    }

    // --- prepare_indirect: descriptor set layout (counter buffer only) ---
    VkDescriptorSetLayoutBinding prepBinding{};
    prepBinding.binding = 0;
    prepBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    prepBinding.descriptorCount = 1;
    prepBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout prepSetLayout = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &prepBinding;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &prepSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                                  "vkCreateDescriptorSetLayout failed\n");
            ok = false;
        }
    }

    struct PrepPush {
        uint32_t countSlot;
        uint32_t argsSlot;
    };

    VkPipelineLayout prepPipelineLayout = VK_NULL_HANDLE;
    if (ok) {
        VkPushConstantRange pr{};
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.offset = 0;
        pr.size = sizeof(PrepPush);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &prepSetLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &prepPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                                  "vkCreatePipelineLayout failed\n");
            ok = false;
        }
    }

    VkPipeline prepPipeline = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineShaderStageCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        si.module = prepModule;
        si.pName = "main";
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage = si;
        pi.layout = prepPipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &prepPipeline) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                                  "vkCreateComputePipelines failed\n");
            ok = false;
        }
    }

    // --- scatter: descriptor set layout (state, queue, counter, debug draws) ---
    VkDescriptorSetLayoutBinding scatterBindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        scatterBindings[i].binding = i;
        scatterBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        scatterBindings[i].descriptorCount = 1;
        scatterBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayout scatterSetLayout = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings = scatterBindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &scatterSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter "
                                  "vkCreateDescriptorSetLayout failed\n");
            ok = false;
        }
    }

    struct ScatterPush {
        uint32_t capacity;
        uint32_t srcQueueBase;
        uint32_t srcCountSlot;
        uint32_t dstQueueBase;
        uint32_t dstCountSlot;
        float albedo;
        uint32_t iterationSeed;
    };

    VkPipelineLayout scatterPipelineLayout = VK_NULL_HANDLE;
    if (ok) {
        VkPushConstantRange pr{};
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.offset = 0;
        pr.size = sizeof(ScatterPush);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &scatterSetLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &scatterPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter "
                                  "vkCreatePipelineLayout failed\n");
            ok = false;
        }
    }

    VkPipeline scatterPipeline = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineShaderStageCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        si.module = scatterModule;
        si.pName = "main";
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage = si;
        pi.layout = scatterPipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &scatterPipeline) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter "
                                  "vkCreateComputePipelines failed\n");
            ok = false;
        }
    }

    // --- Descriptor pool/sets ---
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 5;  // 1 (prep counter) + 4 (scatter state/queue/counter/debug)

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 2;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(m_device, &dpi, nullptr, &descPool) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: "
                                  "vkCreateDescriptorPool failed\n");
            ok = false;
        }
    }

    VkDescriptorSet prepSet = VK_NULL_HANDLE;
    VkDescriptorSet scatterSet = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayout layouts[2] = {prepSetLayout, scatterSetLayout};
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = descPool;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(m_device, &dai, sets) != VK_SUCCESS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: "
                                  "vkAllocateDescriptorSets failed\n");
            ok = false;
        } else {
            prepSet = sets[0];
            scatterSet = sets[1];
        }
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
        VkDescriptorBufferInfo counterInfo{};
        counterInfo.buffer = buffers.counterBuffer();
        counterInfo.offset = 0;
        counterInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet prepWrite{};
        prepWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        prepWrite.dstSet = prepSet;
        prepWrite.dstBinding = 0;
        prepWrite.descriptorCount = 1;
        prepWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        prepWrite.pBufferInfo = &counterInfo;

        VkDescriptorBufferInfo stateInfo{};
        stateInfo.buffer = buffers.stateBuffer();
        stateInfo.offset = 0;
        stateInfo.range = VK_WHOLE_SIZE;
        VkDescriptorBufferInfo queueInfo{};
        queueInfo.buffer = buffers.queueBuffer();
        queueInfo.offset = 0;
        queueInfo.range = VK_WHOLE_SIZE;
        VkDescriptorBufferInfo debugInfo{};
        debugInfo.buffer = debugDrawsBuffer.buffer;
        debugInfo.offset = 0;
        debugInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet scatterWrites[4]{};
        scatterWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        scatterWrites[0].dstSet = scatterSet;
        scatterWrites[0].dstBinding = 0;
        scatterWrites[0].descriptorCount = 1;
        scatterWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        scatterWrites[0].pBufferInfo = &stateInfo;

        scatterWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        scatterWrites[1].dstSet = scatterSet;
        scatterWrites[1].dstBinding = 1;
        scatterWrites[1].descriptorCount = 1;
        scatterWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        scatterWrites[1].pBufferInfo = &queueInfo;

        scatterWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        scatterWrites[2].dstSet = scatterSet;
        scatterWrites[2].dstBinding = 2;
        scatterWrites[2].descriptorCount = 1;
        scatterWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        scatterWrites[2].pBufferInfo = &counterInfo;

        scatterWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        scatterWrites[3].dstSet = scatterSet;
        scatterWrites[3].dstBinding = 3;
        scatterWrites[3].descriptorCount = 1;
        scatterWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        scatterWrites[3].pBufferInfo = &debugInfo;

        vkUpdateDescriptorSets(m_device, 1, &prepWrite, 0, nullptr);
        vkUpdateDescriptorSets(m_device, 4, scatterWrites, 0, nullptr);

        const PrepPush prepPush{srcCountSlot, WavefrontBuffers::kIndirectArgsSlot};
        const ScatterPush scatterPush{capacity,      srcQueueBase, srcCountSlot,
                                      dstQueueBase,  dstCountSlot, albedo,
                                      iterationSeed};
        const VkDeviceSize indirectOffset =
            static_cast<VkDeviceSize>(WavefrontBuffers::kIndirectArgsSlot) * sizeof(uint32_t);
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

            // --- prepare_indirect: counter[srcCountSlot] -> counter[argsSlot..+2] ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipelineLayout, 0, 1,
                                    &prepSet, 0, nullptr);
            vkCmdPushConstants(cmd, prepPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(prepPush), &prepPush);
            vkCmdDispatch(cmd, 1, 1, 1);

            // Same barrier task-5-report.md documents as load-bearing:
            // wf_prepare_indirect's write of the dispatch-args triple must be
            // visible to vkCmdDispatchIndirect's read before that read
            // happens -- INDIRECT_COMMAND_READ, not HOST_READ or SHADER_READ.
            VkBufferMemoryBarrier toIndirect{};
            toIndirect.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toIndirect.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toIndirect.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            toIndirect.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toIndirect.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toIndirect.buffer = buffers.counterBuffer();
            toIndirect.offset = 0;
            toIndirect.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 0, nullptr, 1, &toIndirect,
                                 0, nullptr);

            // --- scatter: consumes state/queue at (srcQueueBase,
            // srcCountSlot) -- written by an earlier, separately-submitted-
            // and-waited dispatch, so no additional barrier is needed for
            // that read here -- writes state in place and re-queues into
            // (dstQueueBase, dstCountSlot), sized by the indirect args
            // buffer just made visible above. ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterPipelineLayout, 0, 1,
                                    &scatterSet, 0, nullptr);
            vkCmdPushConstants(cmd, scatterPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(scatterPush), &scatterPush);
            vkCmdDispatchIndirect(cmd, buffers.counterBuffer(), indirectOffset);

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

    // --- Cleanup ---
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, descPool, nullptr);
    if (scatterPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, scatterPipeline, nullptr);
    if (scatterPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, scatterPipelineLayout, nullptr);
    if (scatterSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_device, scatterSetLayout, nullptr);
    if (scatterModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, scatterModule, nullptr);
    if (prepPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, prepPipeline, nullptr);
    if (prepPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, prepPipelineLayout, nullptr);
    if (prepSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, prepSetLayout, nullptr);
    if (prepModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, prepModule, nullptr);

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
/// source dispatches (groupCountX, 1, 1), so a fixed dispatch of that shader
/// covers exactly this many pixel rows -- see the height check below.
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
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalY) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontFusedLoopProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y, since a Fixed group count dispatches "
                     "1-D), width a non-zero multiple of %u, width*height == capacity (%u), and "
                     "maxBounces > 0; got %ux%u, maxBounces %u\n",
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
        // 1-D fixed dispatch: (width/8) groups x local_size (8,8) covers
        // exactly width x 8 pixels. See the height check above.
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
