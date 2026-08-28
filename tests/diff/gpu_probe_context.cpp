#include "gpu_probe_context.hpp"

#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"
#include "render/rt/rt_acceleration_structure.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
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

bool GpuProbeContext::runBsdfProbe(GradientArena& sink, const BsdfProbeCase& probeCase) {
    // bsdf_probe.comp guards on gl_GlobalInvocationID.x == 0 and its
    // local_size_x is 1, so one group is one invocation is one case.
    return dispatchStorageBufferCompute("diff_bsdf_probe.comp.spv", sink.buffer(), &probeCase,
                                        sizeof(probeCase), 1u);
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

// --- Scene builders shared by more than one probe -------------------------
namespace {

/// Fills `positions` (3 floats per vertex) and `indices` (3 uints per
/// triangle) with the CLOSED axis-aligned box [-halfExtent, halfExtent]^3:
/// six quads, twelve triangles, twenty-four vertices (each face owns its own
/// four, so no face's winding is constrained by a neighbour's).
///
/// Every face is wound so that cross(v1 - v0, v2 - v0) points OUT of the
/// box. That is deliberate and load-bearing for the normal check: a ray
/// inside the box hits each face from behind its winding-order normal, so
/// wf_intersect.comp's "flip the geometric normal to oppose the incoming
/// ray" step must actually fire on every single hit. Wound inward, the flip
/// would be a no-op on every hit and its absence would be invisible.
///
/// Right-handedness of the (u, v, outward) triple is what makes the winding
/// come out that way: for the +axis face of axis k, (u, v) = ((k+1)%3,
/// (k+2)%3) satisfies e_u x e_v = +e_k, and the pair is swapped for the
/// -axis face so that the cross product flips with the face.
///
/// The face planes are exactly +/-halfExtent on one axis and the edge
/// vectors are exactly axis-aligned, so cross(v1 - v0, v2 - v0) is exactly
/// (+/-4*halfExtent^2) on that axis and exactly 0 on the other two -- which
/// is what lets the normal check assert the two off-axis components are
/// bit-exactly zero.
void buildAxisAlignedBoxGeometry(float halfExtent, std::vector<float>& positions,
                                 std::vector<uint32_t>& indices) {
    positions.clear();
    indices.clear();
    positions.reserve(24u * 3u);
    indices.reserve(12u * 3u);

    const float e = halfExtent;
    for (uint32_t k = 0; k < 3u; ++k) {
        for (int signIndex = 0; signIndex < 2; ++signIndex) {
            const float s = (signIndex == 0) ? 1.0f : -1.0f;
            uint32_t u = (k + 1u) % 3u;
            uint32_t v = (k + 2u) % 3u;
            if (s < 0.0f) {
                const uint32_t swap = u;
                u = v;
                v = swap;
            }

            const float du[4] = {-e, e, e, -e};
            const float dv[4] = {-e, -e, e, e};
            const uint32_t base = static_cast<uint32_t>(positions.size() / 3u);
            for (int corner = 0; corner < 4; ++corner) {
                float pos[3] = {0.0f, 0.0f, 0.0f};
                pos[k] = s * e;
                pos[u] = du[corner];
                pos[v] = dv[corner];
                positions.push_back(pos[0]);
                positions.push_back(pos[1]);
                positions.push_back(pos[2]);
            }
            const uint32_t tris[6] = {base + 0u, base + 1u, base + 2u,
                                      base + 0u, base + 2u, base + 3u};
            indices.insert(indices.end(), std::begin(tris), std::end(tris));
        }
    }
}

}  // namespace

bool GpuProbeContext::runWavefrontIntersectProbe(WavefrontBuffers& buffers, float planeDistance,
                                                 float quadMinY, std::vector<uint32_t>& outQueue1) {
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
    return runWavefrontIntersectOnGeometry(buffers, std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), outQueue1);
}

bool GpuProbeContext::runWavefrontBoxIntersectProbe(WavefrontBuffers& buffers, float halfExtent,
                                                    std::vector<uint32_t>& outQueue1) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(halfExtent, positions, indices);
    return runWavefrontIntersectOnGeometry(buffers, std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), outQueue1);
}

bool GpuProbeContext::runWavefrontIntersectOnGeometry(WavefrontBuffers& buffers,
                                                      std::span<const float> positions,
                                                      std::span<const uint32_t> indices,
                                                      std::vector<uint32_t>& outQueue1) {
    outQueue1.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: buffers not built\n");
        return false;
    }

    // The vertex and index buffers are ALSO storage buffers, not merely
    // acceleration-structure build input: wf_intersect.comp reads the hit
    // triangle's three vertices back out of them (bindings 3 and 4) to
    // compute the hit's geometric normal. There is no other way to recover
    // it -- a ray query reports a primitive index and barycentrics, never a
    // normal.
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

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
    // state, queues, counters, vertex positions, triangle indices, TLAS.
    // The acceleration structure is LAST so that bindBuffers -- which writes
    // a contiguous prefix of storage-buffer bindings starting at 0 -- can
    // cover all five buffers in one call.
    const VkDescriptorType intersectBindingTypes[6] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS())) {
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
            // the measurement is recorded in
            // docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md
            // section 3.1, "What actually guards those hand-written
            // barriers") -> intersect, dispatched indirectly from
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
                                               std::vector<float>& outDebugDraws,
                                               const WavefrontScatterMaterial& material,
                                               std::vector<float>* outEnvSamples,
                                               const WavefrontShadowScene& shadowScene,
                                               std::vector<float>* outNeeSamples) {
    outQueueDst.clear();
    outDebugDraws.clear();
    if (outEnvSamples != nullptr) outEnvSamples->clear();
    if (outNeeSamples != nullptr) outNeeSamples->clear();

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
    // --- Occluders for the next-event estimator's shadow rays. An empty
    // WavefrontShadowScene becomes ONE triangle a million units out: the
    // acceleration-structure descriptor cannot be VK_NULL_HANDLE (this
    // context does not enable nullDescriptor), and the shader's shadow rays
    // stop at kShadowTMax = 1000, so geometry at 1e6 is reachable by nothing
    // and "no occluders" is expressed as data rather than as a null handle.
    // The triangle has real area, so the BLAS build has nothing to reject. ---
    static constexpr float kUnreachable = 1.0e6f;
    static const std::array<float, 9> kEmptySceneVertices = {
        kUnreachable,        kUnreachable,        kUnreachable,
        kUnreachable + 1.0f, kUnreachable,        kUnreachable,
        kUnreachable,        kUnreachable + 1.0f, kUnreachable};
    static const std::array<uint32_t, 3> kEmptySceneIndices = {0, 1, 2};
    const std::span<const float> shadowPositions =
        shadowScene.empty() ? std::span<const float>(kEmptySceneVertices) : shadowScene.positions;
    const std::span<const uint32_t> shadowIndices =
        shadowScene.empty() ? std::span<const uint32_t>(kEmptySceneIndices) : shadowScene.indices;

    GpuBuffer shadowVertexBuffer = m_allocator.createBufferFromSpan<float>(
        shadowPositions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer shadowIndexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        shadowIndices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    RTAccelerationStructure shadowAccel;
    if (!shadowVertexBuffer.isValid() || !shadowIndexBuffer.isValid()) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                              "vertex/index buffer allocation failed\n");
        ok = false;
    }
    if (ok && !shadowAccel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: "
                              "RTAccelerationStructure::init failed\n");
        ok = false;
    }
    if (ok) {
        BlasHandle blas = INVALID_BLAS;
        runImmediate([&](VkCommandBuffer cmd) {
            blas = shadowAccel.createBLASFromPositions(
                shadowVertexBuffer.buffer, static_cast<uint32_t>(shadowPositions.size() / 3),
                shadowIndexBuffer.buffer, static_cast<uint32_t>(shadowIndices.size()),
                /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                                  "createBLASFromPositions failed\n");
            ok = false;
        } else {
            shadowAccel.clearInstances();
            shadowAccel.addInstance(blas, glm::mat4(1.0f));
            runImmediate([&](VkCommandBuffer cmd) { shadowAccel.buildTLAS(cmd); });
            if (shadowAccel.getTLAS() == VK_NULL_HANDLE) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                                      "buildTLAS produced no TLAS\n");
                ok = false;
            }
        }
    }
    if (!ok) {
        if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
        if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);
        return false;
    }

    WavefrontStage prepareIndirect;
    WavefrontStage scatter;
    const VkDescriptorType counterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    // state, queues, counters, debug draws, env marginal CDF, env
    // conditional CDF, env samples, NEE samples, TLAS -- wf_scatter.comp's
    // bindings 0..8 in order. The two env CDF buffers are read-only to the
    // shader but are ordinary storage buffers as far as the descriptor set
    // is concerned; the acceleration structure is last so bindBuffers can
    // write all eight storage buffers as one contiguous prefix.
    // ... and binding 9, the film (Stage 0b-2b Task 5). The acceleration
    // structure at 8 sits BETWEEN two storage buffers now, so the storage
    // buffers are no longer one contiguous prefix -- bindBuffers writes
    // 0..N-1 in order, so the film cannot go through it and is bound
    // separately by bindStorageBuffer below.
    // ... and binding 10, the gradient arena (Stage 1 Task 2). See the note
    // at the bindStorageBuffer call below for why THIS probe binds the film
    // buffer there rather than allocating a placeholder.
    const VkDescriptorType scatterBindingTypes[11] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (!prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", counterOnly,
                               sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                              "build\n");
        m_allocator.destroyBuffer(shadowIndexBuffer);
        m_allocator.destroyBuffer(shadowVertexBuffer);
        return false;
    }
    if (!scatter.build(m_device, "diff_wf_scatter.comp.spv", scatterBindingTypes,
                       sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter build\n");
        prepareIndirect.destroy(m_device);
        m_allocator.destroyBuffer(shadowIndexBuffer);
        m_allocator.destroyBuffer(shadowVertexBuffer);
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
    const VkDeviceSize debugDrawsBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
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

    // wf_scatter.comp's environment-sample sink (binding 6): 4 floats per
    // path index. Allocated unconditionally -- the descriptor set needs
    // something valid bound there whether or not the caller asked to read it
    // back.
    GpuBuffer envSamplesBuffer;
    const VkDeviceSize envSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    if (ok) {
        envSamplesBuffer = m_allocator.createBuffer(
            envSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        if (!envSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: env samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // wf_scatter.comp's next-event sink (binding 7): kNeeSampleFloats floats
    // per path index. Allocated unconditionally for the same reason
    // envSamplesBuffer is -- the descriptor set is not optional.
    GpuBuffer neeSamplesBuffer;
    const VkDeviceSize neeSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    if (ok) {
        neeSamplesBuffer = m_allocator.createBuffer(
            neeSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        if (!neeSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: NEE samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // wf_scatter.comp's FILM (binding 9): 3 floats per PIXEL index. This
    // probe runs one sample per pixel, so pixel count == capacity here.
    // Allocated and bound but never read back -- see this function's doc
    // comment: the ordering it would exercise is a device idle wait's, not a
    // barrier's, so a film check here would be measuring nothing. It is
    // zeroed in the command buffer below all the same, because a film
    // accumulating onto whatever the allocator handed back is a bug class
    // worth not having even in an unread buffer.
    GpuBuffer filmBuffer;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(capacity) * 3u * sizeof(float);
    if (ok) {
        filmBuffer = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: film buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),         buffers.queueBuffer(),
                                            buffers.counterBuffer(),       debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(),   buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer,       neeSamplesBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            !scatter.bindAccelerationStructure(m_device, 8, shadowAccel.getTLAS()) ||
            // Binding 9 sits after the acceleration structure, so it cannot
            // go through bindBuffers' 0-based prefix.
            !scatter.bindStorageBuffer(m_device, 9, filmBuffer.buffer) ||
            // BINDING 10, THE GRADIENT ARENA. A descriptor set must cover
            // every binding the shader statically declares, and this probe
            // has no arena: it runs the FORWARD instantiation, whose hook is
            // the film write, and it leaves ScatterPush::gradArenaFloats at
            // 0, which disables every gradient write in the traversal.
            //
            // The FILM buffer is re-bound here rather than a placeholder
            // being allocated, and that choice is deliberate rather than
            // lazy: if a gradient write ever DID reach a probe that set
            // gradArenaFloats to 0, it would land in the film -- where
            // checks 32, 33 and 34 compare the film against independent
            // oracles and would fail loudly. A private placeholder buffer
            // would absorb the same stray write in silence. The same
            // reasoning and the same re-bind appear in the fused-loop,
            // replay and parity probes.
            !scatter.bindStorageBuffer(m_device, 10, filmBuffer.buffer)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontScatterProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        const WavefrontLoop::ScatterPush scatterPush{capacity,
                                                     srcQueueBase,
                                                     srcCountSlot,
                                                     dstQueueBase,
                                                     dstCountSlot,
                                                     albedo,
                                                     iterationSeed,
                                                     material.roughness,
                                                     material.metallic,
                                                     material.specularWeight,
                                                     buffers.envWidth(),
                                                     buffers.envHeight(),
                                                     buffers.envIntegral(),
                                                     /*filmPixelCount=*/capacity};
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
            // The film, for the same reason: wf_scatter.comp atomicAdds into
            // it, so it must be 0 going in or the accumulation starts from
            // whatever the allocator handed back.
            vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);

            VkBufferMemoryBarrier fillBarrier[2]{};
            const VkBuffer filled[2] = {buffers.counterBuffer(), filmBuffer.buffer};
            for (int i = 0; i < 2; ++i) {
                fillBarrier[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                fillBarrier[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                fillBarrier[i].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                fillBarrier[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fillBarrier[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fillBarrier[i].buffer = filled[i];
                fillBarrier[i].offset = 0;
                fillBarrier[i].size = VK_WHOLE_SIZE;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                                 fillBarrier, 0, nullptr);

            // --- prepare_indirect: counter[srcCountSlot] ->
            // counter[argsSlot..+2] -> the COMPUTE_SHADER -> DRAW_INDIRECT /
            // INDIRECT_COMMAND_READ barrier that
            // docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md
            // section 3.1, "What actually guards those hand-written
            // barriers", documents as
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
            VkBufferMemoryBarrier postScatter[5]{};
            VkBuffer writtenBuffers[5] = {buffers.stateBuffer(), buffers.counterBuffer(),
                                          debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                          neeSamplesBuffer.buffer};
            for (int i = 0; i < 5; ++i) {
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

            VkBufferMemoryBarrier postDispatch[6] = {postScatter[0], postScatter[1], postScatter[2],
                                                     postScatter[3], postScatter[4],
                                                     queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 6, postDispatch, 0, nullptr);

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
            outDebugDraws.assign(mappedDebug,
                                 mappedDebug + (static_cast<std::size_t>(capacity) *
                                                kDebugDrawFloats));
        }

        if (outEnvSamples != nullptr) {
            m_allocator.invalidateBuffer(envSamplesBuffer);
            const auto* mappedEnv = static_cast<const float*>(envSamplesBuffer.getMappedData());
            if (mappedEnv == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: env samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outEnvSamples->assign(mappedEnv,
                                      mappedEnv + (static_cast<std::size_t>(capacity) * kEnvSampleFloats));
            }
        }

        if (outNeeSamples != nullptr) {
            m_allocator.invalidateBuffer(neeSamplesBuffer);
            const auto* mappedNee = static_cast<const float*>(neeSamplesBuffer.getMappedData());
            if (mappedNee == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: NEE samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outNeeSamples->assign(
                    mappedNee,
                    mappedNee + (static_cast<std::size_t>(capacity) * kNeeSampleFloats));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    scatter.destroy(m_device);
    prepareIndirect.destroy(m_device);
    if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
    if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);

    return ok;
}

// --- Fused-loop probe scene ------------------------------------------------
//
// A CLOSED AXIS-ALIGNED BOX, entered from its centre. This replaced the
// staircase of parallel quads that stood here through Stage 0b-2a, and the
// replacement was forced, not cosmetic.
//
// The staircase existed only because wf_scatter.comp hardcoded the surface
// normal to (0,0,1): every scattered ray then had dir.z > 0 whatever it hit,
// so paths marched monotonically in +Z and a stack of planes perpendicular
// to Z caught each one in turn. Reading the REAL geometric normal destroys
// that. A real forward-facing normal always OPPOSES the incoming ray, so a
// cosine hemisphere about it sends the path back the way it came -- a
// staircase of parallel planes is exactly the scene that fails, because the
// path bounces back off the first plane it meets and re-crosses the gap at
// whatever grazing angle the RNG hands it. Survival there would be a
// probability, not a fact, which is what check 16's hard
// `liveCount == kCapacity` refuses to be built on.
//
// SURVIVAL DERIVATION (exact arithmetic). Let B = [-E, E]^3 with E =
// kFusedLoopBoxHalfExtent, built as six quads wound outward (see
// buildAxisAlignedBoxGeometry). Claim: every path is alive after ANY number
// of bounces.
//
//   Base. wf_generate.comp puts every path's origin at the camera position
//   c. c is the box centre, so c is in the open interior int(B).
//
//   Step. Let a path's origin p be in int(B) with any direction d != 0.
//   B is compact and convex and p is interior, so the ray {p + t d : t >= 0}
//   leaves B at a unique t* > 0, and q = p + t* d lies on some face, with
//   |q_j| <= E on every axis -- i.e. inside that face's quad, since the quad
//   spans exactly [-E, E] on the two axes it is not fixed on. So the ray
//   query commits a hit, provided
//     (a) t* >= tMin. wf_intersect.comp traces with tMin = 0 EXACTLY, so
//         this holds for every t* > 0 with nothing to prove. This is why
//         that shader's tMin is 0 and not an epsilon: a positive tMin drops
//         real hits closer than it, which for a closed scene means a bounce
//         landing within tMin of an edge leaks out through the gap and dies.
//         With tMin > 0 the "every path survives" claim would be a statement
//         about how near an edge the RNG happens to land -- a probability.
//     (b) t* <= tMax. t* is at most the box's longest chord, its space
//         diagonal 2*E*sqrt(3); the guard below checks that against
//         wf_intersect.comp's tMax of 1000.
//   Nothing therefore takes the miss branch, so no path is ever killed.
//
//   Normal. The committed hit's forward-facing geometric normal is the
//   inward normal of the exit face: the ray leaves through that face, so
//   d agrees in sign with the face's outward normal, and wf_intersect.comp's
//   flip makes the stored normal point back into B.
//
//   Induction. wf_scatter.comp advances the path to q and offsets it along
//   that normal: p' = q + kFusedLoopScatterOriginOffset * N. On the face's
//   own axis k that gives |p'_k| = E - offset < E (the guard checks
//   offset < E); on the other two axes p' keeps q's coordinates, |q_j| <= E.
//   So p' is in int(B) again -- unless q landed exactly on an edge of B,
//   where one |q_j| is exactly E. And the new direction satisfies
//   dot(d', N) = sqrt(1 - u1) >= 2^-12 > 0 (diffRngNext1D returns
//   (state >> 8) * 2^-24 and so never reaches 1), so d' != 0 and points
//   strictly into the interior half-space. The Step applies again.
//
// WHAT THIS BUYS OVER THE STAIRCASE. The staircase's guarantee decayed with
// bounce count: each scattered bounce could drift up to
// (gap / minimum dir.z) sideways, so its guard had to compare an
// accumulating worst-case drift against the quads' half-extent and refused
// bounce counts above six. The induction above has NO per-bounce term -- it
// is uniform in the number of bounces -- so raising maxBounces is safe by
// the theorem rather than by re-deriving a budget. That is a deliberate
// strengthening, not a loosening of the guard: the guard below still fails
// loudly, and still refuses to run, if any hypothesis the induction actually
// rests on (the diagonal against tMax, the offset against E, the camera
// inside B) is broken by a future change to those constants.
//
// THREE CAVEATS, not one, and none of them silently tolerated: check 16
// asserts every one of the capacity paths survives every bounce, so any path
// that actually falls out of the scene fails the probe rather than skewing
// it.
//
//   1. The edge exclusion is a FLOAT BAND, not a measure-zero set. The Step
//      above reasons about the exact real-number exit point q = p + t* d,
//      but wf_scatter.comp:153 reconstructs it in float as
//      hitPoint = origin + dir * hitT rather than using an exact intersection
//      point, so on a FREE axis (one not fixed by the face) |q_j| can exceed
//      E by about ulp(4) ~= 5e-7 due to that reconstruction's rounding. A hit
//      within that band of an edge is not exactly ON the edge but IS outside
//      the box after reconstruction, so the induction's "p' is in int(B)
//      again" step fails and the path dies next bounce. Correct statement:
//      within ~1e-6 of an edge, not exactly on one; at 512 paths and a box
//      whose edges are a vanishingly thin sliver of the sphere of
//      directions, the probability of landing in that band on any given
//      bounce is of order 1e-3 per run -- still negligible, still caught
//      loudly by check 16 rather than silently skewing a result, but not
//      literally zero the way "measure-zero" claims.
//
//   2. The face-internal triangulation diagonal is a SECOND, distinct
//      window, independent of (1). Each face is two triangles sharing a
//      diagonal (see buildAxisAlignedBoxGeometry), and a ray aimed exactly
//      at that diagonal is a case the derivation's "lies on some face"
//      step glosses over: Vulkan's ray-triangle watertightness language for
//      a ray through a shared edge between two triangles is a SHOULD, not a
//      MUST (VK_KHR_ray_query / VK_KHR_acceleration_structure leave exact
//      watertightness at shared edges as non-normative guidance, not a
//      guarantee this derivation is entitled to lean on).
//
//   3. The theorem is now CONDITIONAL on the feature under test. The
//      "Normal" step above assumes wf_intersect.comp writes the correct
//      forward-facing geometric normal; if it does not (a wrong sign, a
//      swapped axis, a stale value), a scattered direction can point back
//      out through the surface instead of into B, breaking the induction's
//      "p' is in int(B) again" step directly. That is precisely what makes
//      check 16 a genuine, independent-looking observer of a normal bug --
//      the task's own failure demonstration tripped check 16 before the
//      dedicated normals check (19) -- but it also means check 16's
//      guarantee is no longer independent of what Task 1 added: it is now a
//      joint statement about the scene AND about wf_intersect.comp's normal
//      being correct, not about the scene alone.
namespace {

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

/// wf_generate.comp's local_size_X, which is a DIFFERENT number from
/// local_size_y even though both are 8 today. The two were one constant
/// until a review pointed out that `kFusedLoopGenerateLocalY` was being used
/// as the group-count divisor for the X axis (`width / ...`) as well as the
/// height requirement -- so a change to local_size_x alone would have left
/// the dispatch covering fewer pixel columns than the image has, silently,
/// with the uncovered paths never generated and every downstream count
/// quietly short. Split so that each axis's constant is used only for its
/// own axis.
constexpr uint32_t kFusedLoopGenerateLocalX = 8;

/// Half-extent of the closed box the loop bounces inside. Small enough that
/// its space diagonal is far inside wf_intersect.comp's tMax, large enough
/// that the primary rays' spread is nowhere near degenerate. A power of two
/// on purpose: the box's faces are then at exactly representable
/// coordinates and each face's cross(v1 - v0, v2 - v0) is exactly
/// (+/-4E^2, 0, 0) up to axis permutation, so the geometric normal comes out
/// of normalize() with two bit-exact zero components.
constexpr float kFusedLoopBoxHalfExtent = 4.0f;

/// wf_intersect.comp's ray tMax, mirrored here because the derivation above
/// compares the box's longest chord against it. Change one and this guard
/// stops meaning what it says.
constexpr float kFusedLoopRayTMax = 1000.0f;

/// wf_scatter.comp's ray-origin epsilon offset along the geometric normal,
/// mirrored here for the same reason: the induction needs it to be smaller
/// than the half-extent, or the "next origin is still inside" step fails --
/// AND larger than float resolution at the box's scale, or the offset
/// rounds away to nothing and the "next origin is still inside" step fails
/// the other way (see kFusedLoopScatterOriginOffsetMinBound below).
constexpr float kFusedLoopScatterOriginOffset = 1e-4f;

/// The camera, which must sit strictly inside the box for the induction's
/// base case. These are the values actually pushed to wf_generate.comp
/// below, so the guard checks the camera the probe really uses.
constexpr float kFusedLoopCameraX = 0.0f;
constexpr float kFusedLoopCameraY = 0.0f;
constexpr float kFusedLoopCameraZ = 0.0f;
constexpr float kFusedLoopTanHalfFov = 0.2f;

/// sqrt(3), to four more digits than float can hold -- the box's space
/// diagonal is 2*E*sqrt(3).
constexpr float kSqrt3 = 1.7320508075688772f;

constexpr float kFusedLoopAbs(float v) { return v < 0.0f ? -v : v; }

/// The longest distance any ray can travel inside the box: its space
/// diagonal. Every committed hit is at t* <= this.
constexpr float kFusedLoopBoxDiagonal() { return 2.0f * kFusedLoopBoxHalfExtent * kSqrt3; }

/// Lower bound on kFusedLoopScatterOriginOffset. With wf_intersect.comp's
/// tMin at exactly 0 (see that shader's comment), the ENTIRE
/// self-intersection guarantee rests on the scatter offset being large
/// relative to float resolution at the box's scale -- not merely small
/// relative to the half-extent, which is a completely different, unrelated
/// bound (see the comment above kFusedLoopScatterOriginOffset). A face at
/// |coordinate| == E has an ulp of E * epsilon(); if the offset is at or
/// below that, `q + offset * N` rounds back to `q` exactly in float, and
/// the next ray origin lands ON the surface with tMin == 0 -- ray-tracing
/// APIs make no promise about a ray whose origin is exactly on a triangle
/// it did not just leave a genuine distance from, and a self-intersection
/// there would look like a path randomly dying via check 16.
///
/// 8x is a generous, round margin over the 1-ulp threshold where the
/// guarantee actually first breaks -- not a tight bound.
constexpr float kFusedLoopScatterOriginOffsetMinBound() {
    return 8.0f * kFusedLoopBoxHalfExtent * std::numeric_limits<float>::epsilon();
}

constexpr float kFusedLoopCameraMaxAbsCoord() {
    float m = kFusedLoopAbs(kFusedLoopCameraX);
    if (kFusedLoopAbs(kFusedLoopCameraY) > m) m = kFusedLoopAbs(kFusedLoopCameraY);
    if (kFusedLoopAbs(kFusedLoopCameraZ) > m) m = kFusedLoopAbs(kFusedLoopCameraZ);
    return m;
}

// These are exactly the hypotheses the survival induction in this section's
// header rests on, other than `maxBounces >= 1` (checked at RUNTIME below,
// against the caller's actual argument, where a zero-bounce run is rejected
// alongside this probe's other dispatch-shape requirements):
//
//   1. The box's space diagonal fits inside wf_intersect.comp's tMax, so no
//      exit hit is ever rejected as too far.
//   2. wf_scatter.comp's origin offset is smaller than the half-extent, so
//      stepping off a face lands strictly inside the box rather than
//      through the opposite one.
//   3. wf_scatter.comp's origin offset is larger than float resolution at
//      the box's scale, so it survives rounding and the next ray origin is
//      not left sitting exactly on the surface it just left (tMin == 0 --
//      see wf_intersect.comp and kFusedLoopScatterOriginOffsetMinBound).
//   4. The camera is strictly inside the box, which is the induction's base
//      case.
//
// Every one of these is a compile-time constant -- unlike the staircase
// scene this box replaced, NONE of them depends on `maxBounces`, since the
// induction is uniform in the bounce count (a scene that survives one
// bounce survives a thousand) -- so they are asserted at BUILD time,
// unconditionally, rather than only when this probe happens to run. A
// future edit to E, to wf_intersect.comp's mirrored tMax, or to
// wf_scatter.comp's mirrored offset fails the build instead of quietly
// turning check 16's hard `== kCapacity` equality into something that
// merely happens to hold on this run.
static_assert(kFusedLoopBoxDiagonal() <= kFusedLoopRayTMax,
              "fused-loop box's space diagonal must fit inside wf_intersect.comp's ray tMax, or "
              "some exit hit is rejected as too far and the survival induction's step (b) fails");
static_assert(kFusedLoopScatterOriginOffset < kFusedLoopBoxHalfExtent,
              "fused-loop scatter origin offset must be smaller than the box half-extent, or "
              "stepping off a face can land through the opposite one");
static_assert(kFusedLoopScatterOriginOffset > kFusedLoopScatterOriginOffsetMinBound(),
              "fused-loop scatter origin offset must exceed float resolution at the box's scale "
              "(see kFusedLoopScatterOriginOffsetMinBound), or it rounds away to nothing and the "
              "next ray origin lands ON the surface with wf_intersect.comp's tMin == 0");
static_assert(kFusedLoopCameraMaxAbsCoord() < kFusedLoopBoxHalfExtent,
              "fused-loop camera must sit strictly inside the box, which is the survival "
              "induction's base case");

}  // namespace

bool GpuProbeContext::runWavefrontFusedLoopProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces, float albedo,
    uint32_t iterationSeed, std::vector<std::vector<float>>& outDrawsPerBounce,
    std::vector<uint32_t>& outLiveCountPerRun, std::vector<uint32_t>& outFinalQueue,
    std::vector<float>* outEnvSamples, std::vector<float>* outNeeSamples,
    bool unoccludedShadowRays, std::vector<std::vector<float>>* outNeeSamplesPerRun,
    std::vector<std::vector<float>>* outFilmPerRun) {
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
    if (outNeeSamples != nullptr) outNeeSamples->clear();
    if (outNeeSamplesPerRun != nullptr) outNeeSamplesPerRun->clear();
    if (outFilmPerRun != nullptr) outFilmPerRun->clear();

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
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontFusedLoopProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y -- this probe's generate dispatch is "
                     "1-D, one row of pixels per group), width a non-zero multiple of %u, "
                     "width*height == capacity (%u), and maxBounces > 0; got %ux%u, "
                     "maxBounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     maxBounces);
        return false;
    }
    // This scene's "every path survives every bounce" guarantee also needs
    // maxBounces >= 1 (a zero-bounce run has nothing to guarantee), which is
    // exactly the condition already checked and reported above, alongside
    // this probe's other dispatch-shape requirements -- so it is not
    // repeated here. Every other hypothesis of the survival induction (see
    // the scene-constants header above) is a compile-time constant and is
    // enforced by the static_asserts just above this function, which fail
    // the BUILD, unconditionally, rather than only when this probe runs.

    // --- Scene: the closed box derived above. ---
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(kFusedLoopBoxHalfExtent, positions, indices);

    // Storage-buffer usage as well as build input: wf_intersect.comp reads
    // the hit triangle's vertices back out of these to compute its geometric
    // normal (bindings 3 and 4).
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        std::span<const uint32_t>(indices),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: failed to create "
                              "box vertex/index buffers\n");
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

    // --- The SHADOW acceleration structure, when the caller asked for the
    // path rays and the shadow rays to see different scenes. See this
    // function's doc comment for why that rig exists at all: it is the only
    // way to have every path survive every bounce (which needs a closed
    // body) AND a nonzero direct-lighting contribution at every bounce
    // (which needs shadow rays that escape). Same "no occluders" expression
    // runWavefrontScatterProbe uses -- one triangle a million units out,
    // three orders of magnitude past the shader's kShadowTMax of 1000 --
    // because an acceleration-structure descriptor cannot be
    // VK_NULL_HANDLE without the nullDescriptor feature this context does
    // not enable.
    //
    // With unoccludedShadowRays false (the default, and what checks 27/28
    // run) NONE of this is built and scatter's binding 8 gets the box, i.e.
    // exactly the previous behaviour.
    static constexpr float kUnreachable = 1.0e6f;
    static const std::array<float, 9> kUnreachableVertices = {
        kUnreachable,        kUnreachable,        kUnreachable,
        kUnreachable + 1.0f, kUnreachable,        kUnreachable,
        kUnreachable,        kUnreachable + 1.0f, kUnreachable};
    static const std::array<uint32_t, 3> kUnreachableIndices = {0, 1, 2};
    GpuBuffer shadowVertexBuffer;
    GpuBuffer shadowIndexBuffer;
    RTAccelerationStructure shadowAccel;
    if (ok && unoccludedShadowRays) {
        shadowVertexBuffer = m_allocator.createBufferFromSpan<float>(
            std::span<const float>(kUnreachableVertices),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        shadowIndexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
            std::span<const uint32_t>(kUnreachableIndices),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!shadowVertexBuffer.isValid() || !shadowIndexBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow scene "
                                  "vertex/index buffer allocation failed\n");
            ok = false;
        }
        if (ok && !shadowAccel.init(m_device, m_physicalDevice, m_queue, m_queueFamily,
                                    m_commandPool, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow "
                                  "RTAccelerationStructure::init failed\n");
            ok = false;
        }
        if (ok) {
            BlasHandle shadowBlas = INVALID_BLAS;
            runImmediate([&](VkCommandBuffer cmd) {
                shadowBlas = shadowAccel.createBLASFromPositions(
                    shadowVertexBuffer.buffer,
                    static_cast<uint32_t>(kUnreachableVertices.size() / 3),
                    shadowIndexBuffer.buffer, static_cast<uint32_t>(kUnreachableIndices.size()),
                    /*indexByteOffset=*/0, cmd);
            });
            if (shadowBlas == INVALID_BLAS) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow scene "
                                      "createBLASFromPositions failed\n");
                ok = false;
            } else {
                shadowAccel.clearInstances();
                shadowAccel.addInstance(shadowBlas, glm::mat4(1.0f));
                runImmediate([&](VkCommandBuffer cmd) { shadowAccel.buildTLAS(cmd); });
                if (shadowAccel.getTLAS() == VK_NULL_HANDLE) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow "
                                          "scene buildTLAS produced no TLAS\n");
                    ok = false;
                }
            }
        }
    }
    const VkAccelerationStructureKHR scatterTLAS =
        unoccludedShadowRays ? shadowAccel.getTLAS() : accel.getTLAS();

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
    const VkDeviceSize debugDrawsBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
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
    // wf_scatter.comp's environment-sample sink (binding 6). The descriptor
    // set needs it bound whether or not the caller asked to read it back --
    // every scatter dispatch in the fused loop writes it, so it goes into
    // loopExtras below for exactly the reason debugDrawsBuffer does -- and it
    // is read back into outEnvSamples when non-null (see this function's doc
    // comment: this is the ONLY probe that exercises WavefrontLoop::record's
    // OWN fill of ScatterPush's envWidth/envHeight/envIntegral tail, as
    // opposed to runWavefrontScatterProbe's hand-filled push constants, so it
    // is also the only probe that can observe a bug in that fill). The
    // chi-squared check (24-26 in diff_gpu_probe.cpp) still runs against the
    // stage-by-stage scatter probe, which can vary the seed per dispatch;
    // this sink exists for a different purpose (check 27).
    GpuBuffer envSamplesBuffer;
    const VkDeviceSize envSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    if (ok) {
        envSamplesBuffer =
            m_allocator.createBuffer(envSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!envSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: env samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }
    // wf_scatter.comp's next-event sink (binding 7). Written at a fixed
    // pathIndex*kNeeSampleFloats offset by EVERY scatter dispatch in the
    // fused run, so -- exactly like debugDrawsBuffer and envSamplesBuffer --
    // it belongs in loopExtras below, and only the LAST bounce's write
    // survives in it.
    GpuBuffer neeSamplesBuffer;
    const VkDeviceSize neeSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    if (ok) {
        neeSamplesBuffer =
            m_allocator.createBuffer(neeSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!neeSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }
    // wf_scatter.comp's FILM (binding 9, Stage 0b-2b Task 5): 3 floats per
    // PIXEL. This probe runs one sample per pixel over width*height pixels
    // and capacity == width*height (checked above), so the film is
    // capacity*3 floats -- but it is sized from the pixel count on purpose,
    // because that is what it is, and a probe that one day runs several
    // samples per pixel must not silently over-allocate instead of failing.
    //
    // TRANSFER_DST as well as STORAGE: it is zeroed with vkCmdFillBuffer at
    // the top of every run's command buffer (see below). Unlike the three
    // probe sinks above, which every dispatch overwrites wholesale, this one
    // is READ-MODIFY-WRITTEN, so a stale prior value is not overwritten --
    // it is added to.
    const uint32_t filmPixelCount = width * height;
    GpuBuffer filmBuffer;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    if (ok) {
        filmBuffer = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: film buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // --- The four stages. Built exactly once each, because this probe has
    // no reason to rebuild one. ComputePipeline::build IS safe to call again
    // -- commit e2f88e7 gave it a re-entrancy guard (compute_pipeline.cpp:
    // `if (m_device != VK_NULL_HANDLE) destroy(m_device);`), so a second
    // build() destroys the first build's objects rather than leaking them.
    // Building once here is a statement about this probe, not a constraint
    // to work around. ---
    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatter;

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    // state, queues, counters, vertex positions, triangle indices, TLAS --
    // the acceleration structure last, so bindBuffers can write all five
    // storage buffers as one contiguous prefix.
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // state, queues, counters, debug draws, env marginal CDF, env
    // conditional CDF, env samples, NEE samples, TLAS --
    // wf_scatter.comp's bindings 0..9. The eight storage buffers 0-7 are one
    // contiguous prefix bindBuffers can write; the acceleration structure at
    // 8 and the film at 9 are bound one at a time after it.
    //
    // Binding 8 is by DEFAULT the SAME TLAS intersect traces against:
    // next-event estimation's shadow rays and the path's own rays see one
    // scene, which is what makes check 28's "inside a closed box every
    // shadow ray is occluded" a statement about the scene the paths are
    // actually in. `unoccludedShadowRays` is the one documented exception --
    // see this function's doc comment for the film check that needs it and
    // why nothing about the estimator is concluded from such a run.
    // ... and the film at 9, after the acceleration structure, so it is
    // bound with bindStorageBuffer rather than through bindBuffers' prefix.
    // Eleven, not ten: binding 10 is the gradient arena (Stage 1 Task 2).
    // See runWavefrontScatterProbe's note at its binding-10 bind for why the
    // probes that have no arena re-bind the film buffer there.
    const VkDescriptorType kScatterBindings[11] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
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
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),       buffers.queueBuffer(),
                                            buffers.counterBuffer(),     debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer,     neeSamplesBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            // scatterTLAS is accel's TLAS unless the caller asked for
            // unoccluded shadow rays -- see the shadow-scene block above.
            !scatter.bindAccelerationStructure(m_device, 8, scatterTLAS) ||
            !scatter.bindStorageBuffer(m_device, 9, filmBuffer.buffer) ||
            // Binding 10, the gradient arena: none here, so the film is
            // re-bound and ScatterPush::gradArenaFloats stays 0. See
            // runWavefrontScatterProbe's note at the same call.
            !scatter.bindStorageBuffer(m_device, 10, filmBuffer.buffer)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontFusedLoopProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        // Camera at the box CENTRE. That is the survival induction's base
        // case -- an origin in the open interior -- and unlike the staircase
        // it replaced, no direction is privileged: whichever way a primary
        // ray goes it leaves through a face, so the camera basis below is
        // free to be the plain identity one. These fields are pushed from
        // the same constants the static_asserts above check, so the camera
        // the build-time guard reasons about is the camera the probe uses.
        GeneratePush genPush{};
        genPush.origin[0] = kFusedLoopCameraX;
        genPush.origin[1] = kFusedLoopCameraY;
        genPush.origin[2] = kFusedLoopCameraZ;
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
        genPush.tanHalfFov = kFusedLoopTanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        // Fixed dispatch, used here as 1-D: groupsY/groupsZ are left at
        // Fixed's default of 1 (Fixed itself now supports setting them to
        // get a genuine 3-D dispatch -- see kFusedLoopGenerateLocalY's
        // comment above -- this call site just doesn't use that), so this is
        // (width/8, 1, 1) groups x local_size (8,8), covering exactly
        // width x 8 pixels. See the height check above.
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop loop;
        // Named rather than positional: Config grew material fields BETWEEN
        // albedo and iterationSeed (Stage 0b-2b Task 2), and a positional
        // aggregate here would have silently re-bound iterationSeed to
        // roughness had the compiler not caught the narrowing. The material
        // is left at Config's defaults -- the pure Lambertian configuration
        // for which the per-bounce estimator weight is exactly `albedo`, so
        // checks 16-18 keep asserting what they always asserted.
        WavefrontLoop::Config loopConfig;
        loopConfig.albedo = albedo;
        loopConfig.iterationSeed = iterationSeed;
        // The film is caller-owned, so its size is Config's to state and not
        // `buffers`' -- see WavefrontLoop::Config::filmPixelCount.
        loopConfig.filmPixelCount = filmPixelCount;
        loop.setConfig(loopConfig);
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);
        loop.setScatter(scatter);

        outDrawsPerBounce.resize(maxBounces);
        outLiveCountPerRun.resize(maxBounces);
        if (outNeeSamplesPerRun != nullptr) outNeeSamplesPerRun->resize(maxBounces);
        if (outFilmPerRun != nullptr) outFilmPerRun->resize(maxBounces);

        for (uint32_t bounces = 1; ok && bounces <= maxBounces; ++bounces) {
            const WavefrontLoop::Ring finalRing = WavefrontLoop::finalLiveRing(capacity, bounces);

            runImmediate([&](VkCommandBuffer cmd) {
                // Fresh state for every run: zero() ends with its own
                // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier, so
                // generate's first read of the counter is ordered against it.
                buffers.zero(cmd);

                // The film is NOT part of WavefrontBuffers, so zero() does
                // not touch it and record() will not either -- record()
                // zeroes nothing it does not own. wf_scatter.comp atomicAdds
                // into this buffer, i.e. it READS the previous value, so an
                // un-zeroed film accumulates onto whatever the allocator
                // handed back (and, across the runs of this loop, onto the
                // previous run's total). This fill plus its
                // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier is the
                // caller-side obligation wf_scatter.comp's film note names.
                // dstAccessMask includes SHADER_WRITE for the usual reason:
                // the first thing the shader does to these bytes is an
                // atomicAdd, which is a read-modify-write.
                vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);
                VkBufferMemoryBarrier filmZeroBarrier{};
                filmZeroBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                filmZeroBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                filmZeroBarrier.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                filmZeroBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.buffer = filmBuffer.buffer;
                filmZeroBarrier.offset = 0;
                filmZeroBarrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &filmZeroBarrier, 0, nullptr);

                // THE fused loop -- one command buffer, no vkQueueWaitIdle
                // anywhere inside it. Everything that orders the stages
                // against each other is a barrier recorded by
                // WavefrontLoop::record.
                //
                // debugDrawsBuffer is passed as an extra barrier buffer:
                // wf_scatter.comp writes its (u1, u2, drawCount) triple at a
                // FIXED pathIndex*3 offset on every bounce, so for bounces >= 2
                // successive scatter dispatches in this ONE command buffer
                // overwrite the same bytes. It is caller-owned -- it is not
                // part of WavefrontBuffers -- so the loop's own barriers name
                // only state/queue/counter and nothing would otherwise make
                // bounce k's write available before bounce k+1's. Check 18
                // depends on the LAST bounce's write being the survivor. See
                // wavefront_loop.hpp's "Caller-owned buffers the loop must
                // also order".
                //
                // envSamplesBuffer is here for the identical reason: every
                // scatter dispatch writes the same pathIndex*4 slots.
                // buffers.envMarginalBuffer()/envConditionalBuffer() are NOT
                // -- no dispatch writes them, and extraBarrierBuffers orders
                // buffers the dispatches write.
                // neeSamplesBuffer joins them for the third time over: same
                // fixed per-path offset, same overwrite between bounces of
                // one command buffer, same absence of any barrier that would
                // otherwise order it.
                //
                // filmBuffer is the fourth, and the one where forgetting
                // would cost real radiance rather than a stale diagnostic:
                // wf_scatter.comp atomicAdds into film[pixelIndex*3..+2]
                // every bounce, so bounce k+1 both READS and WRITES the
                // bytes bounce k wrote. Barrier (7) inside
                // WavefrontLoop::recordCompactingStage is what makes that
                // read see that write, and it only covers the buffers named
                // in this span. See wf_scatter.comp's note at the atomicAdd
                // and wavefront_loop.hpp's "Caller-owned buffers the loop
                // must also order".
                const VkBuffer loopExtras[4] = {debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                                neeSamplesBuffer.buffer, filmBuffer.buffer};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Everything below is this probe's own readback plumbing,
                // deliberately NOT part of WavefrontLoop::record (only the
                // caller knows what consumes the loop's output).
                //
                // EVERY buffer this function reads back through a mapped
                // pointer has to be named here. envSamplesBuffer was missing
                // from this list before Stage 0b-2b Task 4 even though the
                // readback below invalidates and maps it: vmaInvalidate-
                // Allocation only handles the CPU cache side, it is not a
                // substitute for the GPU-side availability operation, and
                // vkQueueWaitIdle alone does not make writes visible in the
                // host domain per the Vulkan spec. Nothing detected it --
                // synchronization validation has been MEASURED silent on
                // this subsystem's hazards (wavefront_loop.hpp's class
                // comment records disabling every compute-side barrier with
                // zero SYNC- diagnostics) -- so a green run was never
                // evidence either way. The sibling runWavefrontScatterProbe
                // has always listed all of its readback targets; this one
                // now does too, neeSamplesBuffer and (Stage 0b-2b Task 5)
                // filmBuffer included -- the film is read back through a
                // mapped pointer below exactly like the other three sinks,
                // so it needs the same SHADER_WRITE -> HOST_READ availability
                // operation and nothing else provides one.
                VkBufferMemoryBarrier toHost[6]{};
                const VkBuffer hostRead[6] = {buffers.stateBuffer(),   buffers.counterBuffer(),
                                              debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                              neeSamplesBuffer.buffer, filmBuffer.buffer};
                for (int i = 0; i < 6; ++i) {
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

                const VkBufferMemoryBarrier post[7] = {toHost[0], toHost[1], toHost[2],
                                                       toHost[3], toHost[4], toHost[5],
                                                       queueToTransfer};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 7, post, 0, nullptr);

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
                mappedDebug, mappedDebug + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
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

            // --- Per-run sinks (Stage 0b-2b Task 5). Read INSIDE the loop,
            // unlike outEnvSamples/outNeeSamples below, because their whole
            // value is that each run exposes a DIFFERENT bounce: run b+1
            // leaves bounce b's NEE record in binding 7 (the same argument
            // outDrawsPerBounce rests on), and leaves the film holding the
            // total over exactly b+1 bounces because it is re-zeroed at the
            // top of every run's command buffer.
            if (outNeeSamplesPerRun != nullptr) {
                m_allocator.invalidateBuffer(neeSamplesBuffer);
                const auto* mappedNeeRun =
                    static_cast<const float*>(neeSamplesBuffer.getMappedData());
                if (mappedNeeRun == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE "
                                          "samples buffer not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                (*outNeeSamplesPerRun)[bounces - 1].assign(
                    mappedNeeRun,
                    mappedNeeRun + (static_cast<std::size_t>(capacity) * kNeeSampleFloats));
            }
            if (outFilmPerRun != nullptr) {
                m_allocator.invalidateBuffer(filmBuffer);
                const auto* mappedFilm = static_cast<const float*>(filmBuffer.getMappedData());
                if (mappedFilm == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: film "
                                          "buffer not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                (*outFilmPerRun)[bounces - 1].assign(
                    mappedFilm, mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
            }
        }

        // outEnvSamples, when requested: binding 6 after the FINAL run,
        // capacity*4 floats (dirX, dirY, dirZ, pdf per path index), written
        // by ScatterPush fields THIS function never touches -- they are
        // filled by ohao::diff::WavefrontLoop::record from `buffers`, not by
        // this probe. See this function's doc comment.
        if (ok && outEnvSamples != nullptr) {
            m_allocator.invalidateBuffer(envSamplesBuffer);
            const auto* mappedEnv = static_cast<const float*>(envSamplesBuffer.getMappedData());
            if (mappedEnv == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: env samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outEnvSamples->assign(mappedEnv,
                                      mappedEnv + (static_cast<std::size_t>(capacity) * kEnvSampleFloats));
            }
        }

        // outNeeSamples, when requested: binding 7 after the FINAL run. The
        // scene is the closed box, so every shadow ray traced into it is
        // occluded and every contribution here must be exactly zero.
        if (ok && outNeeSamples != nullptr) {
            m_allocator.invalidateBuffer(neeSamplesBuffer);
            const auto* mappedNee = static_cast<const float*>(neeSamplesBuffer.getMappedData());
            if (mappedNee == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outNeeSamples->assign(
                    mappedNee,
                    mappedNee + (static_cast<std::size_t>(capacity) * kNeeSampleFloats));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
    if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}


// ===========================================================================
// Stage 1 Task 1 -- the REPLAY-EQUIVALENCE probe.
// ===========================================================================
//
// The same closed-box fused loop runWavefrontFusedLoopProbe runs, run TWICE
// per bounce count -- once through the FORWARD instantiation of
// shaders/includes/diff/traverse.glsl and once through the REPLAY one -- with
// each writing its own binding-3 vertex trace. See the doc comment in
// gpu_probe_context.hpp for why the replay is a second FULL run from zeroed
// buffers rather than a resumed dispatch, and for why it is handed nothing
// the forward run produced.
//
// WHAT IS DELIBERATELY DUPLICATED FROM runWavefrontFusedLoopProbe, AND WHY IT
// IS NOT A PARAMETER ON IT. That probe's expected values -- the bit-exact
// 0.0625 throughput, the per-bounce PathRng parity, the live counts -- are
// calibrated to exactly one configuration, and its own doc comment records
// the judgement that generalising it would put every one of those calibrated
// checks "one parameter default away from silently changing scene". This
// probe needs a different SHAPE of run (two loops per bounce count, against
// two different scatter SPVs, with two sets of sinks), not different values,
// so it is a sibling for the same reason runWavefrontParityProbe is. The
// SCENE, though, is shared by construction: buildAxisAlignedBoxGeometry and
// the kFusedLoop* constants below are the same objects, not copies of them,
// so the survival induction that makes "every path, every bounce" non-vacuous
// cannot drift between the two probes.
bool GpuProbeContext::runWavefrontReplayProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces, float albedo,
    uint32_t iterationSeed, std::vector<std::vector<float>>& outForwardTracePerBounce,
    std::vector<std::vector<float>>& outReplayTracePerBounce) {
    // Byte-identical to runWavefrontFusedLoopProbe's / runWavefrontGenerateProbe's.
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

    outForwardTracePerBounce.clear();
    outReplayTracePerBounce.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: buffers not built\n");
        return false;
    }
    // The dispatch-shape guard is runWavefrontFusedLoopProbe's, and
    // `width * height == capacity` carries a SECOND meaning here: it is
    // exactly "one path per pixel", i.e. one sample per pixel per dispatch,
    // which is the film-hazard option this subsystem took (spec 4.5; see the
    // long note on diffVertexHook in wf_scatter.comp). Refusing to run
    // without it is half of how that option is enforced; the other half is
    // the pixel-index histogram the consuming check measures.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontReplayProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y -- this probe's generate dispatch is 1-D), "
                     "width a non-zero multiple of %u, width*height == capacity (%u) -- which is "
                     "also the ONE-SAMPLE-PER-PIXEL condition the film-hazard resolution rests "
                     "on -- and maxBounces > 0; got %ux%u, maxBounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     maxBounces);
        return false;
    }

    // --- Scene: the closed box. Same builder, same constants, same survival
    // induction as runWavefrontFusedLoopProbe. ---
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(kFusedLoopBoxHalfExtent, positions, indices);

    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        std::span<const uint32_t>(indices),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: failed to create box "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: "
                              "RTAccelerationStructure::init failed\n");
        ok = false;
    }
    if (ok) {
        BlasHandle blas = INVALID_BLAS;
        runImmediate([&](VkCommandBuffer cmd) {
            blas = accel.createBLASFromPositions(vertexBuffer.buffer,
                                                 static_cast<uint32_t>(positions.size() / 3),
                                                 indexBuffer.buffer,
                                                 static_cast<uint32_t>(indices.size()),
                                                 /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        } else {
            accel.clearInstances();
            accel.addInstance(blas, glm::mat4(1.0f));
            runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
            if (accel.getTLAS() == VK_NULL_HANDLE) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: buildTLAS "
                                      "produced no TLAS\n");
                ok = false;
            }
        }
    }

    // --- Two INDEPENDENT sets of scatter-side sinks, one per instantiation.
    //
    // Separate allocations, not one set reused, so that nothing the replay
    // stage writes can reach a byte the forward comparison reads -- including
    // through a future hook that does touch the film or the NEE record. The
    // traces in particular are read back into two separate host vectors from
    // two separate device buffers; "read back independently" is what makes
    // the forward run its own oracle rather than a value the replay was
    // handed.
    struct ScatterSinks {
        GpuBuffer trace;  // binding 3, the vertex trace -- the only one read
        GpuBuffer env;    // binding 6
        GpuBuffer nee;    // binding 7
        GpuBuffer film;   // binding 9
    };
    ScatterSinks fwdSinks;
    ScatterSinks repSinks;
    const VkDeviceSize traceBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
    const VkDeviceSize envBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    const VkDeviceSize neeBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    const uint32_t filmPixelCount = width * height;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    ScatterSinks* const sinkSets[2] = {&fwdSinks, &repSinks};
    for (ScatterSinks* s : sinkSets) {
        if (!ok) break;
        s->trace = m_allocator.createBuffer(traceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            AllocationUsage::GpuToCpu,
                                            /*persistentlyMapped=*/true);
        s->env = m_allocator.createBuffer(envBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        s->nee = m_allocator.createBuffer(neeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        // TRANSFER_DST: the film is read-modify-written by the forward hook,
        // so it is zero-filled at the top of every run's command buffer.
        s->film = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!s->trace.isValid() || !s->env.isValid() || !s->nee.isValid() || !s->film.isValid()) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontReplayProbe: scatter sink allocation "
                         "failed\n");
            ok = false;
        }
    }

    // --- Stages. generate/prepare_indirect/intersect are shared by both
    // runs -- the whole point is that only the SCATTER stage differs. ---
    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatterForward;
    WavefrontStage scatterReplay;
    // Index-parallel to sinkSets: variant 0 is the FORWARD instantiation,
    // variant 1 the REPLAY one. The pairing is expressed once, here, so the
    // run loop below cannot bind one variant's stage to the other's sinks.
    WavefrontStage* const scatterStages[2] = {&scatterForward, &scatterReplay};

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // Both scatter instantiations declare the SAME bindings, because both
    // include the same traverse.glsl -- that is the structural claim this
    // task rests on, and here it shows up as one binding-type array used
    // twice rather than two that have to be kept in step.
    // Eleven, not ten: binding 10 is the gradient arena (Stage 1 Task 2).
    // See runWavefrontScatterProbe's note at its binding-10 bind for why the
    // probes that have no arena re-bind the film buffer there.
    const VkDescriptorType kScatterBindings[11] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: prepare_indirect build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatterForward.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                                    sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: forward scatter build\n");
        ok = false;
    }
    // The REPLAY instantiation. Before shaders/diff/wf_scatter_replay.comp
    // exists this is where the probe fails, and the message says so plainly:
    // the check that consumes this probe is written before the shader is, so
    // "no replay stage yet" must read as a clear failure and not as a
    // mysterious Vulkan error.
    if (ok && !scatterReplay.build(m_device, "diff_wf_scatter_replay.comp.spv", kScatterBindings,
                                   sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontReplayProbe: REPLAY scatter build failed "
                     "(diff_wf_scatter_replay.comp.spv). If that shader does not exist yet, this "
                     "is the expected failure: there is no second instantiation of the traversal "
                     "to compare the forward one against\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        ok = generate.bindBuffers(m_device, stateQueueCounter) &&
             prepareIndirect.bindBuffers(m_device, counterOnly) &&
             intersect.bindBuffers(m_device, intersectBuffers) &&
             intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS());
        for (int i = 0; ok && i < 2; ++i) {
            const ScatterSinks& s = *sinkSets[i];
            const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),
                                                buffers.queueBuffer(),
                                                buffers.counterBuffer(),
                                                s.trace.buffer,
                                                buffers.envMarginalBuffer(),
                                                buffers.envConditionalBuffer(),
                                                s.env.buffer,
                                                s.nee.buffer};
            // ONE TLAS for both: the shadow rays and the path rays see one
            // scene, which is what keeps the two runs comparable at all.
            ok = scatterStages[i]->bindBuffers(m_device, scatterBuffers) &&
                 scatterStages[i]->bindAccelerationStructure(m_device, 8, accel.getTLAS()) &&
                 scatterStages[i]->bindStorageBuffer(m_device, 9, s.film.buffer) &&
                 // Binding 10, the gradient arena. This probe has none --
                 // it is about the two traversals walking ONE path, not
                 // about gradients -- so each variant re-binds its OWN
                 // film there and gradArenaFloats stays 0. See
                 // runWavefrontScatterProbe's note at the same call.
                 scatterStages[i]->bindStorageBuffer(m_device, 10, s.film.buffer);
        }
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: descriptor binding\n");
        }
    }

    if (ok) {
        // Camera at the box centre -- the survival induction's base case.
        GeneratePush genPush{};
        genPush.origin[0] = kFusedLoopCameraX;
        genPush.origin[1] = kFusedLoopCameraY;
        genPush.origin[2] = kFusedLoopCameraZ;
        genPush.forward[2] = 1.0f;
        genPush.right[0] = 1.0f;
        genPush.up[1] = 1.0f;
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = kFusedLoopTanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop::Config loopConfig;
        loopConfig.albedo = albedo;
        loopConfig.iterationSeed = iterationSeed;
        loopConfig.filmPixelCount = filmPixelCount;
        // Material left at Config's defaults -- the pure Lambertian
        // configuration whose per-bounce estimator weight is exactly
        // `albedo`, which is what lets the consuming check assert the traced
        // throughput against albedo^bounce EXACTLY and so establish that the
        // records it is comparing are not two buffers of zeros.

        WavefrontLoop loop;
        loop.setConfig(loopConfig);
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);

        outForwardTracePerBounce.resize(maxBounces);
        outReplayTracePerBounce.resize(maxBounces);

        for (uint32_t bounces = 1; ok && bounces <= maxBounces; ++bounces) {
            for (int variant = 0; ok && variant < 2; ++variant) {
                ScatterSinks& s = *sinkSets[variant];
                // The ONLY difference between the two runs of this loop.
                loop.setScatter(*scatterStages[variant]);

                runImmediate([&](VkCommandBuffer cmd) {
                    // Fresh path state for EVERY run, forward and replay
                    // alike. The replay therefore starts from precisely what
                    // the forward started from -- zeroed buffers and the same
                    // seed -- and from nothing the forward produced.
                    buffers.zero(cmd);

                    // The film is caller-owned, so record() will not zero it
                    // (it zeroes nothing it does not own) and the forward
                    // hook atomicAdds into it. TRANSFER_WRITE ->
                    // SHADER_READ|SHADER_WRITE because the first thing the
                    // shader does to these bytes is a read-modify-write.
                    vkCmdFillBuffer(cmd, s.film.buffer, 0, VK_WHOLE_SIZE, 0u);
                    VkBufferMemoryBarrier filmZero{};
                    filmZero.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    filmZero.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    filmZero.dstAccessMask =
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    filmZero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    filmZero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    filmZero.buffer = s.film.buffer;
                    filmZero.offset = 0;
                    filmZero.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                         &filmZero, 0, nullptr);

                    // EVERY caller-owned buffer these dispatches WRITE goes
                    // through extraBarrierBuffers -- the trace at a fixed
                    // pathIndex*kDebugDrawFloats offset every bounce, the env
                    // and NEE sinks likewise, and the film, which is
                    // read-modify-written. Omitting any of them is a real
                    // missing memory dependency that nothing here would
                    // detect (wavefront_loop.hpp's class comment: every
                    // compute-side barrier deleted, zero SYNC- diagnostics).
                    // The env CDF buffers are deliberately absent: no
                    // dispatch writes them.
                    const VkBuffer loopExtras[4] = {s.trace.buffer, s.env.buffer, s.nee.buffer,
                                                    s.film.buffer};
                    loop.record(cmd, buffers, bounces, loopExtras);

                    // This probe reads back exactly ONE buffer through a
                    // mapped pointer -- the trace -- so exactly one buffer is
                    // named in a SHADER_WRITE -> HOST_READ barrier.
                    // vmaInvalidateAllocation handles the CPU cache side
                    // only; it is not a substitute for the GPU-side
                    // availability operation, and vkQueueWaitIdle alone does
                    // not make writes visible in the host domain per the
                    // Vulkan spec.
                    VkBufferMemoryBarrier toHost{};
                    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.buffer = s.trace.buffer;
                    toHost.offset = 0;
                    toHost.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);
                });

                m_allocator.invalidateBuffer(s.trace);
                const auto* mapped = static_cast<const float*>(s.trace.getMappedData());
                if (mapped == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: trace buffer "
                                          "not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                std::vector<float>& out = (variant == 0)
                                              ? outForwardTracePerBounce[bounces - 1]
                                              : outReplayTracePerBounce[bounces - 1];
                out.assign(mapped,
                           mapped + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    scatterReplay.destroy(m_device);
    scatterForward.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    for (ScatterSinks* s : sinkSets) {
        if (s->film.isValid()) m_allocator.destroyBuffer(s->film);
        if (s->nee.isValid()) m_allocator.destroyBuffer(s->nee);
        if (s->env.isValid()) m_allocator.destroyBuffer(s->env);
        if (s->trace.isValid()) m_allocator.destroyBuffer(s->trace);
    }
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}


// ===========================================================================
// Stage 0b-2b Task 6 -- the PARITY probe.
// ===========================================================================
//
// A caller-supplied scene driven through the SAME ohao::diff::WavefrontLoop
// the fused-loop probe uses, at one sample per pixel per run, once per seed.
// See the doc comment in gpu_probe_context.hpp for why this is a sibling of
// runWavefrontFusedLoopProbe rather than more parameters on it.
bool GpuProbeContext::runWavefrontParityProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
    const WavefrontGenerateCamera& camera, std::span<const float> positions,
    std::span<const uint32_t> indices, float albedo, const WavefrontScatterMaterial& material,
    std::span<const uint32_t> iterationSeeds, std::vector<std::vector<float>>& outFilmPerSeed,
    std::vector<uint32_t>& outLiveCountPerSeed) {
    // Byte-identical to runWavefrontGenerateProbe's / runWavefrontFusedLoop-
    // Probe's GeneratePush (80 bytes, see those functions).
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

    outFilmPerSeed.clear();
    outLiveCountPerSeed.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: buffers not built\n");
        return false;
    }
    // Same dispatch-shape requirement as runWavefrontFusedLoopProbe, and for
    // the same reason: this probe's generate dispatch sets groupCountX only,
    // so one dispatch covers exactly one row of local_size_y = 8 pixels.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity || bounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontParityProbe: requires height == %u, width a "
                     "non-zero multiple of %u, width*height == capacity (%u) and bounces > 0; "
                     "got %ux%u, bounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     bounces);
        return false;
    }
    if (positions.size() < 9u || positions.size() % 3u != 0u || indices.size() < 3u ||
        indices.size() % 3u != 0u || iterationSeeds.empty()) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontParityProbe: needs at least one triangle "
                     "(3 floats per vertex, 3 indices per triangle) and at least one seed; got "
                     "%zu floats, %zu indices, %zu seeds\n",
                     positions.size(), indices.size(), iterationSeeds.size());
        return false;
    }

    // --- Scene. ONE triangle soup, bound to the primary trace (as
    // acceleration structure AND as wf_intersect.comp's vertex/index storage
    // buffers, which is how it recovers the geometric normal) and to
    // wf_scatter.comp's shadow rays. Not two.
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: failed to create scene "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: "
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- Sinks. debugDraws (3), envSamples (6) and neeSamples (7) are
    // allocated and bound but NEVER read back here: a descriptor set must
    // cover every binding the shader statically uses, and this probe's
    // subject is the film. They still go through record()'s
    // extraBarrierBuffers below, because every scatter dispatch in the fused
    // run overwrites the same per-path offsets in them and the loop's own
    // barriers name only state/queue/counter -- an unordered write is a
    // hazard whether or not anyone reads the result.
    GpuBuffer debugDrawsBuffer;
    GpuBuffer envSamplesBuffer;
    GpuBuffer neeSamplesBuffer;
    GpuBuffer filmBuffer;
    const uint32_t filmPixelCount = width * height;
    if (ok) {
        debugDrawsBuffer =
            m_allocator.createBuffer(static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats *
                                         sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        envSamplesBuffer =
            m_allocator.createBuffer(static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats *
                                         sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        neeSamplesBuffer = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        // TRANSFER_DST as well as STORAGE: read-modify-written by atomicAdd,
        // so it is zeroed with vkCmdFillBuffer at the top of every run.
        filmBuffer = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid() || !envSamplesBuffer.isValid() ||
            !neeSamplesBuffer.isValid() || !filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: sink buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatter;

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // Eleven, not ten: binding 10 is the gradient arena (Stage 1 Task 2).
    // See runWavefrontScatterProbe's note at its binding-10 bind for why the
    // probes that have no arena re-bind the film buffer there.
    const VkDescriptorType kScatterBindings[11] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: prepare_indirect build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatter.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                             sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: scatter build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        const VkBuffer scatterBuffers[8] = {
            buffers.stateBuffer(),       buffers.queueBuffer(),
            buffers.counterBuffer(),     debugDrawsBuffer.buffer,
            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
            envSamplesBuffer.buffer,     neeSamplesBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            // The SAME TLAS the path rays trace. Deliberately not a second
            // one -- see this function's doc comment.
            !scatter.bindAccelerationStructure(m_device, 8, accel.getTLAS()) ||
            !scatter.bindStorageBuffer(m_device, 9, filmBuffer.buffer) ||
            // Binding 10, the gradient arena: none here, so the film is
            // re-bound and ScatterPush::gradArenaFloats stays 0. See
            // runWavefrontScatterProbe's note at the same call.
            !scatter.bindStorageBuffer(m_device, 10, filmBuffer.buffer)) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        GeneratePush genPush{};
        for (int i = 0; i < 3; ++i) {
            genPush.origin[i] = camera.origin[i];
            genPush.forward[i] = camera.forward[i];
            genPush.right[i] = camera.right[i];
            genPush.up[i] = camera.up[i];
        }
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = camera.tanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop loop;
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);
        loop.setScatter(scatter);

        outFilmPerSeed.resize(iterationSeeds.size());
        outLiveCountPerSeed.resize(iterationSeeds.size());

        const WavefrontLoop::Ring finalRing = WavefrontLoop::finalLiveRing(capacity, bounces);

        for (std::size_t s = 0; ok && s < iterationSeeds.size(); ++s) {
            // Named-field assignment, not a positional aggregate: Config has
            // grown fields BETWEEN albedo and iterationSeed twice on this
            // branch already.
            WavefrontLoop::Config loopConfig;
            loopConfig.albedo = albedo;
            loopConfig.roughness = material.roughness;
            loopConfig.metallic = material.metallic;
            loopConfig.specularWeight = material.specularWeight;
            loopConfig.filmPixelCount = filmPixelCount;
            loopConfig.iterationSeed = iterationSeeds[s];
            loop.setConfig(loopConfig);

            runImmediate([&](VkCommandBuffer cmd) {
                buffers.zero(cmd);

                // The film is caller-owned, so record() neither zeroes it nor
                // knows its size. It is READ-modify-written by atomicAdd, so
                // an un-zeroed film accumulates onto the previous seed's
                // total; dstAccessMask includes SHADER_WRITE because the
                // first thing the shader does to these bytes is that
                // read-modify-write.
                vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);
                VkBufferMemoryBarrier filmZeroBarrier{};
                filmZeroBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                filmZeroBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                filmZeroBarrier.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                filmZeroBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.buffer = filmBuffer.buffer;
                filmZeroBarrier.offset = 0;
                filmZeroBarrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &filmZeroBarrier, 0, nullptr);

                // Every caller-owned buffer a scatter dispatch WRITES. The
                // film is the one that costs radiance rather than a stale
                // diagnostic if it is omitted: bounce k+1 both reads and
                // writes the bytes bounce k wrote, and barrier (7) inside
                // WavefrontLoop::recordCompactingStage -- which covers only
                // the buffers named in this span -- is the only thing that
                // makes that read see that write. See wf_scatter.comp's note
                // at the atomicAdd. The three unread sinks are here for the
                // same structural reason, not because anything reads them.
                const VkBuffer loopExtras[4] = {debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                                neeSamplesBuffer.buffer, filmBuffer.buffer};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Host-read availability for exactly the two buffers this
                // function reads back: the counter (through
                // buffers.readbackCounter) and the film (through a mapped
                // pointer). vkQueueWaitIdle does not make writes visible in
                // the host domain, and vmaInvalidateAllocation only handles
                // the CPU cache side. Synchronization validation has been
                // MEASURED silent on this subsystem, so a green run is not
                // evidence that this list is complete -- it is complete
                // because it names every buffer read below and nothing else
                // is read.
                VkBufferMemoryBarrier toHost[2]{};
                const VkBuffer hostRead[2] = {buffers.counterBuffer(), filmBuffer.buffer};
                for (int i = 0; i < 2; ++i) {
                    toHost[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].buffer = hostRead[i];
                    toHost[i].offset = 0;
                    toHost[i].size = VK_WHOLE_SIZE;
                }
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, toHost, 0,
                                     nullptr);
            });

            outLiveCountPerSeed[s] = buffers.readbackCounter(m_allocator, finalRing.countSlot);

            m_allocator.invalidateBuffer(filmBuffer);
            const auto* mappedFilm = static_cast<const float*>(filmBuffer.getMappedData());
            if (mappedFilm == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: film buffer not "
                                      "mapped, cannot read back\n");
                ok = false;
                break;
            }
            outFilmPerSeed[s].assign(mappedFilm,
                                     mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
        }
    }

    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}


// ===========================================================================
// Stage 1 Task 2 -- the GRADIENT probe.
// ===========================================================================
//
// One evaluation point of (film, dJ/d(albedo)) on a caller-supplied scene,
// produced by two fused runs of ohao::diff::WavefrontLoop at one seed: the
// FORWARD instantiation for the film and the REPLAY one for the arena. See
// the doc comment in gpu_probe_context.hpp for why the two halves are kept on
// separate buffers and why this refuses to run outside the pure Lambertian
// configuration.
//
// WHY THIS IS NOT runWavefrontParityProbe WITH MORE PARAMETERS. That probe
// runs ONE stage (the forward one) many times, once per seed, because its
// subject is a Monte Carlo mean over seeds. This one runs TWO stages once,
// because its subject is a derivative at a single common-random-number
// realisation -- a second seed would be a second measurement, not a better
// one. Generalising the parity probe would have put its calibrated
// non-vacuity gates one parameter default away from a different question.
bool GpuProbeContext::runWavefrontGradientProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
    const WavefrontGenerateCamera& camera, std::span<const float> positions,
    std::span<const uint32_t> indices, float albedo, const WavefrontScatterMaterial& material,
    uint32_t iterationSeed, GradientArena& arena, uint32_t gradArenaFloats,
    uint32_t gradAlbedoOffset, std::vector<float>& outFilm,
    const WavefrontGradientOptions& options) {
    // Byte-identical to runWavefrontGenerateProbe's (80 bytes).
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

    outFilm.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: buffers not built\n");
        return false;
    }
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity || bounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: requires height == %u, width a "
                     "non-zero multiple of %u, width*height == capacity (%u) -- which is also the "
                     "ONE-SAMPLE-PER-PIXEL condition the film-hazard resolution rests on -- and "
                     "bounces > 0; got %ux%u, bounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     bounces);
        return false;
    }
    if (positions.size() < 9u || positions.size() % 3u != 0u || indices.size() < 3u ||
        indices.size() % 3u != 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: needs at least one triangle "
                     "(3 floats per vertex, 3 indices per triangle); got %zu floats, %zu indices\n",
                     positions.size(), indices.size());
        return false;
    }
    // THE MATERIAL REFUSAL. See the doc comment: bsdf_adjoint.glsl's
    // derivative is exact only at metallic == 0 (where F0 and the lobe
    // probability q stop depending on the base colour) and its throughput
    // term is exact only at specularWeight == 0 (where bsdf.glsl's fast path
    // makes the per-bounce weight EXACTLY `albedo`). Outside that, a green
    // gradient check would be measuring a derivative of something other than
    // what it thinks -- and at metallic > 0 the finite difference would not
    // even be comparing two runs of the same path.
    if (options.diffParam == 0u &&
        (material.metallic != 0.0f || material.specularWeight != 0.0f)) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: refuses to run with metallic "
                     "%.9g / specularWeight %.9g. shaders/includes/diff/bsdf_adjoint.glsl is the "
                     "PURE LAMBERTIAN derivative only: at metallic > 0 the base colour enters both "
                     "F0 and the lobe-selection probability, so a +/-h perturbation of the albedo "
                     "MOVES THE SAMPLED DIRECTION and the common-random-number comparison is "
                     "between two different paths; at specularWeight > 0 the per-bounce weight "
                     "stops being exactly `albedo` and the throughput term's closed form fails. "
                     "Task 3 replaces those bodies FOR ROUGHNESS AND METALLIC (diffParam 1 and 2, "
                     "which carry their own preconditions below); for the base colour this is "
                     "still a refusal, not a tolerance\n",
                     static_cast<double>(material.metallic),
                     static_cast<double>(material.specularWeight));
        return false;
    }
    // --- THE TASK 3 PRECONDITIONS. Not a relaxation of the one above: a
    // different parameter needs different things to be true, and each of these
    // is a condition without which the measurement would be vacuous or wrong
    // rather than merely awkward.
    if (options.diffParam == 1u || options.diffParam == 2u) {
        if (material.metallic <= 0.0f && material.specularWeight <= 0.0f) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                         "roughness or metallic at metallic %.9g / specularWeight %.9g. The "
                         "lobe-selection probability q = clamp(mix(specScale * maxF * (1-0.9r), "
                         "1, metallic), 0, 1) is then identically 0, diffBsdfEval returns before "
                         "it evaluates D, G or F at all, and BOTH gradients are exactly zero -- "
                         "which a finite difference would confirm, vacuously\n",
                         static_cast<double>(material.metallic),
                         static_cast<double>(material.specularWeight));
            return false;
        }
        if (!(material.roughness > 0.01f)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                         "at roughness %.9g. pbr_unpack.glsl floors roughness at 0.01, so at or "
                         "below the floor d(unpacked)/d(pushed) is 0 on one side and 1 on the "
                         "other: the analytic derivative reports 0 and a central difference "
                         "reports half the unfloored slope. A run must sit strictly above it, "
                         "with room for +/-h\n",
                         static_cast<double>(material.roughness));
            return false;
        }
    }
    if (options.diffParam == 2u && !(material.metallic > 0.0f && material.metallic < 1.0f)) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                     "metallic at metallic %.9g. unpackHitPbr clamps it to [0,1], so at either "
                     "endpoint the derivative is ONE-SIDED -- the adjoint reports 0 there and a "
                     "central difference reports half the interior slope. A metallic gradient run "
                     "must sit strictly inside, with room for +/-h\n",
                     static_cast<double>(material.metallic));
        return false;
    }
    if (arena.buffer() == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: gradient arena not "
                              "built\n");
        return false;
    }

    // --- Scene. ONE triangle soup, bound to the primary trace (acceleration
    // structure plus wf_intersect.comp's vertex/index storage buffers) and to
    // the traversal's shadow rays. Not two.
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: failed to create scene "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: "
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- Sinks. Two INDEPENDENT sets, one per instantiation, for
    // runWavefrontReplayProbe's reason: nothing the replay run writes may
    // reach a byte the forward run's film is read out of. debugDraws (3),
    // envSamples (6) and neeSamples (7) are allocated and bound but never
    // read -- a descriptor set must cover every binding the shader statically
    // uses -- and still go through extraBarrierBuffers, because every scatter
    // dispatch overwrites the same per-path offsets in them.
    struct ScatterSinks {
        GpuBuffer trace;
        GpuBuffer env;
        GpuBuffer nee;
        GpuBuffer film;
    };
    ScatterSinks fwdSinks;
    ScatterSinks repSinks;
    ScatterSinks* const sinkSets[2] = {&fwdSinks, &repSinks};
    const uint32_t filmPixelCount = width * height;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    for (ScatterSinks* s : sinkSets) {
        if (!ok) break;
        // HOST-READABLE, unlike the other two sinks: Stage 1 Task 3's
        // frozen-direction measurement reads the FORWARD run's vertex trace
        // back and compares two renders' ray origins, directions and hit
        // distances bit for bit. That is how the detached instrument's
        // defining claim is MEASURED rather than argued.
        s->trace = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        s->env = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        s->nee = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        s->film = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!s->trace.isValid() || !s->env.isValid() || !s->nee.isValid() || !s->film.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: scatter sink "
                                  "allocation failed\n");
            ok = false;
        }
    }

    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatterForward;
    WavefrontStage scatterReplay;
    WavefrontStage* const scatterStages[2] = {&scatterForward, &scatterReplay};

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // Both instantiations declare the same eleven bindings, because both
    // include the same traverse.glsl. Binding 10 is the gradient arena and is
    // bound to the REAL arena for both -- unlike every other probe here,
    // which has none and re-binds its film there.
    const VkDescriptorType kScatterBindings[11] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatterForward.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                                    sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: forward scatter "
                              "build\n");
        ok = false;
    }
    if (ok && !scatterReplay.build(m_device, "diff_wf_scatter_replay.comp.spv", kScatterBindings,
                                   sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: REPLAY scatter build "
                              "failed (diff_wf_scatter_replay.comp.spv)\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        ok = generate.bindBuffers(m_device, stateQueueCounter) &&
             prepareIndirect.bindBuffers(m_device, counterOnly) &&
             intersect.bindBuffers(m_device, intersectBuffers) &&
             intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS());
        for (int i = 0; ok && i < 2; ++i) {
            const ScatterSinks& s = *sinkSets[i];
            const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),
                                                buffers.queueBuffer(),
                                                buffers.counterBuffer(),
                                                s.trace.buffer,
                                                buffers.envMarginalBuffer(),
                                                buffers.envConditionalBuffer(),
                                                s.env.buffer,
                                                s.nee.buffer};
            ok = scatterStages[i]->bindBuffers(m_device, scatterBuffers) &&
                 scatterStages[i]->bindAccelerationStructure(m_device, 8, accel.getTLAS()) &&
                 scatterStages[i]->bindStorageBuffer(m_device, 9, s.film.buffer) &&
                 // The REAL gradient arena, bound to BOTH instantiations. The
                 // forward one never writes it (its hook is the film write)
                 // and is pushed gradArenaFloats = 0 below besides.
                 scatterStages[i]->bindStorageBuffer(m_device, 10, arena.buffer());
        }
        if (!ok) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: descriptor binding\n");
        }
    }

    if (ok) {
        GeneratePush genPush{};
        for (int i = 0; i < 3; ++i) {
            genPush.origin[i] = camera.origin[i];
            genPush.forward[i] = camera.forward[i];
            genPush.right[i] = camera.right[i];
            genPush.up[i] = camera.up[i];
        }
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = camera.tanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop loop;
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);

        for (int variant = 0; ok && variant < 2; ++variant) {
            const bool isReplay = (variant == 1);
            ScatterSinks& s = *sinkSets[variant];
            loop.setScatter(*scatterStages[variant]);

            WavefrontLoop::Config loopConfig;
            loopConfig.albedo = albedo;
            loopConfig.roughness = material.roughness;
            loopConfig.metallic = material.metallic;
            loopConfig.specularWeight = material.specularWeight;
            loopConfig.filmPixelCount = filmPixelCount;
            // ONLY the replay run is given an arena. The forward run's
            // gradArenaFloats stays 0, which disables every gradient write in
            // its traversal -- so "the forward pass wrote no gradient" is
            // enforced by a push constant as well as by its hook being the
            // film write.
            loopConfig.gradArenaFloats = isReplay ? gradArenaFloats : 0u;
            loopConfig.gradAlbedoOffset = isReplay ? gradAlbedoOffset : 0u;
            // Both runs are pushed the SAME diffParam and the SAME sampling
            // material. They must be: the two instantiations walk one path
            // only while every push-constant field that steers the traversal
            // agrees, and both of these steer it -- diffParam gates the
            // tangent update in path state, and the sampling material gates
            // every direction.
            loopConfig.diffParam = options.diffParam;
            if (options.freezeSampling) {
                loopConfig.samplingAlbedo = options.samplingAlbedo;
                loopConfig.samplingRoughness = options.samplingMaterial.roughness;
                loopConfig.samplingMetallic = options.samplingMaterial.metallic;
                loopConfig.samplingSpecularWeight = options.samplingMaterial.specularWeight;
            }
            loopConfig.iterationSeed = iterationSeed;
            loop.setConfig(loopConfig);

            runImmediate([&](VkCommandBuffer cmd) {
                buffers.zero(cmd);

                // The film is caller-owned and read-modify-written, so it is
                // zeroed here with its own TRANSFER_WRITE ->
                // SHADER_READ|SHADER_WRITE barrier.
                vkCmdFillBuffer(cmd, s.film.buffer, 0, VK_WHOLE_SIZE, 0u);
                VkBufferMemoryBarrier filmZero{};
                filmZero.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                filmZero.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                filmZero.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                filmZero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZero.buffer = s.film.buffer;
                filmZero.offset = 0;
                filmZero.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &filmZero, 0, nullptr);

                // The arena is zeroed on BOTH runs, in the same command
                // buffer as the loop that follows -- GradientArena::zero
                // records the fill AND its TRANSFER_WRITE ->
                // SHADER_READ|SHADER_WRITE barrier, which is exactly the
                // configuration its own comment says that barrier becomes
                // load-bearing in.
                arena.zero(cmd);

                const VkBuffer loopExtras[5] = {s.trace.buffer, s.env.buffer, s.nee.buffer,
                                                s.film.buffer, arena.buffer()};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Host-read availability for exactly what this function reads
                // back: the film (mapped) and, on the replay run, the arena
                // (mapped, through GradientArena::readback). vkQueueWaitIdle
                // does not make writes visible in the host domain and
                // vmaInvalidateAllocation covers only the CPU cache side.
                VkBufferMemoryBarrier toHost[3]{};
                VkBuffer hostRead[3] = {s.film.buffer, arena.buffer(), VK_NULL_HANDLE};
                uint32_t hostReadCount = isReplay ? 2u : 1u;
                // Any host readback must be NAMED in this barrier, not merely
                // waited for: vkQueueWaitIdle does not make writes visible in
                // the host domain, and a VkBufferMemoryBarrier's memory scope
                // covers only the buffers it lists.
                if (!isReplay && options.outForwardTrace != nullptr) {
                    hostRead[hostReadCount] = s.trace.buffer;
                    ++hostReadCount;
                }
                for (uint32_t i = 0; i < hostReadCount; ++i) {
                    toHost[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].buffer = hostRead[i];
                    toHost[i].offset = 0;
                    toHost[i].size = VK_WHOLE_SIZE;
                }
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, hostReadCount,
                                     toHost, 0, nullptr);
            });

            if (!isReplay) {
                m_allocator.invalidateBuffer(s.film);
                const auto* mappedFilm = static_cast<const float*>(s.film.getMappedData());
                if (mappedFilm == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: film buffer "
                                          "not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                outFilm.assign(mappedFilm,
                               mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
                // The LAST bounce's vertex trace, if the caller asked for it.
                // It is what makes the frozen-direction claim MEASURABLE
                // rather than argued: two renders that walked the same path
                // wrote bit-identical origins, directions and hit distances
                // into these slots.
                if (options.outForwardTrace != nullptr) {
                    m_allocator.invalidateBuffer(s.trace);
                    const auto* mappedTrace = static_cast<const float*>(s.trace.getMappedData());
                    if (mappedTrace == nullptr) {
                        std::fprintf(stderr,
                                     "[GpuProbeContext] runWavefrontGradientProbe: vertex trace "
                                     "buffer not mapped, cannot read back\n");
                        ok = false;
                        break;
                    }
                    options.outForwardTrace->assign(
                        mappedTrace,
                        mappedTrace + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
                }
            }
        }
    }

    scatterReplay.destroy(m_device);
    scatterForward.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    for (ScatterSinks* s : sinkSets) {
        if (s->film.isValid()) m_allocator.destroyBuffer(s->film);
        if (s->nee.isValid()) m_allocator.destroyBuffer(s->nee);
        if (s->env.isValid()) m_allocator.destroyBuffer(s->env);
        if (s->trace.isValid()) m_allocator.destroyBuffer(s->trace);
    }
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
