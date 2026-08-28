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
                                               std::vector<float>* outEnvSamples) {
    outQueueDst.clear();
    outDebugDraws.clear();
    if (outEnvSamples != nullptr) outEnvSamples->clear();

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
    // state, queues, counters, debug draws, env marginal CDF, env
    // conditional CDF, env samples -- wf_scatter.comp's bindings 0..6 in
    // order. The two env CDF buffers are read-only to the shader but are
    // ordinary storage buffers as far as the descriptor set is concerned.
    const VkDescriptorType scatterBindingTypes[7] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

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

    // wf_scatter.comp's environment-sample sink (binding 6): 4 floats per
    // path index. Allocated unconditionally -- the descriptor set needs
    // something valid bound there whether or not the caller asked to read it
    // back.
    GpuBuffer envSamplesBuffer;
    const VkDeviceSize envSamplesBytes = static_cast<VkDeviceSize>(capacity) * 4u * sizeof(float);
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

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer scatterBuffers[7] = {buffers.stateBuffer(),         buffers.queueBuffer(),
                                            buffers.counterBuffer(),       debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(),   buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !scatter.bindBuffers(m_device, scatterBuffers)) {
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
                                                     buffers.envIntegral()};
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
            VkBufferMemoryBarrier postScatter[4]{};
            VkBuffer writtenBuffers[4] = {buffers.stateBuffer(), buffers.counterBuffer(),
                                          debugDrawsBuffer.buffer, envSamplesBuffer.buffer};
            for (int i = 0; i < 4; ++i) {
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

            VkBufferMemoryBarrier postDispatch[5] = {postScatter[0], postScatter[1], postScatter[2],
                                                     postScatter[3], queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 5, postDispatch, 0, nullptr);

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

        if (outEnvSamples != nullptr) {
            m_allocator.invalidateBuffer(envSamplesBuffer);
            const auto* mappedEnv = static_cast<const float*>(envSamplesBuffer.getMappedData());
            if (mappedEnv == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: env samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outEnvSamples->assign(mappedEnv,
                                      mappedEnv + (static_cast<std::size_t>(capacity) * 4u));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    scatter.destroy(m_device);
    prepareIndirect.destroy(m_device);

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

bool GpuProbeContext::runWavefrontFusedLoopProbe(WavefrontBuffers& buffers, uint32_t width,
                                                 uint32_t height, uint32_t maxBounces, float albedo,
                                                 uint32_t iterationSeed,
                                                 std::vector<std::vector<float>>& outDrawsPerBounce,
                                                 std::vector<uint32_t>& outLiveCountPerRun,
                                                 std::vector<uint32_t>& outFinalQueue,
                                                 std::vector<float>* outEnvSamples) {
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
    const VkDeviceSize envSamplesBytes = static_cast<VkDeviceSize>(capacity) * 4u * sizeof(float);
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
    // conditional CDF, env samples -- wf_scatter.comp's bindings 0..6.
    const VkDescriptorType kScatterBindings[7] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        const VkBuffer scatterBuffers[7] = {buffers.stateBuffer(),       buffers.queueBuffer(),
                                            buffers.counterBuffer(),     debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers)) {
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
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalY});

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
        loop.setConfig(loopConfig);
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
                const VkBuffer loopExtras[2] = {debugDrawsBuffer.buffer, envSamplesBuffer.buffer};
                loop.record(cmd, buffers, bounces, loopExtras);

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
                                      mappedEnv + (static_cast<std::size_t>(capacity) * 4u));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
