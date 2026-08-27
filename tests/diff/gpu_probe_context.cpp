#include "gpu_probe_context.hpp"

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

}  // namespace

bool GpuProbeContext::init() {
    // --- Instance ---
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "diff_gpu_probe";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "OHAO Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuProbeContext] vkCreateInstance failed\n");
        return false;
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
    const char* deviceExtensions[] = {
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
    };

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    atomicFloatFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = &atomicFloatFeatures;
    rayQueryFeatures.rayQuery = VK_TRUE;

    // Needed for acceleration structures in Task 6 -- set it now.
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &rayQueryFeatures;
    features12.bufferDeviceAddress = VK_TRUE;

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
        std::fprintf(stderr, "[GpuProbeContext] vkCreateDevice failed (ray query / shader atomic "
                              "float extensions likely unsupported)\n");
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

}  // namespace ohao::diff
