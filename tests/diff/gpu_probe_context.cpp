#include "gpu_probe_context.hpp"

#include "context/probe_scene.hpp"

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

// The scene block that used to sit in this file's two anonymous
// namespaces, now shared via context/probe_scene.hpp. Pulled in by name so
// that every call site below reads exactly as it did when these were
// file-local -- a linkage change, not a value change.
using namespace probe_scene;  // NOLINT(google-build-using-namespace)
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

}  // namespace ohao::diff
